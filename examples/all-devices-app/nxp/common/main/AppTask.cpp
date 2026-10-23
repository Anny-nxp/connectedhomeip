/*
 *
 *    Copyright (c) 2021-2023 Project CHIP Authors
 *    Copyright (c) 2021 Google LLC.
 *    Copyright 2025 NXP
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "AppTask.h"
#include "CHIPDeviceManager.h"
#include "ICDUtil.h"

#include <app/DefaultSafeAttributePersistenceProvider.h>
#include <app/DeviceLoadStatusProvider.h>
#include <app/InteractionModelEngine.h>
#include <app/SafeAttributePersistenceProvider.h>
#include <app/TestEventTriggerDelegate.h>
#include <app/clusters/bindings/BindingManager.h>
#include <app/clusters/bindings/binding-table.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/server/Dnssd.h>
#include <app/server/Server.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <data-model-providers/codedriven/CodeDrivenDataModelProvider.h>
#include <device-factory/DeviceFactory.h>
#include <device/api/Interface.h>
#include <device/api/allocator/ConsecutiveEndpointIdAllocator.h>
#include <device/capabilities/identify/LoggingIdentifyDelegate.h>
#include <device/types/root-node/RootNode.h>
#include <device/types/root-node/RootNodeWith.h>
#if CONFIG_OPENTHREAD
#include <device/types/root-node/features/ThreadFeature.h>
#include <platform/OpenThread/GenericNetworkCommissioningThreadDriver.h>
#elif CONFIG_CHIP_WIFI
#include <device/types/root-node/features/WifiFeature.h>
#endif
#include <platform/DefaultTimerDelegate.h>

#include <platform/DiagnosticDataProvider.h>
#include <platform/KeyValueStoreManager.h>
#include <platform/KvsPersistentStorageDelegate.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifdef ENABLE_CHIP_SHELL
#include <lib/shell/Engine.h>
#include <lib/shell/commands/Help.h>
using namespace chip::Shell;
#endif /* ENABLE_CHIP_SHELL */

using namespace chip;
using namespace chip::app;
using namespace chip::DeviceLayer;
using namespace chip::app::Clusters;

namespace {

// At startup the root node is registered on endpoint 0 and a single application
// device is registered starting at endpoint 1. The device type is read from KVS
// (kDeviceTypeKey, set via the "devtype set <device-type>" CLI command followed by
// a reboot) and falls back to the DeviceFactory default device. This mirrors the
// silabs/esp32 all-devices-app.

// KVS key holding the selected device type (same key as the silabs all-devices-app).
constexpr char kDeviceTypeKey[] = "all-devices/dev-type";
// First endpoint used by the application device.
constexpr EndpointId kDeviceEndpointId = 1;
// Maximum stored device type length, including the terminating NUL.
constexpr size_t kMaxDeviceTypeLength = 64;
// Delay before rebooting after "devtype set", so the CLI output is flushed.
constexpr uint32_t kDevtypeRebootDelayMs = 500;

// Provider-side dependencies. These must outlive the server, so they are static.
Credentials::GroupDataProviderImpl gGroupDataProvider;
DefaultSafeAttributePersistenceProvider gSafeAttributePersistenceProvider;
DefaultTimerDelegate gTimerDelegate;
SimpleTestEventTriggerDelegate gTestEventTriggerDelegate;
// Identify delegate handed to every device created by the DeviceFactory.
LoggingIdentifyDelegate gIdentifyDelegate;

// Persistent storage delegate used by the code-driven data model provider and
// its attribute persistence. It wraps the device-layer KVS manager which is
// initialized during PlatformMgr().InitChipStack() (before InitServer runs), so
// it is valid BOTH before and after chip::Server::Init(). This avoids depending
// on Server::GetPersistentStorage(), which is only valid after Server::Init().
chip::KvsPersistentStorageDelegate gProviderStorage;

chip::app::DefaultAttributePersistenceProvider gProviderAttributePersistence;

// Raw storage for the CodeDrivenDataModelProvider, constructed via placement new
// in GetDataModelProvider(). The destructor is intentionally never called: this
// object has static (process) lifetime on an embedded target and there is no
// orderly shutdown path that would require cleanup.
alignas(chip::app::CodeDrivenDataModelProvider) uint8_t gProviderStorageBuf[sizeof(chip::app::CodeDrivenDataModelProvider)];
chip::app::CodeDrivenDataModelProvider * gProvider = nullptr;

// Compose the root node at build time based on the platform transport, mirroring
// the silabs/zephyr all-devices-app. Thread builds add ThreadFeature, WiFi builds add
// WifiFeature, and any other (no-transport) build falls back to the generic root node.
#if CONFIG_OPENTHREAD
using RootNodeType = chip::app::RootNodeWith<chip::app::ThreadFeature>;
// Thread network driver required by the code-driven NetworkCommissioning cluster.
DeviceLayer::NetworkCommissioning::GenericThreadDriver gThreadDriver;
#elif CONFIG_CHIP_WIFI
using RootNodeType = chip::app::RootNodeWith<chip::app::WifiFeature>;
#else
using RootNodeType = chip::app::RootNode;
#endif

/// Owns the registered devices (root node + application endpoints) and drives
/// their registration into the already-created CodeDrivenDataModelProvider.
class CodeDrivenDataModelDevices
{
public:
    struct Context
    {
        CommissioningWindowManager & commissioningWindowManager;
        DeviceLayer::ConfigurationManager & configurationManager;
        DeviceLayer::DeviceControlServer & deviceControlServer;
        FabricTable & fabricTable;
        Access::AccessControl & accessControl;
        PersistentStorageDelegate & persistentStorage;
        FailSafeContext & failSafeContext;
        DeviceLayer::DeviceInstanceInfoProvider & deviceInstanceInfoProvider;
        DeviceLayer::PlatformManager & platformManager;
        Credentials::GroupDataProvider & groupDataProvider;
        SessionManager & sessionManager;
        DnssdServer & dnssdServer;
        DeviceLoadStatusProvider & deviceLoadStatusProvider;
        DeviceLayer::DiagnosticDataProvider & diagnosticDataProvider;
        TestEventTriggerDelegate * testEventTriggerDelegate;
        Credentials::DeviceAttestationCredentialsProvider & dacProvider;
        EventManagement & eventManagement;
        TimerDelegate & timerDelegate;
        uint16_t minGuaranteedSubscriptionsPerFabric;
#if CONFIG_OPENTHREAD
        DeviceLayer::NetworkCommissioning::ThreadDriver & threadDriver;
#elif CONFIG_CHIP_WIFI
        DeviceLayer::NetworkCommissioning::WiFiDriver & wifiDriver;
#endif
    };

    CodeDrivenDataModelDevices(chip::app::CodeDrivenDataModelProvider & provider, const Context & context) :
        mProvider(provider), mContext(context),
        mRootNode(
            chip::app::RootNode::Context{
                .commissioningWindowManager          = mContext.commissioningWindowManager,
                .configurationManager                = mContext.configurationManager,
                .deviceControlServer                 = mContext.deviceControlServer,
                .fabricTable                         = mContext.fabricTable,
                .accessControl                       = mContext.accessControl,
                .persistentStorage                   = mContext.persistentStorage,
                .failSafeContext                     = mContext.failSafeContext,
                .deviceInstanceInfoProvider          = mContext.deviceInstanceInfoProvider,
                .platformManager                     = mContext.platformManager,
                .groupDataProvider                   = mContext.groupDataProvider,
                .sessionManager                      = mContext.sessionManager,
                .dnssdServer                         = mContext.dnssdServer,
                .deviceLoadStatusProvider            = mContext.deviceLoadStatusProvider,
                .diagnosticDataProvider              = mContext.diagnosticDataProvider,
                .testEventTriggerDelegate            = mContext.testEventTriggerDelegate,
                .dacProvider                         = mContext.dacProvider,
                .eventManagement                     = mContext.eventManagement,
                .timerDelegate                       = mContext.timerDelegate,
                .minGuaranteedSubscriptionsPerFabric = mContext.minGuaranteedSubscriptionsPerFabric,
            }
#if CONFIG_OPENTHREAD
            ,
            chip::app::ThreadFeature::Context{ .threadDriver = mContext.threadDriver }
#elif CONFIG_CHIP_WIFI
            ,
            chip::app::WifiFeature::Context{ .wifiDriver = mContext.wifiDriver }
#endif
        )
    {}

    CHIP_ERROR Startup()
    {
        // Register the root node on endpoint 0. The application device is added
        // afterwards by CreateAndRegisterDevice(), once the DeviceFactory is initialized.
        return mRootNode.Register(kRootEndpointId, mProvider);
    }

    /// Create the selected device type and register it starting at kDeviceEndpointId.
    /// Composed (multi-endpoint) devices allocate consecutive endpoints from there.
    CHIP_ERROR CreateAndRegisterDevice(const std::string & type)
    {
        auto entry = NoHooksDeviceFactory::GetInstance().Create(type);
        VerifyOrReturnError(entry.device != nullptr, CHIP_ERROR_NO_MEMORY);

        ConsecutiveEndpointIdAllocator allocator(kDeviceEndpointId);
        ReturnErrorOnFailure(entry.device->Register(allocator, mProvider));
        VerifyOrDo(!entry.onDeviceRegistered, entry.onDeviceRegistered());
        mDevice = std::move(entry.device);
        ChipLogProgress(AppServer, "all-devices: registered device type '%s'", type.c_str());
        return CHIP_NO_ERROR;
    }

    RootNodeType & RootNodeRef() { return mRootNode; }

private:
    chip::app::CodeDrivenDataModelProvider & mProvider;
    Context mContext;
    RootNodeType mRootNode;

    // Application device (endpoint >= kDeviceEndpointId). Owns its lifetime.
    std::unique_ptr<DeviceInterface> mDevice;
};

/// Returns the device type to instantiate: the KVS-stored type when valid,
/// otherwise the DeviceFactory default device.
std::string SelectDeviceType(PersistentStorageDelegate & storage)
{
    auto & factory         = NoHooksDeviceFactory::GetInstance();
    std::string deviceType = factory.GetDefaultDevice();

    char stored[kMaxDeviceTypeLength] = {};
    uint16_t storedLen                = sizeof(stored);
    if (storage.SyncGetKeyValue(kDeviceTypeKey, stored, storedLen) == CHIP_NO_ERROR && storedLen > 0)
    {
        deviceType.assign(stored, strnlen(stored, storedLen));
    }

    if (!factory.IsValidDevice(deviceType))
    {
        ChipLogError(AppServer, "all-devices: invalid stored device type '%s', using default '%s'", deviceType.c_str(),
                     factory.GetDefaultDevice().c_str());
        deviceType = factory.GetDefaultDevice();
    }
    return deviceType;
}

} // namespace

void AllDevicesApp::AppTask::PreInitMatterStack()
{
    ChipLogProgress(DeviceLayer, "Welcome to NXP all-devices-app (code-driven) Demo App");
}

void AllDevicesApp::AppTask::PostInitMatterStack()
{
    chip::app::InteractionModelEngine::GetInstance()->RegisterReadHandlerAppCallback(&chip::NXP::App::GetICDUtil());
}

chip::app::DataModel::Provider * AllDevicesApp::AppTask::GetDataModelProvider(chip::ServerInitParams & initParams)
{
    // Set the global DAC provider before cluster init so any path that snapshots
    // the provider during construction sees a valid implementation.
    if (Credentials::GetDeviceAttestationCredentialsProvider() == nullptr)
    {
        Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());
    }

    // Construct (once) the code-driven data model provider and register the root-node
    // endpoint into it. This runs BEFORE chip::Server::Init(), so all initialization
    // here must use resources that are valid before Init() (i.e. KVS-based storage,
    // not Server::GetPersistentStorage()). Server-owned references obtained via
    // Server::GetInstance().GetXxx() are stored as references in the Context but NOT
    // accessed until cluster Startup(), which is triggered by Server::Init() internally
    // via dataModelProvider->Startup(). It is therefore safe to call Register() here.
    if (gProvider == nullptr)
    {
        chip::DeviceLayer::PersistedStorage::KeyValueStoreManager & kvsManager = DeviceLayer::PersistedStorage::KeyValueStoreMgr();
        CHIP_ERROR err                                                         = gProviderStorage.Init(&kvsManager);
        VerifyOrReturnValue(err == CHIP_NO_ERROR, nullptr,
                            ChipLogError(AppServer, "all-devices: KVS storage Init failed: %" CHIP_ERROR_FORMAT, err.Format()));

        err = gProviderAttributePersistence.Init(&gProviderStorage);
        VerifyOrReturnValue(
            err == CHIP_NO_ERROR, nullptr,
            ChipLogError(AppServer, "all-devices: attribute persistence Init failed: %" CHIP_ERROR_FORMAT, err.Format()));

        // The group data provider and safe attribute persistence provider are backed
        // by gProviderStorage (KVS), which is valid before Server::Init().
        gGroupDataProvider.SetStorageDelegate(&gProviderStorage);
        gGroupDataProvider.SetSessionKeystore(initParams.sessionKeystore);
        // Publish the app-owned group data provider back into initParams so that
        // chip::Server::Init() adopts THIS instance (mGroupsProvider = initParams.groupDataProvider),
        // instead of the temporary one created by InitializeStaticResourcesBeforeServerInit().
        // This keeps a single group data provider + session keystore across the server,
        // the code-driven clusters and the DeviceFactory (mirrors the ESP32 reference app).
        initParams.groupDataProvider = &gGroupDataProvider;
        if (Credentials::GetGroupDataProvider() == nullptr)
        {
            Credentials::SetGroupDataProvider(&gGroupDataProvider);
        }
        err = gGroupDataProvider.Init();
        VerifyOrReturnValue(
            err == CHIP_NO_ERROR, nullptr,
            ChipLogError(AppServer, "all-devices: GroupDataProvider Init failed: %" CHIP_ERROR_FORMAT, err.Format()));

        err = gSafeAttributePersistenceProvider.Init(&gProviderStorage);
        VerifyOrReturnValue(
            err == CHIP_NO_ERROR, nullptr,
            ChipLogError(AppServer, "all-devices: safe attr persistence Init failed: %" CHIP_ERROR_FORMAT, err.Format()));
        SetSafeAttributePersistenceProvider(&gSafeAttributePersistenceProvider);

        // Publish the provider in gProvider only once every step below succeeded, so a
        // partially initialized provider is never returned.
        auto * provider =
            new (gProviderStorageBuf) chip::app::CodeDrivenDataModelProvider(gProviderStorage, gProviderAttributePersistence);

        DeviceLayer::DeviceInstanceInfoProvider * instanceInfoProvider = DeviceLayer::GetDeviceInstanceInfoProvider();
        VerifyOrReturnValue(instanceInfoProvider != nullptr, nullptr,
                            ChipLogError(AppServer, "all-devices: DeviceInstanceInfoProvider not available"));

#if CONFIG_CHIP_WIFI
        DeviceLayer::NetworkCommissioning::WiFiDriver * wifiDriver = GetWifiDriverInstance();
        VerifyOrReturnValue(wifiDriver != nullptr, nullptr, ChipLogError(AppServer, "all-devices: WiFi driver not available"));
#endif

        auto & server = chip::Server::GetInstance();

        // Construct the devices object and register the root endpoint into the provider.
        // Server resource references (fabricTable, accessControl, etc.) are stored here
        // but not yet accessed -- they will be valid when clusters call Startup() during
        // Server::Init(). Register() only adds descriptors to the provider linked list.
        static CodeDrivenDataModelDevices devices(
            *provider,
            {
                .commissioningWindowManager = server.GetCommissioningWindowManager(),
                .configurationManager       = DeviceLayer::ConfigurationMgr(),
                .deviceControlServer        = DeviceLayer::DeviceControlServer::DeviceControlSvr(),
                .fabricTable                = server.GetFabricTable(),
                .accessControl              = server.GetAccessControl(),
                .persistentStorage          = gProviderStorage,
                .failSafeContext            = server.GetFailSafeContext(),
                .deviceInstanceInfoProvider = *instanceInfoProvider,
                .platformManager            = DeviceLayer::PlatformMgr(),
                .groupDataProvider          = gGroupDataProvider,
                .sessionManager             = server.GetSecureSessionManager(),
                .dnssdServer                = DnssdServer::Instance(),
                .deviceLoadStatusProvider   = *InteractionModelEngine::GetInstance(),
                .diagnosticDataProvider     = DeviceLayer::GetDiagnosticDataProvider(),
                .testEventTriggerDelegate   = &gTestEventTriggerDelegate,
                .dacProvider                = *Credentials::GetDeviceAttestationCredentialsProvider(),
                .eventManagement            = EventManagement::GetInstance(),
                .timerDelegate              = gTimerDelegate,
                .minGuaranteedSubscriptionsPerFabric =
                    InteractionModelEngine::GetInstance()->GetMinGuaranteedSubscriptionsPerFabric(),
#if CONFIG_OPENTHREAD
                .threadDriver = gThreadDriver,
#elif CONFIG_CHIP_WIFI
                .wifiDriver = *wifiDriver,
#endif
            });

        err = devices.Startup();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(AppServer, "all-devices: root node Register failed: %" CHIP_ERROR_FORMAT, err.Format());
            return nullptr;
        }

        // Device factory: Init() only stores references to server resources.
        // Server::GetInstance().GetXxx() returns references (not values), so
        // storing them here is safe even though Server::Init() has not yet run.
        // It must run after the root node is registered because the breadcrumb
        // tracker is the root node's GeneralCommissioning cluster.
        NoHooksDeviceFactory::GetInstance().Init(NoHooksDeviceFactory::Context{
            .groupDataProvider        = gGroupDataProvider,
            .fabricTable              = server.GetFabricTable(),
            .timerDelegate            = gTimerDelegate,
            .storageDelegate          = gProviderStorage,
            .diagnosticDataProvider   = DeviceLayer::GetDiagnosticDataProvider(),
            .platformManager          = DeviceLayer::PlatformMgr(),
            .failSafeContext          = server.GetFailSafeContext(),
            .breadcrumbTracker        = devices.RootNodeRef().GeneralCommissioning(),
            .bindingTable             = Clusters::Binding::Table::GetInstance(),
            .bindingManager           = Clusters::Binding::Manager::GetInstance(),
            .testEventTriggerDelegate = gTestEventTriggerDelegate,
            .identifyDelegate         = gIdentifyDelegate,
        });

        // Register the selected application device (KVS "devtype" or factory default).
        const std::string deviceType = SelectDeviceType(gProviderStorage);
        err                          = devices.CreateAndRegisterDevice(deviceType);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(AppServer, "all-devices: device '%s' Register failed: %" CHIP_ERROR_FORMAT, deviceType.c_str(),
                         err.Format());
            return nullptr;
        }

        gProvider = provider;
    }

    return gProvider;
}

void AllDevicesApp::AppTask::PostInitMatterServerInstance(void)
{
    // All initialization (GroupDataProvider, SafeAttributePersistence,
    // root-node registration, DeviceFactory and application device) was completed in
    // GetDataModelProvider() before Server::Init(). Nothing additional
    // is required after Server::Init().
}

#ifdef ENABLE_CHIP_SHELL
namespace {

void PrintSupportedDeviceTypes()
{
    streamer_printf(streamer_get(), "Supported device types:\r\n");
    for (const auto & deviceType : NoHooksDeviceFactory::GetInstance().SupportedDeviceTypes())
    {
        streamer_printf(streamer_get(), "  - %s\r\n", deviceType.c_str());
    }
}

CHIP_ERROR DevtypeSetHandler(int argc, char ** argv)
{
    if (argc != 1)
    {
        streamer_printf(streamer_get(), "Usage: devtype set <device-type>\r\n");
        PrintSupportedDeviceTypes();
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    const char * deviceType       = argv[0];
    const size_t deviceTypeLength = strlen(deviceType);
    if (deviceTypeLength >= kMaxDeviceTypeLength)
    {
        streamer_printf(streamer_get(), "Device type too long (max %u characters)\r\n",
                        static_cast<unsigned>(kMaxDeviceTypeLength - 1));
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    if (!NoHooksDeviceFactory::GetInstance().IsValidDevice(deviceType))
    {
        streamer_printf(streamer_get(), "Invalid device type: %s\r\n", deviceType);
        PrintSupportedDeviceTypes();
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    CHIP_ERROR err = gProviderStorage.SyncSetKeyValue(kDeviceTypeKey, deviceType, static_cast<uint16_t>(deviceTypeLength));
    if (err != CHIP_NO_ERROR)
    {
        streamer_printf(streamer_get(), "Failed to save device type to KVS: %" CHIP_ERROR_FORMAT "\r\n", err.Format());
        return err;
    }

    streamer_printf(streamer_get(), "Device type set to: %s. Rebooting...\r\n", deviceType);

    // The data model is built once at boot, so a reboot is required to apply the new device type.
    return DeviceLayer::SystemLayer().StartTimer(
        System::Clock::Milliseconds32(kDevtypeRebootDelayMs),
        [](System::Layer *, void *) { DeviceLayer::PlatformMgrImpl().Reset(); }, nullptr);
}

CHIP_ERROR DevtypeGetHandler(int argc, char ** argv)
{
    char stored[kMaxDeviceTypeLength] = {};
    uint16_t storedLen                = sizeof(stored);
    CHIP_ERROR err                    = gProviderStorage.SyncGetKeyValue(kDeviceTypeKey, stored, storedLen);
    if (err != CHIP_NO_ERROR || storedLen == 0)
    {
        streamer_printf(streamer_get(), "No stored device type (using default: %s)\r\n",
                        NoHooksDeviceFactory::GetInstance().GetDefaultDevice().c_str());
        return CHIP_NO_ERROR;
    }

    streamer_printf(streamer_get(), "Current device type: %.*s\r\n", static_cast<int>(strnlen(stored, storedLen)), stored);
    return CHIP_NO_ERROR;
}

CHIP_ERROR DevtypeListHandler(int argc, char ** argv)
{
    PrintSupportedDeviceTypes();
    return CHIP_NO_ERROR;
}

Engine sDevtypeSubShell;

CHIP_ERROR DevtypeDispatch(int argc, char ** argv)
{
    if (argc == 0)
    {
        sDevtypeSubShell.ForEachCommand(PrintCommandHelp, nullptr);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR err = sDevtypeSubShell.ExecCommand(argc, argv);
    if (err != CHIP_NO_ERROR)
    {
        streamer_printf(streamer_get(), "Error: %" CHIP_ERROR_FORMAT "\r\n", err.Format());
    }
    return err;
}

} // namespace
#endif /* ENABLE_CHIP_SHELL */

void AllDevicesApp::AppTask::AppMatter_RegisterCustomCliCommands(void)
{
#ifdef ENABLE_CHIP_SHELL
    static const shell_command_t sDevtypeSubCommands[] = {
        { &DevtypeSetHandler, "set", "Usage: devtype set <device-type>" },
        { &DevtypeGetHandler, "get", "Usage: devtype get" },
        { &DevtypeListHandler, "list", "Usage: devtype list" },
    };
    sDevtypeSubShell.RegisterCommands(sDevtypeSubCommands, MATTER_ARRAY_SIZE(sDevtypeSubCommands));

    // Root "devtype" command (same as the silabs/esp32 all-devices-app). "device" is
    // already used by the SDK (src/lib/shell/commands/Device.cpp).
    static const shell_command_t sDevtypeCommand = { &DevtypeDispatch, "devtype", "Device type management commands" };
    Engine::Root().RegisterCommands(&sDevtypeCommand, 1);
#endif /* ENABLE_CHIP_SHELL */
}

// This returns an instance of this class.
AllDevicesApp::AppTask & AllDevicesApp::AppTask::GetDefaultInstance()
{
    static AllDevicesApp::AppTask sAppTask;
    return sAppTask;
}

chip::NXP::App::AppTaskBase & chip::NXP::App::GetAppTask()
{
    return AllDevicesApp::AppTask::GetDefaultInstance();
}
