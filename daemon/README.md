# Limelight Systemcore EtherCAT MainDevice Daemon

This directory contains a C++17 ARM64 Linux daemon for Limelight Systemcore hardware. It runs SOEM as an EtherCAT MainDevice, reports NT4 telemetry under `/EtherCAT/`, and records WPILog events for AdvantageScope.

## Build

```sh
cmake -S daemon -B build/daemon -DLIMELIGHT_EC_WITH_WPILIB=ON
cmake --build build/daemon
```

The default CMake path selects `aarch64-linux-gnu-gcc` and `aarch64-linux-gnu-g++` when configured from a non-ARM64 host. Use `-DCMAKE_TOOLCHAIN_FILE=...` to provide a site-specific toolchain instead.

For local syntax checks on machines without WPILib libraries:

```sh
cmake -S daemon -B build/daemon-local -DLIMELIGHT_EC_WITH_WPILIB=OFF
```

## Configuration

Install the JSON configuration at:

```text
/etc/ec-configuration/ec-configuration.json
```

Each enabled entry maps a logical name such as `LEDs` to a physical Linux interface such as `eth1`. Logs and NT4 values use the logical name. Interfaces `eth0`, `wlan0`, and `usb0` are blocked unless `allow_restricted_interfaces` is set to `true`.

## IPC Packet

The Unix domain socket is:

```text
/var/run/ethercat_maindevice.sock
```

It uses `SOCK_SEQPACKET`, non-blocking mode, and this packed header:

```cpp
struct IpcPacketHeader {
  uint32_t magic;             // 0x4C4C4543
  uint16_t version;           // 1
  uint16_t type;              // 1 writes output bytes
  uint32_t sequence;
  uint16_t logicalNameBytes;
  uint16_t reserved;
  uint32_t payloadBytes;
};
```

The header is followed by the logical interface name bytes and then the binary payload. Packet type `1` copies payload bytes into the configured output image for that logical interface.
