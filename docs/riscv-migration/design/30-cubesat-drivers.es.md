# Drivers CubeSat existentes — inventario para migración

> **Naturaleza del documento.** Estudio de **este repo CubeSat**
> (`/home/heikki/Documents/pwncube-sdk`). Rutas relativas a este repo. Cubre las
> Fases 1 y 5 del plan: catalogar los drivers Linux funcionales y separar la
> **lógica de hardware portable** del *glue* específico de Linux, base de la
> migración (Fase 6). No se modifica código.

Documentación de usuario existente y complementaria: `docs/sx1262.md`,
`docs/bme280.md`, `docs/icm42670.md`. Device tree: `dts/rv1106g-sdk.dts` (incluye
`dts/rv1106-sdk-ipc.dtsi`). Empaquetado: `pkg/available/{sx1262,icm42670}`.

## Criterio: portable vs. glue

- **Portable (reutilizable en RISC-V):** secuencias de registros del chip, máquinas
  de estado, fórmulas de calibración/escalado, codificación de parámetros. No
  dependen del kernel.
- **Glue de Linux (a reescribir):** acceso SPI/I²C (spi core / regmap), GPIO
  (`gpiod_*`), IRQ (`request_irq`), char device / IIO / sysfs, device tree probe,
  primitivas de sincronización y `devm_*`.

---

## 1. SX1262 (×2) — LoRa sobre SPI

**Fuentes:** `src/sx1262-kmod/` (módulo `sx1262.ko`) y `src/sx1262-cli/` (CLI).

| Archivo | Rol |
|---------|-----|
| `sx1262_core.c` (1–194) | `spi_register_driver`, char device (`alloc_chrdev_region`, `cdev_add`), probe por radio. **[glue]** |
| `sx1262_hal.c` (1–178) | Transferencias SPI (`spi_write`, `spi_write_then_read`, `spi_sync`), GPIO (`gpiod_*`), IRQ DIO1, switch de antena. **[glue + lógica de pines]** |
| `sx1262_cmd.c` (1–769) | Set de comandos: registros, FIFO, frecuencia, TX/RX, modulación, calibración, IRQ. **[mayormente portable]** |
| `sx1262_chardev.c` (1–304) | `file_operations` (open/ioctl/read/write/poll), máquina TX/RX. **[glue]** |
| `sx1262.h` (1–82) | IOCTLs (`SX1262_IOCTL_*`, magic 'L'), struct de dispositivo, máx. 2. **[interfaz]** |
| `sx1262_regs.h` (1–130) | Opcodes (0x00–0x1E), mapa de registros (0x0740–0x08F9), máscaras IRQ. **[portable]** |
| `../sx1262-cli/sx1262_cli.c` (1–340) | CLI: abre `/dev/sx1262-{0,1}`, despacha por ioctl/read/write. **[reubicar como cliente IPC]** |

**Dependencias Linux:** spi core (`linux/spi/spi.h`), GPIO consumer
(`linux/gpio/consumer.h`), IRQ (`devm_request_irq`, `IRQF_TRIGGER_RISING`), char
device (`cdev`, `class_create`), sync (`completion`, `mutex`, `wait_queue`),
`devm_kzalloc`, device tree (`semtech,sx1262`).

**Portable (alto valor de reutilización):** construcción de tramas opcode+addr+datos
(p.ej. leer registro `[0x1D, addr_hi, addr_lo, 0,0]`), secuencias FIFO,
inicialización completa del chip (`sx1262_init`: standby → regulador → packet type →
calibración → PA → frecuencia → TX params → modulación → IRQ), conversión Frf
(`freq*(1<<25)/32e6`), codificación de timeouts (15.625 µs/tick), SF/BW/CR, parsing
de IRQ. **Particularidades de hardware ya resueltas** (ver `docs/sx1262.md` y memoria
del proyecto): **XTAL 32 MHz (NO TCXO)** → DIO3-as-TCXO NO se configura; **switch de
antena externo por GPIO** (TX: ANT_SW0=1/ANT_SW1=0; RX: 0/1), no por DIO2; erratas
TxClamp (0x08D8) y TxModulation (0x0889).

**Glue a reescribir sobre la HAL del MCU:** SPI (`spi_sync` → `HAL_SPI_*`/`drv_spi`),
GPIO (`gpiod_*` → `HAL_GPIO_*`/`rt_pin_*`), IRQ DIO1 (`request_irq` → `rt_pin_attach_irq`
o polling de BUSY), e interfaz al exterior (char device/ioctl → comandos IPC; doc 50).

### Configuración de bus / pines (de `dts/rv1106-sdk-ipc.dtsi`)

| Señal | SX1262 #0 (SPI0) | SX1262 #1 (SPI1) |
|-------|------------------|------------------|
| Bus / clk máx | SPI0 / 8 MHz | SPI1 / 8 MHz |
| CLK/MOSI/MISO | GPIO1_C1 / C2 / C3 (SPI0m0) | GPIO4_A7 / A1 / A0 (SPI1m0) |
| CS | GPIO1_C0 | GPIO1_C4 (a confirmar) |
| RST | GPIO3_A6 | GPIO1_C6 |
| BUSY | GPIO3_A7 | GPIO1_C7 |
| DIO1 (IRQ) | GPIO3_A3 | GPIO1_D1 |
| ANT_SW0 / SW1 | GPIO0_A3 / A4 | GPIO1_C5 / D0 |
| Pinctrl group | `sx1262_dev0_pins` | `sx1262_dev1_pins` |

Nodos: `rv1106-sdk-ipc.dtsi` ~`:256-273` (#0), ~`:275-292` (#1). Defaults: 868 MHz,
LoRa SF7/BW125/CR4-5, 14 dBm, RX al arranque, IRQ TX/RX_DONE+TIMEOUT→DIO1.

> **Bloqueante de migración (ver doc 10 §3, doc 40):** SPI no está habilitado en el
> MCU (`RT_USING_SPI` off) y SPI0 lo posee Linux en el DT. Migrar el SX1262 exige
> habilitar SPI en RT-Thread y reasignar el/los controladores SPI al RISC-V.

---

## 2. BME280 — T/P/H sobre I²C

**Driver:** mainline **`bmp280`** del kernel (in-tree, `CONFIG_BMP280=y`), **sin .ko
propio**. Fuentes en `src/kernel/drivers/iio/pressure/bmp280-*.c`. Config en
`configs/kernel/rv1106_minimal_defconfig`.

**Bus:** I²C0 m0 — SCL **GPIO1_A3**, SDA **GPIO1_A4**, 400 kHz, **compartido con el
ICM-42670**. Dirección **0x76** (SDO a GND; alt. 0x77). Chip ID 0x60 @ reg 0xD0.
Nodo DT `bme280@76` (`compatible="bosch,bme280"`), `rv1106-sdk-ipc.dtsi` ~`:320-329`.

**Dependencias Linux:** i2c core, **regmap** (`regmap_*`), framework **IIO**
(`iio_dev`, canales, `read_raw`), sysfs (`/sys/bus/iio/.../in_{temp,pressure,humidityrelative}_input`).

**Portable:** WHO_AM_I, lectura de coeficientes de calibración (layout no trivial,
26 bytes, mezcla signed/unsigned), **algoritmo de compensación Bosch** (punto fijo),
soft reset (0xE0=0xB6) y espera de recarga NVM, mapa de registros (calib 0x88–0xA1,
ctrl 0xF2–0xF5, datos 0xF7–0xFE).

**Glue:** i2c_driver/probe → I²C de la HAL; regmap → acceso directo; IIO/sysfs →
exportación por IPC (doc 50).

> **Favorable a migración:** el MCU **sí** tiene I²C (HAL + `drv_i2c`, habilitado
> I2C4 por defecto; doc Luckfox MCU). Hay que rutar I²C0 (los pines del CubeSat) al
> MCU y arbitrar el bus si Linux también lo usa.

---

## 3. ICM-42670-P — IMU 6 ejes sobre I²C

**Fuentes:** `src/icm42670-kmod/icm42670.c` (1–262), módulo `icm42670.ko`. IIO
custom (el mainline `inv_icm42600` es otra familia). Empaquetado
`pkg/available/icm42670/`.

**Bus:** I²C0 m0 (mismo que BME280), 400 kHz. Dirección **0x68** (AD0 forzado a 0 vía
**gpio-hog** en `&gpio0`, RK_PA5; `rv1106-sdk-ipc.dtsi` ~`:349-356`). Chip ID 0x67 @
WHO_AM_I 0x75. **INT1 = GPIO0_A1** (level-high; declarado en DT pero sin uso en este
MVP). Nodo `icm42670@68` (`compatible="invensense,icm42670p"`) ~`:338-356`.

**Dependencias Linux:** i2c core, regmap, IIO, device tree (interrupt + gpio-hog).

**Portable:** soft reset (bit4 de 0x02, espera 2–3 ms), WHO_AM_I, configuración
(PWR_MGMT0 0x1F low-noise, ACCEL_CONFIG0 0x21, GYRO_CONFIG0 0x20, espera 50 ms),
lectura big-endian 16-bit (ACCEL 0x0B–0x10, GYRO 0x11–0x16, TEMP 0x09), **escalado en
punto fijo** (accel 4788400 nm/s²/LSB @±16g, gyro 1065264 nrad/s/LSB @±2000 dps, temp
1000/128 m°C/LSB con offset +3200). FS ±16 g / ±2000 dps, ODR 100 Hz.

**Glue:** i2c_driver/regmap/IIO/sysfs → I²C de la HAL + exportación por IPC; gpio-hog
de AD0 → fijar el pin desde el firmware MCU (o mantenerlo en DT de Linux antes del
arranque).

---

## 4. Tabla resumen — portable vs glue

| Driver | Bus | Lógica portable | Glue Linux | Habilitado en MCU hoy |
|--------|-----|-----------------|------------|------------------------|
| **SX1262 ×2** | SPI 8 MHz | Tramas SPI, init, calibración, codificación radio, parsing IRQ | spi core, gpiod, IRQ, chardev/ioctl, DT | **SPI: NO** (solo HAL; SPI0 de Linux) |
| **BME280** | I²C0 400 kHz | Calib + compensación Bosch, mapa de registros | i2c, regmap, IIO/sysfs, DT | **I²C: SÍ** (HAL+drv_i2c) |
| **ICM-42670** | I²C0 400 kHz | Reset/config, lectura BE, escalado punto fijo | i2c, regmap, IIO/sysfs, gpio-hog, IRQ | **I²C: SÍ** |

## 5. Puntos a confirmar

- CS real de SPI1 del SX1262 #1 (`GPIO1_C4` vs `GPIO4_A2`) — trazar grupos en
  `dts/rv1106-pinctrl.dtsi`.
- DIO2/DIO3 del SX1262 no se usan (switch externo + XTAL hardwired) — confirmado en
  código; mantener en la versión MCU.
- Codificación ODR 100 Hz del ICM (0x09 vs 0x08 según tabla del datasheet).
- Si I²C0 (pines del CubeSat) puede asignarse al MCU sin romper a Linux (arbitraje de
  bus / propiedad exclusiva).
