# RV1106 SDK

SDK mínimo para Rockchip RV1106 (Cortex-A7). Basado en Luckfox Pico SDK V1.4.

Board: RV1106 SDK (SPI NAND, 256 MB)

## Componentes

| Componente | Versión |
|------------|---------|
| Kernel | Linux 5.10.160 |
| U-Boot | 2017.09 + rkbin |
| Busybox | 1.27.2 |
| Toolchain | GCC 8.3, uClibc, ARMv7-a hard-float |
| Rootfs | ext4 (también soporta squashfs, ubifs, initramfs) |

## Requisitos

```bash
sudo apt-get install -y git make gcc gcc-multilib g++ g++-multilib \
    gawk texinfo libssl-dev bison flex fakeroot cmake unzip gperf \
    autoconf device-tree-compiler libncurses5-dev pkg-config bc python3 cpio rsync
```

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
./build.sh uboot     # Solo U-Boot
./build.sh kernel    # Solo kernel
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

En `docs/`: `toolchain.md`, `uboot.md`, `kernel.md`, `rootfs.md`, `packaging.md`, `pkg-system.md`
- Bilingüe EN/ES: `./docs/switch.sh` para cambiar idioma, `./docs/switch.sh en|es` para fijar idioma

## Licencia

Scripts: MIT · Kernel: GPL-2.0 · U-Boot: GPL-2.0 · Busybox: GPL-2.0 · Toolchain: GPL-3.0/LGPL-2.1 · rkbin: Propietaria Rockchip
