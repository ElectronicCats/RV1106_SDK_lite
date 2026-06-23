# Guía de Empaquetado de Firmware Rockchip

[English](packaging.en.md)

## Resumen

El firmware del RV1106 se empaqueta como una imagen `update.img` de Rockchip.
El proceso es:

```
afptool -pack  →  rkImageMaker -RK1106  →  update.img
```

`afptool` ensambla las imágenes de partición individuales en un blob de
firmware, luego `rkImageMaker` antepone el encabezado Rockchip y el bootloader
para producir la imagen final flasheable.

## Diseño de Particiones

Definido en `configs/board/rv1106-sdk.mk` mediante `RK_PARTITION_CMD_IN_ENV`:

```
mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)
```

| Partición | Offset    | Tamaño | Descripción                     |
|-----------|-----------|--------|----------------------------------|
| env       | 0         | 256K   | Entorno U-Boot (MTD)             |
| idblock   | 256K      | 512K   | IDB (loader)                     |
| uboot     | 768K      | 256K   | U-Boot                           |
| boot      | 1024K     | 32M    | Kernel + DTB + initramfs         |
| rootfs    | a continuación | resto | Sistema de archivos raíz       |

> **Nota:** La partición `env` debe ser de **256K** para coincidir con
> `CONFIG_ENV_NAND_SIZE` en la configuración de U-Boot.

## Referencia de Herramientas

| Herramienta    | Ubicación                     | Propósito                                |
|----------------|-------------------------------|------------------------------------------|
| `rkImageMaker` | `tools/rkImageMaker`          | Envuelve el blob con el bootloader       |
| `afptool`      | `tools/afptool`               | Empaqueta / desempaqueta particiones     |
| `mkenvimage`   | `tools/mkenvimage`            | Genera env.img desde env.txt             |
| `upgrade_tool` | `tools/upgrade_tool`          | Flashea update.img al dispositivo        |
| `boot_merger`  | `tools/boot_merger`           | Fusiona U-Boot + TEE en download.bin     |
| `loaderimage`  | `tools/loaderimage`           | Convierte imágenes a formato loader      |

## Creación de env.img

La imagen de entorno se crea desde un archivo `.env.txt` usando `mkenvimage`:

```bash
tools/mkenvimage -s 262144 -p 0x0 -o output/images/env.img output/images/.env.txt
```

- `-s 262144` — tamaño de la partición env en bytes (256K)
- `-p 0x0` — byte de relleno (0x00)
- Formato de entrada: líneas `clave=valor`
- La cadena de partición y `sys_bootargs` son escritas por
  `scripts/04-pack-image.sh`

Ejemplo de `.env.txt`:

```
mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)
sys_bootargs=root=/dev/mtdblock4 rootfstype=ext4
```

## Flasheo

### Actualización completa

```bash
sudo tools/upgrade_tool UF output/images/update.img
```

### Particiones individuales

```bash
sudo tools/upgrade_tool DI -env output/images/env.img
sudo tools/upgrade_tool DI -boot output/images/boot.img
sudo tools/upgrade_tool DI -rootfs output/images/rootfs.img
sudo tools/upgrade_tool DI -idblock output/images/idblock.img
```

### Modo Boot ROM (Maskrom)

1. Mantener presionado el botón de recovery / Maskrom
2. Encender la placa
3. Verificar detección:

   ```bash
   sudo tools/upgrade_tool LD
   ```

4. Flashear como arriba

## Estructura de Archivos del SDK

```
configs/board/rv1106-sdk.mk     — diseño de particiones y configuración
scripts/04-pack-image.sh        — script de empaquetado
tools/                          — herramientas (rkImageMaker, afptool, etc.)
output/images/                  — imágenes de partición y update.img
```

## Solución de Problemas

### bad CRC

La CRC de la partición de entorno es inválida — probablemente la partición env
no fue formateada o el tamaño es incorrecto. Re-flashear `env.img` o borrar
env:

```bash
sudo tools/upgrade_tool EF output/images/update.img
```

### MMC timeout / Sin respuesta

La placa no está en modo de descarga. Verificar:
- Conexión del cable USB
- Alimentación de la placa
- Detección Rockusb (`tools/upgrade_tool LD`)
- Reingresar al modo Maskrom

### mkimage no encontrado

La herramienta `mkimage` falta en el árbol de compilación de U-Boot o en el
PATH del sistema.

```bash
export PATH=$PATH:/ruta/a/u-boot/tools
```

O usar la herramienta precompilada en `tools/mkimage`.

### Dispositivo Rockusb no detectado

```bash
lsusb | grep 2207
```

Los dispositivos Rockchip tienen VID `2207`. Si no aparece nada:
- Verificar permisos del driver (Linux: regla `udev` para `2207`)
- Probar otro puerto USB o cable
- Reingresar al modo Maskrom

---

[English](packaging.en.md)
