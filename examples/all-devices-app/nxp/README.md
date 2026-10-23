# CHIP NXP All-Devices Application

## Overview

The all-devices-app is a code-driven Matter application: its data model is
built at runtime with the `CodeDrivenDataModelProvider` instead of a ZAP file.
The root node (endpoint 0) is composed with the network feature of the
platform (Wi-Fi or Thread), and one device type from the all-devices-app
device factory is registered on endpoint 1.

The device type is selected at runtime with the `devtype` Matter shell command
and applied on the next boot. See
[all-devices-app](../README.md) for the list of supported device types.

### Supported platforms

| NXP platform | Network | Dedicated readme                                                             |
| ------------ | ------- | ---------------------------------------------------------------------------- |
| RW61x        | Wi-Fi   | [NXP RW61x (FreeRTOS) Guide](../../../docs/platforms/nxp/nxp_rw61x_guide.md) |
| RT1060       | Wi-Fi   | [NXP RT1060 Guide](../../../docs/platforms/nxp/nxp_rt1060_guide.md)          |
| RT1170       | Wi-Fi   | [NXP RT1170 Guide](../../../docs/platforms/nxp/nxp_rt1170_guide.md)          |
| MCXW72       | Thread  | [NXP MCXW72 Guide](../../../docs/platforms/nxp/nxp_mcxw72_guide.md)          |

## Environment Setup, Building, and Testing

Refer to the
[CHIP NXP Examples Guide for FreeRTOS platforms](../../../docs/platforms/nxp/nxp_examples_freertos_platforms.md)
for environment setup. The application is built with `build_examples.py`, for
example:

```sh
./scripts/build/build_examples.py --target nxp-rw61x-freertos-all-devices-wifi-frdm build
./scripts/build/build_examples.py --target nxp-rt1060-freertos-all-devices-wifi-evkc-iwx12 build
./scripts/build/build_examples.py --target nxp-rt1170-freertos-all-devices-wifi-iwx12 build
./scripts/build/build_examples.py --target nxp-mcxw72-freertos-all-devices-thread-frdm build
```

## Selecting the device type

The selected device type is stored in KVS. Without a stored value, the default
device type of the device factory is used.

```sh
matterCli> devtype list
matterCli> devtype set on-off-light
matterCli> devtype get
```

`devtype set` stores the device type and reboots the device to apply it.
