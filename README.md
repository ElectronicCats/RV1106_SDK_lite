# RV1106 SDK — CubeSat flight-computer firmware

SDK para Rockchip RV1106, computadora de vuelo CubeSat. Arranque **dual**:
Cortex-A7 (Linux, misión) + coprocesador RISC-V SCR1 "HPMCU" (RT-Thread, control
de hardware determinista). El MCU posee las radios **SX1262 ×2 (SPI)** y los
sensores **BME280 + ICM-42670 (I²C0)** y los expone a Linux por rpmsg
(`radio_test` / `sensor_test`). Basado en Luckfox Pico SDK V1.4.

Board: RV1106 SDK (SPI NAND, 256 MB)

> **Clon-y-compila:** el repo es autocontenido — ambos toolchains (ARM y
> RISC-V) y todo el código fuente (kernel, U-Boot, rkbin, RT-Thread BSP) están
> versionados. Tras clonar, `./build.sh` produce `update.img` sin descargas.

> 🚀 **¿Primera vez?** Sigue el tutorial desde cero (instalar dependencias →
> clonar → compilar → flashear): [`docs/getting-started.md`](docs/getting-started.md).

## Componentes

| Componente | Versión |
|------------|---------|
| Kernel | Linux 5.10.160 |
| U-Boot | 2017.09 + rkbin |
| MCU firmware | RT-Thread (Syntacore SCR1, rv32imc) — `rtthread.bin` |
| Busybox | 1.27.2 |
| Toolchain ARM | GCC 8.3, uClibc, ARMv7-a hard-float (vendored) |
| Toolchain RISC-V | xpack riscv-none-embed-gcc 10.2.0 (vendored) |
| Rootfs | ext4 (también soporta squashfs, ubifs, initramfs) |

## Requisitos

Instala todas las dependencias del host con **un solo comando**:

```bash
./build.sh deps        # = ./scripts/install-deps.sh (apt, Debian/Ubuntu)
```

Instala: `git make gcc g++ bc cpio rsync fakeroot bison flex libssl-dev
device-tree-compiler scons gawk texinfo cmake unzip gperf autoconf
libncurses5-dev pkg-config python3` (los toolchains ARM y RISC-V ya vienen
versionados). `scons` es imprescindible para el firmware del MCU (RT-Thread).

## Uso

```bash
# Configurar toolchain (una vez por terminal)
source scripts/00-setup-toolchain.sh

# Build completo desde cero
./build.sh

# Rebuild completo (clean + build)
./build.sh rebuild

# Ayuda
./build.sh help

# Componentes individuales
./build.sh uboot     # Solo U-Boot (embebe rtthread.bin del MCU)
./build.sh kernel    # Solo kernel
./build.sh mcu       # Solo firmware RISC-V del MCU (rtthread.bin)
./build.sh rootfs    # Solo rootfs + paquetes
./build.sh pack      # Solo empaquetar update.img
./build.sh flash     # Flashear al dispositivo (sudo)
./build.sh clean     # Limpiar todo
./build.sh info      # Ver configuración

# Gestión de paquetes
./pkg/pkg.sh list                    # Paquetes disponibles
./pkg/pkg.sh enable dropbear         # Habilitar SSH
./pkg/pkg.sh build-all               # Compilar habilitados
./pkg/pkg.sh menuconfig              # TUI whiptail
```

## Actualización del firmware (paso a paso)

Actualizar es **compilar → empaquetar → cargar**. Puedes recompilar un solo
componente o todo, pero la imagen que se flashea es siempre `update.img`.

### 1. Compilar cada componente (individual)

```bash
source scripts/00-setup-toolchain.sh   # una vez por terminal

./build.sh mcu       # firmware RISC-V del MCU (rtthread.bin)
./build.sh uboot     # U-Boot (idblock, uboot, trust)
./build.sh kernel    # kernel + DTB + boot.img
./build.sh rootfs    # busybox + paquetes + rootfs
```

> ⚠️ **Orden que importa:** `uboot` **embebe** `rtthread.bin` dentro de
> `uboot.img`/`trust.img`. Si tocas el MCU, corre **siempre** `./build.sh mcu`
> **antes** de `./build.sh uboot`, o la placa arrancará con el MCU viejo.
> Los paquetes se compilan tras el kernel; si editas un paquete, reconstrúyelo
> con `./pkg/pkg.sh build-all` antes de `rootfs`.

### 2. Compilar todo junto

```bash
./build.sh            # orden correcto: mcu → uboot → kernel → paquetes → rootfs → pack
./build.sh rebuild    # igual pero con clean previo
```

### 3. Juntarlo en la imagen final

```bash
./build.sh pack       # combina todos los output/images/*.img en update.img
```

`./build.sh` (completo) ya hace este paso al final. Solo necesitas `pack`
suelto si recompilaste un componente individual y quieres reempaquetar sin
rebuild completo.

### 4. Entrar en modo bootloader (maskrom) y cargar

En la placa:

1. **Mantén presionado el botón `BOOT`.**
2. Sin soltar `BOOT`, **presiona y suelta `RST` (reset)**.
3. Sigue sosteniendo `BOOT` ~**5 s** hasta que el host detecte el dispositivo
   en modo maskrom; entonces suelta `BOOT`.

**¿Cómo sé que está en maskrom?** Verifícalo desde el host:

```bash
sudo tools/upgrade_tool LD        # lista los dispositivos Rockchip conectados
# Maskrom OK →  DevNo=1  Vid=0x2207,Pid=0x350a,...  Mode=Maskrom

lsusb | grep 2207                 # alternativa: Rockchip = VID 0x2207
# ID 2207:350a  → maskrom;  ID 2207:110a  → ya en modo Loader (U-Boot)
```

Si `LD` no lista nada o `lsusb` no muestra el `2207:xxxx`, la placa **no**
entró en maskrom: repite la secuencia del botón. `Mode=Maskrom` (o `Loader`)
es la única confirmación fiable antes de flashear.

Con la placa ya en Linux, puedes entrar sin tocar botones con `reboot loader`
desde el shell serie.

Luego, desde el host:

```bash
./build.sh flash                              # = upgrade_tool UF output/images/update.img (sudo)
# o directo:
sudo tools/upgrade_tool UF output/images/update.img
```

> ⚠️ Usa **siempre `UF`** (imagen completa). `DI -b` responde "ok" pero **no
> escribe** en esta SPI-NAND. Si flasheaste con el botón de recovery, **suéltalo
> apenas empiece el flasheo** o el reboot posterior queda mudo.

Detalle registro-a-registro del arranque, arranque dual A7+MCU y todos los
gotchas: [`docs/migration/implementation/90-mcu-config-replication.md`](docs/migration/implementation/90-mcu-config-replication.md)
y [`docs/migration/implementation/80-dual-boot.md`](docs/migration/implementation/80-dual-boot.md).

## Outputs

Tras `./build.sh` en `output/images/`:

| Archivo | Descripción |
|---------|-------------|
| `idblock.img` | Boot loader (DDR init + SPL) |
| `download.bin` | Raw loader para Rockchip tools |
| `uboot.img` | U-Boot proper |
| `trust.img` | OP-TEE + HPMCU |
| `boot.img` | Kernel FIT (Image + DTB) |
| `env.img` | Entorno U-Boot (particiones + bootargs) |
| `rootfs_base.img` | Rootfs |
| `update.img` | Imagen completa flasheable |

## Particionado (SPI NAND — 256 MB)

```
256K(env), 512K@256K(idblock), 256K@768K(uboot), 32M@1024K(boot), -(rootfs)
```

## Documentación

Toda la documentación está en `docs/` (bilingüe EN/ES). Empieza por el índice
[`docs/README.md`](docs/README.md). Cambia de idioma con `./docs/switch.sh en|es`
(o sin argumento para alternar).

- **Arquitectura** — `docs/architecture/`: visión general, IPC rpmsg, y propiedad de
  periféricos (qué posee el MCU RISC-V vs Linux).
- **Build** — `docs/build/`: `toolchain`, `uboot`, `kernel`, `rootfs`, `packaging`, `pkg-system`.
- **Periféricos** (los posee el MCU, manejados por rpmsg) — `docs/peripherals/`:
  `sx1262-radio`, `bme280`, `icm42670`.
- **Seguridad** — `docs/security/exploitation-guide.md`.
- **Migración RISC-V / firmware del MCU** (registro del bring-up, arranque dual,
  configuración registro a registro) — `docs/migration/`.

## Licencia

Scripts: MIT · Kernel: GPL-2.0 · U-Boot: GPL-2.0 · Busybox: GPL-2.0 · Toolchain: GPL-3.0/LGPL-2.1 · rkbin: Propietaria Rockchip
