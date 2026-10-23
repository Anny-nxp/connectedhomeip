/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2021-2023 Google LLC.
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

#pragma once

#if CONFIG_APP_FREERTOS_OS
#include "AppTaskFreeRTOS.h"
#else
#include "AppTaskZephyr.h"
#endif

#include <app/data-model-provider/Provider.h>

namespace AllDevicesApp {
#if CONFIG_APP_FREERTOS_OS
class AppTask : public chip::NXP::App::AppTaskFreeRTOS
#else
class AppTask : public chip::NXP::App::AppTaskZephyr
#endif
{
public:
    ~AppTask() override {};
    void PreInitMatterStack(void) override;
    void PostInitMatterStack(void) override;

    /**
     * Called at the end of AppTaskBase::InitServer, i.e. AFTER chip::Server::Init().
     * All initialization (GroupDataProvider, SafeAttributePersistence, DeviceFactory,
     * and root-node registration) is performed in GetDataModelProvider() before
     * Server::Init(). This override is intentionally empty.
     */
    void PostInitMatterServerInstance(void) override;

    /**
     * Code-driven data model provider hook. Builds and populates a
     * CodeDrivenDataModelProvider (root node + a fixed set of endpoint devices)
     * and returns it to AppTaskBase::InitServer, replacing the ZAP/codegen provider.
     */
    chip::app::DataModel::Provider * GetDataModelProvider(chip::ServerInitParams & initParams) override;

    /**
     * Registers the application-specific "devtype" CLI command (set/get/list).
     * "devtype set <device-type>" stores the device type in KVS and reboots; the
     * selected device is registered on endpoint 1 at the next boot.
     * Only compiled when the CHIP shell is enabled.
     */
    void AppMatter_RegisterCustomCliCommands(void) override;

    // This returns an instance of this class.
    static AppTask & GetDefaultInstance();
};
} // namespace AllDevicesApp

/**
 * Returns the application-specific implementation of the AppTaskBase object.
 *
 * Applications can use this to gain access to features of the AppTaskBase
 * that are specific to the selected application.
 */
chip::NXP::App::AppTaskBase & GetAppTask();
