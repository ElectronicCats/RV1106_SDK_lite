# IPC — rpmsg between Linux (A7) and the MCU (RISC-V)

Linux (Cortex-A7) and the MCU (SCR1, RT-Thread) talk over **rpmsg-lite** (virtio vrings +
hardware mailbox). Linux is the master; the MCU announces **named endpoints**. All services
share one rpmsg-lite instance, and a single poll thread on the MCU (`ping_echo.c`) ticks every
service in turn.

See also: [`overview.md`](overview.md), [`peripheral-ownership.md`](peripheral-ownership.md).

## Services and endpoints

| Service | Channel name | Endpoint | Responsibility | Linux client |
|---|---|---|---|---|
| PingEcho | `rpmsg-ap3-ch0` | 0x4004 | Link bring-up + echo; hosts the poll thread that ticks all services | — |
| **RadioService** | `rpmsg-radio` | 0x4005 | Both SX1262 (the `instance` byte selects radio 0/1): init, freq, power, TX, RX, modulation, packet, sync. Pushes `EVT_RX` / `EVT_RX_TIMEOUT`. | `radio_test` |
| **SensorService** | `rpmsg-sensor` | 0x4006 | BME280 (@0x76) + ICM-42670 (@0x68) on I²C0: `PING`, `WHOAMI`, `BME280_READ`, `IMU_READ` | `sensor_test` |
| **TelemetryService** | `rpmsg-telemetry` | 0x4007 | Downlink control (start/stop/status/config/monitor); the worker itself runs in CommandService | `radio_test tlm` |
| **CommandService** | `rpmsg-command` | 0x4008 | Uplink RX on radio 0 + telecommand dispatch; downlink TX on radio 1. Pushes `EVT_TC_RX` (0xE3). | `radio_test cmd_* / tcsend` |

## Message pattern

Request byte 0 is the command; byte 1 is the `instance` (radio 0/1) for RadioService; the
payload follows. Responses are synchronous; **events** (`EVT_RX`, `EVT_TC_RX` = 0xE3) are
pushed asynchronously from the MCU when a packet arrives, and the Linux client `poll()`s for
them (e.g. `radio_test cmd_listen` / `cmd_watch`).

## Transport shape

- rpmsg-lite payload is **≤ 496 B**; larger messages are chunked.
- Measured on this SoC: **B2A** (MCU→A7) works directly, **A2B** (A7→MCU) is not readable by
  the MCU by interrupt, so the MCU **polls the vrings** from its thread. Details:
  [`../migration/reference/20-luckfox-ipc.md`](../migration/reference/20-luckfox-ipc.md) and
  [`../migration/implementation/70-mailbox-loopback-test.md`](../migration/implementation/70-mailbox-loopback-test.md).
- On Linux the endpoints appear under `/dev/rpmsg*`; `radio_test` opens destinations 0x4005,
  0x4007 and 0x4008.

## Protocol design and bring-up

- Design / command map: [`../migration/design/50-ipc-protocol-roadmap.md`](../migration/design/50-ipc-protocol-roadmap.md)
- Bring-up story: [`../migration/implementation/60-ipc-bringup.md`](../migration/implementation/60-ipc-bringup.md)
