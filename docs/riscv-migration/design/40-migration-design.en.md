# Migration design

> **Nature of the document.** Design, **prior to implementation**.
> Based on docs 10/20/30. Decides, per driver, what is reused and what is
> adapted, and fixes the target Linux↔RISC-V architecture.

## 1. Target architecture

```
 ┌──────────────────────── Linux / Cortex-A7 ────────────────────────┐
 │  Software de misión · Telemetría · Telecomandos · CCSDS · Storage  │
 │  ───────────────────────────────────────────────────────────────  │
 │  Clientes de servicio (reemplazan a la CLI/sysfs actuales):        │
 │    radio_client · sensor_client · config_client · event_client     │
 │  ───────────────────────────────────────────────────────────────  │
 │  Capa de transporte:  /dev/rpmsg* (rockchip_rpmsg)                 │
 └───────────────────────────────┬───────────────────────────────────┘
                                  │  RPMsg / Mailbox / vrings (doc 20)
 ┌───────────────────────────────┴───────────────────────────────────┐
 │  RT-Thread / RISC-V (HPMCU)                                        │
 │  Dispatcher IPC (tabla cmd→handler, patrón rpmsg_cmd)              │
 │  ───────────────────────────────────────────────────────────────  │
 │  RadioService · SensorService · ConfigurationService · EventSvc    │
 │  ───────────────────────────────────────────────────────────────  │
 │  Drivers portados:  sx1262 (SPI) · bme280 (I²C) · icm42670 (I²C)   │
 │  HAL Rockchip:  HAL_SPI · HAL_I2C · HAL_GPIO · HAL_PINCTRL         │
 └───────────────────────────────┬───────────────────────────────────┘
                                  │
                            Hardware (SX1262 ×2, BME280, ICM-42670)
```

**Distribution of responsibilities:**
- **Linux:** mission logic, CCSDS, storage, scheduling, events.
  **Never** accesses the hardware directly after the migration.
- **RT-Thread:** deterministic hardware control (radio, SPI, I²C, GPIO, sensors).
  Does **not** implement mission logic.

**Porting principle (by design: reuse the maximum):** for each driver the
**portable hardware logic** from doc 30 is kept (ideally in files
`*_cmd.c`/`*_regs.h` almost untouched) and **only** the access layer (SPI/
I²C/GPIO/IRQ) and the external interface (char dev/IIO → IPC) are replaced. Pattern: a thin *Portability
Interface* (driver HAL) that on Linux maps to `spi_sync`/`regmap`/
`gpiod_*` and on RT-Thread to `HAL_SPI`/`drv_i2c`/`HAL_GPIO`. Thus the same `*_cmd.c`
compiles on both sides.

## 2. IPC transport (prerequisite for everything)

Before the first driver, RPMsg must be **wired** for the RV1106 (doc 20 §8):

1. Kernel: add `rv1106` to the match table of `rockchip_rpmsg.c`.
2. Linux device tree: `rpmsg` node + `reserved-memory` (64 KB/instance) tied to
   `mailbox@0xff5c0000`.
3. RT-Thread: port `platform/RV1106/` for RPMsg-Lite (real IRQs and vring
   addresses of the RV1106).
4. Minimal MCU firmware: reduced `rv1106-mcu` BSP + `rpmsg_cmd` dispatcher that
   responds to a `PING`/`ECHO` command.
5. Integrate into the CubeSat `build.sh`: `mcu` target (RISC-V toolchain + SCons) and
   package `rtthread.bin` as `LOADER2=Hpmcu` in the rkbin flow.

**Acceptance criterion for the IPC transport:** from Linux, send a message over
`/dev/rpmsg*` and receive the echo from the RISC-V. Without this, no driver is
migrated.

## 3. Design per driver

### 3.1 SPI (infrastructure, before the SX1262)

| Aspect | Decision |
|---------|----------|
| Linux dependencies | spi core (`spi_sync`, `spi_write_then_read`). |
| Reusable | The byte sequences are already in `sx1262_cmd.c` (they are not Linux-specific). |
| RT-Thread adaptation | Enable `RT_USING_SPI`; use `drv_spi.c` + `HAL_SPI_*`. Define bus/CS via IOMUX (`HAL_PINCTRL_SetIOMUX`). |
| HW ownership | **Reassign SPI0 (and SPI1) from the A7 to the RISC-V**: remove `&spi0/&spi1` from the Linux DT and configure them in the MCU `board/iomux.c`. |
| IPC interface | None direct: SPI is internal to the firmware; the services use it. |
| Risk | SPI footprint and throughput from SCR1; verify `HAL_SPI_ItTransfer`/DMA. |

### 3.2 SX1262 (×2) — RadioService

| Aspect | Decision |
|---------|----------|
| Reusable (high) | `sx1262_cmd.c` and `sx1262_regs.h` almost untouched (init, calibration, Frf, modulation, IRQ). Keep the particularities: **XTAL not TCXO**, **ANT_SW via GPIO**, 0x08D8/0x0889 errata. |
| Adapt | `sx1262_hal.c`: `spi_*`→`HAL_SPI`, `gpiod_*`→`HAL_GPIO/rt_pin`, DIO1 IRQ→`rt_pin_attach_irq` (or BUSY polling). `sx1262_core/_chardev` are replaced by the service + IPC dispatcher. |
| API visible from Linux | IPC commands of the **RadioService** (reset, set_freq, set_power, set_modem, tx, rx, get_status, read/write_reg, set_antsw, get_irq) — mirror of the current `SX1262_IOCTL_*` (doc 50). |
| Two instances | Two driver contexts (SPI0/SPI1 + their own GPIOs), one `instance_id` in the IPC protocol. |
| Risk | DIO1 as MCU IRQ (map pin↔IRQ in SCR1); TX/RX_DONE latency via IPC for the mission logic. |

### 3.3 BME280 — SensorService

| Aspect | Decision |
|---------|----------|
| Reusable | Bosch compensation + register map (port from `bmp280-core.c`, or reimplement the algorithm, which is public and self-contained). |
| Adapt | regmap/i2c → `drv_i2c`/`HAL_I2C`; IIO/sysfs → IPC commands `read_sensor`. |
| API visible from Linux | `SensorService`: `read(bme280)` → {temp_m°C, pres_kPa, hum_%RH}. |
| Bus | Route I²C0 (GPIO1_A3/A4) to the MCU; arbitrate with ICM-42670 (same bus). |
| Risk | I²C0 ownership between A7 and MCU; if the bme280 is in-tree on Linux, stop instantiating it in the Linux DT once migrated. |

### 3.4 ICM-42670 — SensorService

| Aspect | Decision |
|---------|----------|
| Reusable | Reset/config + BE reading + **fixed-point scaling** (from `icm42670.c`). |
| Adapt | regmap/i2c → `drv_i2c`/`HAL_I2C`; IIO → IPC; **AD0 gpio-hog** → fix pin in the MCU `board/iomux.c`. |
| API visible | `SensorService`: `read(icm42670)` → {ax,ay,az, gx,gy,gz, temp}. INT1 (GPIO0_A1) optional for a future *streaming* mode. |
| Bus | Shares I²C0 with BME280 → a single I²C bus driver on the MCU, two devices. |

## 4. Code portability strategy (how to reuse the maximum)

The proposal, **without reorganizing the project**, is to add next to each
driver a thin portability layer, e.g.:

```
src/sx1262-kmod/
  sx1262_cmd.c     sx1262_regs.h      <- PORTABLE: sin cambios o casi
  sx1262_port.h                       <- NUEVO: macros SPI/GPIO/delay/log
  sx1262_port_linux.c                 <- glue actual (extraído de _hal.c/_core.c)
  sx1262_port_rtt.c                   <- NUEVO: glue RT-Thread (HAL_SPI/GPIO)
```

`sx1262_cmd.c` would call `sx_spi_xfer()`, `sx_gpio_set()`, `sx_delay_ms()` defined
in `sx1262_port.h`, with two implementations interchangeable at compile time. Same
pattern for BME280/ICM. This maximizes shared code and keeps the Linux driver
working during the transition.

## 5. Hardware ownership caveats (critical)

There is no automatic HW arbiter between the A7 and RISC-V for SPI/I²C/GPIO: **each peripheral
must have a single owner**, fixed by configuration:
- What migrates to the MCU is **removed from the Linux device tree** (or marked `disabled`)
  so that Linux does not claim it.
- The pins/IOMUX of those peripherals are configured in the MCU firmware `board/iomux.c`.
- For occasional shared data there is `HAL_SPINLOCK` (seen in `shmem_ipc_test`),
  but the main model is: hardware → RISC-V; data → Linux via IPC.

## 6. Open decisions for the user (before implementing)

1. **SPI/I²C ownership:** are SPI0/SPI1 and I²C0 ceded entirely to the RISC-V (Linux
   loses direct access, by design)? This implies editing the CubeSat DT.
2. **Initial scope:** implement only the IPC transport (PING/ECHO) first and
   validate it in hardware before migrating the SX1262?
3. **RPMsg strategy:** reuse the RK3568 porting assuming compatibility, or
   create `platform/RV1106/` from scratch (safer, more work)?
4. **Coexistence:** keep the Linux drivers operational in parallel during the
   migration (portability layer §4) or migrate "all at once"?

The implementation roadmap and the concrete IPC protocol are in
doc 50.
