#
#   Copyright (c) 2025-2026 Project CHIP Authors
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
#

# ****************************************************************
# Matter all-devices-app (code-driven) for NXP FreeRTOS platforms
# ****************************************************************

cmake_minimum_required(VERSION 3.30)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Retrieve CHIP_ROOT if provided
if (DEFINED CHIP_ROOT)
    get_filename_component(CHIP_ROOT ${CHIP_ROOT} ABSOLUTE)
else()
    get_filename_component(CHIP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../ REALPATH)
endif()

# Export CHIP_ROOT as an environment variable to be recognized by Kconfig
set(COMMON_KCONFIG_ENV_SETTINGS CHIP_ROOT=${CHIP_ROOT})

get_filename_component(GEN_DIR ${CHIP_ROOT}/zzz_generated/ REALPATH)
get_filename_component(ALL_DEVICES_COMMON_DIR ${CHIP_ROOT}/examples/all-devices-app/all-devices-common REALPATH)
get_filename_component(NXP_EXAMPLE_DIR ${CHIP_ROOT}/examples/all-devices-app/nxp/common REALPATH)
get_filename_component(EXAMPLE_PLATFORM_NXP_COMMON_DIR ${CHIP_ROOT}/examples/platform/nxp/common REALPATH)
get_filename_component(NXP_MATTER_SUPPORT_DIR ${CHIP_ROOT}/third_party/nxp/nxp_matter_support REALPATH)

# Device factory device selection and generation of app_config/enabled_devices.h.
# enabled_devices.cmake defines ALL_DEVICES_DEVICE_SOURCES (the list of device
# implementation source files for the enabled set) and generates the header
# app_config/enabled_devices.h into ${CMAKE_CURRENT_BINARY_DIR}.
#
# By default, ALL_DEVICES_ENABLED_DEVICES is not set and enabled_devices.cmake
# enables all supported device types. To restrict the build to a specific subset
# (e.g. to reduce binary size or limit the DeviceFactory to exactly the endpoints
# instantiated by AppTask), define ALL_DEVICES_ENABLED_DEVICES before the
# include(), for example:
#
#   set(ALL_DEVICES_ENABLED_DEVICES
#       on-off-light
#       dimmable-light
#       occupancy-sensor
#       contact-sensor
#   )
#
# Each entry must match a device-type name recognised by enabled_devices.cmake.
include(${ALL_DEVICES_COMMON_DIR}/device-factory/enabled_devices.cmake)

# Add the CHIP stack & NXP port libs as an external module
list(APPEND EXTRA_MCUX_MODULES ${CHIP_ROOT}/config/nxp/chip-cmake-freertos)

# Add secure element lib as an external module
list(APPEND EXTRA_MCUX_MODULES ${CHIP_ROOT}/third_party/simw-top-mini/repo/matter)

# If CONF_FILE_NAME is provided, add the full path in front of CONF_FILE
if (DEFINED CONF_FILE_NAME)
    set(CONF_FILE ${CHIP_ROOT}/examples/platform/nxp/config/${CONF_FILE_NAME} ${CONF_FILE})
endif()

# Add application prj.conf in front of CONF_FILE
set(CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/prj.conf ${CONF_FILE})

set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)

# Load the MCUX SDK package
find_package(McuxSDK 3.0.0)

# Get compiler flags and includes and pass them to CHIP target
list(APPEND CONFIG_CHIP_EXTERNAL_TARGETS McuxSDK)

if (NOT DEFINED NXP_SDK_RECONFIG_CMAKE_DIR)
    get_filename_component(NXP_SDK_RECONFIG_CMAKE_DIR ${NXP_MATTER_SUPPORT_DIR}/examples/platform/${CONFIG_CHIP_NXP_PLATFORM_FAMILY} REALPATH)
endif()

# Include the nxp_sdk_reconfig.cmake to customize the SDK for Matter application
include(${NXP_SDK_RECONFIG_CMAKE_DIR}/nxp_sdk_reconfig.cmake)

# Define application project name
project(chip-nxp-all-devices-app-example)

# Add application definitions, include paths and source files.
# NOTE: this is a code-driven application (no ZAP data model). The data model
# is built at runtime by the AppTask via CodeDrivenDataModelProvider, so we do
# NOT call chip_configure_data_model()/ZAP_FILE here.
include(${CHIP_ROOT}/src/app/chip_data_model.cmake)

# Code-driven application: skip Ember-dependent cmd_server.cpp that
# app_common.cmake would otherwise add when CONFIG_CHIP_APP_CLI is enabled.
set(CHIP_DATA_MODEL_CODE_DRIVEN_SKIP_EMBER_SHELL_CMDS ON)

# Add NXP common application files
include(${EXAMPLE_PLATFORM_NXP_COMMON_DIR}/app_common.cmake)

# Code-driven application: there are no ZAP-generated headers. Instruct the
# app-specific config.h to skip the zap-generated/gen_config.h and
# zap-generated/endpoint_config.h includes.
target_compile_definitions(app
    PRIVATE
    CHIP_CONFIG_SKIP_APP_SPECIFIC_GENERATED_HEADER_INCLUDES=1
    # This is a code-driven application: the shared NXP AppTaskBase must not
    # reference the ZAP/codegen data model provider or the codegen-coupled
    # NetworkCommissioning/Identify cluster registrations.
    CONFIG_CHIP_DATA_MODEL_CODE_DRIVEN=1
)

target_include_directories(app
    PRIVATE
    ${NXP_EXAMPLE_DIR}/main/include
    ${ALL_DEVICES_COMMON_DIR}
    ${GEN_DIR}/app-common
    # Generated app_config/enabled_devices.h (produced by enabled_devices.cmake).
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(app
    PRIVATE
    ${NXP_EXAMPLE_DIR}/main/main.cpp
    ${NXP_EXAMPLE_DIR}/main/AppTask.cpp
    ${NXP_EXAMPLE_DIR}/main/DeviceCallbacks.cpp

    # Code-driven data model provider sources. The NXP chip GN library does not
    # currently link the codedriven source_set (only referenced by tests), so
    # compile it directly into the application.
    ${CHIP_ROOT}/src/data-model-providers/codedriven/CodeDrivenDataModelProvider.cpp
    ${CHIP_ROOT}/src/data-model-providers/codedriven/endpoint/EndpointInterfaceRegistry.cpp
    ${CHIP_ROOT}/src/data-model-providers/codedriven/endpoint/SpanEndpoint.cpp
    ${CHIP_ROOT}/src/app/server-cluster/ServerClusterInterfaceRegistry.cpp
    ${CHIP_ROOT}/src/app/server-cluster/SingleEndpointServerClusterRegistry.cpp
    ${CHIP_ROOT}/src/app/persistence/DefaultAttributePersistenceProvider.cpp

    # all-devices-common code-driven device implementations.
    # The DeviceFactory registers a creator for every supported device type in its
    # constructor, which ODR-uses every device class. All device implementation
    # sources must therefore be compiled in. Use the authoritative source list
    # exported by enabled_devices.cmake instead of hand-listing a subset.
    ${ALL_DEVICES_DEVICE_SOURCES}

    # Cluster sources required by enabled device types but not included in the
    # default NXP SDK build (e.g. Binding cluster for on-off-light-switch).
    ${ALL_DEVICES_CLUSTER_SOURCES}
)

if (CONFIG_CHIP_WIFI AND CONFIG_OPENTHREAD)
    message(FATAL_ERROR "WiFi + Thread (Border Router) configuration is not supported in this application.")
elseif(CONFIG_CHIP_ETHERNET)
    message(FATAL_ERROR "Ethernet configuration is not supported in this application.")
endif()

# Platform-specific root-node network feature and its NetworkCommissioning cluster.
# The AppTask composes the root node at build time as RootNodeWith<ThreadFeature> or
# RootNodeWith<WifiFeature> based on the platform transport (see RootNodeType in
# AppTask.cpp). The matching feature source and its clusters are compiled in here.
# These features are not part of ALL_DEVICES_DEVICE_SOURCES (which excludes
# platform-specific root-node features) and must be added explicitly.
if (CONFIG_OPENTHREAD)
    target_sources(app
        PRIVATE
        # ThreadFeature registers the code-driven NetworkCommissioning cluster on the
        # root node (the ZAP NetworkCommissioning::Instance is excluded under
        # CONFIG_CHIP_DATA_MODEL_CODE_DRIVEN).
        ${ALL_DEVICES_COMMON_DIR}/device/types/root-node/features/ThreadFeature.cpp
        ${CHIP_ROOT}/src/app/clusters/network-commissioning/NetworkCommissioningCluster.cpp
        ${CHIP_ROOT}/src/app/clusters/network-commissioning/WifiScanResponse.cpp
        ${CHIP_ROOT}/src/app/clusters/network-commissioning/ThreadScanResponse.cpp
    )
elseif (CONFIG_CHIP_WIFI)
    target_sources(app
        PRIVATE
        # WifiFeature registers the code-driven NetworkCommissioning + WiFiNetworkDiagnostics
        # clusters on the root node (the ZAP NetworkCommissioning::Instance is excluded
        # under CONFIG_CHIP_DATA_MODEL_CODE_DRIVEN).
        ${ALL_DEVICES_COMMON_DIR}/device/types/root-node/features/WifiFeature.cpp
        ${CHIP_ROOT}/src/app/clusters/network-commissioning/NetworkCommissioningCluster.cpp
        ${CHIP_ROOT}/src/app/clusters/network-commissioning/WifiScanResponse.cpp
        ${CHIP_ROOT}/src/app/clusters/network-commissioning/ThreadScanResponse.cpp
        ${CHIP_ROOT}/src/app/clusters/wifi-network-diagnostics-server/WiFiNetworkDiagnosticsCluster.cpp
    )
endif()
