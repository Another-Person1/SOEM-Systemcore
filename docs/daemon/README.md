# ec-systemcore EtherCAT MainDevice Daemon

This directory contains a C++17 ARM64 Linux daemon for Limelight Systemcore hardware. It runs SOEM as an EtherCAT MainDevice, reports NT4 telemetry under the `ec-systemcore` table, and records WPILog events for AdvantageScope.

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
/etc/ethercat/ec-configuration.json
```

The dashboard-compatible schema is:

```json
{
  "allow_restricted_interfaces": false,
  "log_directory": "/var/log/ethercat",
  "log_count_limit": 10,
  "free_space_threshold_mb": 50,
  "interface_mappings": [
    {
      "logical_name": "LED_Trunk",
      "physical_interface": "eth1"
    }
  ]
}
```

Each entry maps a logical name such as `LED_Trunk` to a physical Linux interface such as `eth1`. Logs and NT4 values use the logical name. Interfaces `eth0`, `wlan0`, and `usb0` are blocked unless `allow_restricted_interfaces` is set to `true`. When the config file changes, the daemon exits cleanly so systemd can restart it with the new settings.

## IPC Packet

The Unix domain socket is:

```text
/var/run/ec-systemcore.sock
```

It uses `SOCK_SEQPACKET`, non-blocking mode, and broadcasts this exact 128-byte packed status packet every 200 ms:

```cpp
struct [[gnu::packed]] MainDeviceStatus {
  uint8_t  daemon_status;
  uint8_t  maindevice_state;
  uint8_t  active_adapters;
  uint8_t  subdevice_count;
  uint16_t active_faults;
  uint16_t maindevice_jitter_us;
  uint32_t lost_frames;
  char     interface_name[16];
  char     logical_name[16];
  uint8_t  reserved[84];
};
```

Incoming dashboard commands must be exactly this 64-byte packed packet:

```cpp
struct [[gnu::packed]] SubDeviceCommand {
  uint8_t  command_type;
  uint8_t  target_subdevice;
  uint8_t  target_port;
  uint8_t  payload_length;
  uint8_t  payload_data[60];
};
```

Command types `0x01` and `0x02` copy payload bytes into the process output image for operational buses. Command type `0x03` resets lost-frame counters.

WPILog files are written under `log_directory` using `ec_log_YYYYMMDD_HHMMSS.wpilog`.
