# Migración a RISC-V — Documentación e Índice

> **Alcance de este conjunto de documentos.** Esta carpeta materializa las Fases
> 1–9 del *Plan Maestro* (`Plan_Firmware_CubeSat_Luckfox_Referencia.md`):
> estudio del proyecto CubeSat, estudio del SDK Luckfox como **referencia
> técnica**, documentación diferenciada de ambos, y el **diseño** de la
> migración de los drivers de Linux (Cortex-A7) al coprocesador RISC-V
> (RT-Thread). **No contiene cambios de código.** La implementación (Fases
> 10–12) está pendiente de aprobación explícita.

## Separación de proyectos (regla del plan)

| Proyecto | Ubicación | Rol |
|----------|-----------|-----|
| **CubeSat (este repo)** | `/home/heikki/Documents/pwncube-sdk` | Firmware de la computadora de vuelo. Es lo que se desarrolla. |
| **SDK Luckfox** | `/home/heikki/Documents/luckfox-pico` | **Solo referencia técnica** del RV1106. No se modifica. |

Todas las citas a archivos de la forma `sysdrv/...`, `project/...` se refieren al
**SDK Luckfox**. Las citas `src/...`, `dts/...`, `pkg/...` se refieren a **este
repo CubeSat**.

## Documentos

| # | Documento | Fase del plan | Contenido |
|---|-----------|---------------|-----------|
| 10 | [`10-referencia-luckfox-riscv.md`](10-referencia-luckfox-riscv.md) | 2, 4 | Cómo Luckfox compila, arranca, carga y mapea en memoria el firmware RISC-V (HPMCU / RT-Thread). |
| 20 | [`20-referencia-luckfox-ipc.md`](20-referencia-luckfox-ipc.md) | 8 | Implementación real de IPC ARM↔RISC-V: RPMsg-Lite, Mailbox, Shared Memory, vrings, interrupciones. |
| 30 | [`30-drivers-cubesat.md`](30-drivers-cubesat.md) | 1, 5 | Inventario de los drivers Linux existentes: lógica portable vs. *glue* de Linux, buses, GPIO. |
| 40 | [`40-diseno-migracion.md`](40-diseno-migracion.md) | 6, 7 | Diseño de la migración por driver y arquitectura objetivo Linux↔RISC-V. |
| 50 | [`50-protocolo-ipc-y-roadmap.md`](50-protocolo-ipc-y-roadmap.md) | 9, 10–12 | Protocolo IPC versionado, servicios, CCSDS y orden de implementación. |
| 60 | [`60-paso0-ipc-bringup.md`](60-paso0-ipc-bringup.md) | 10 (Paso 0) | Estado de implementación del transporte IPC: hecho/verificado vs. pendiente en placa, mapa de memoria y co-diseño. |
| 70 | [`70-ipc-mailbox-loopback-test.md`](70-ipc-mailbox-loopback-test.md) | 10 (Paso 0) | Test de loopback del mailbox: B2A funciona, A2B ilegible por el MCU, shared-memory DDR coherente. |
| 80 | [`80-arranque-dual-a7-mcu.md`](80-arranque-dual-a7-mcu.md) | 10 (Paso 0) | **Arranque dual FUNCIONANDO**: mapa de memoria definitivo, la cadena de 5 bugs y sus fixes, flasheo correcto (UF, nunca `DI -b`), diagnóstico por marcadores y limpieza pendiente. |
| 90 | [`90-mcu-configuracion-y-replicacion.md`](90-mcu-configuracion-y-replicacion.md) | 10-11 | **DOS SERVICIOS VALIDADOS EN PLACA**: guía definitiva de configuración del MCU — arranque registro a registro, reglas de memoria, BSP, transporte rpmsg, patrón de periféricos (gotchas HAL_SPI_Stop y clock-antes-de-registro), driver compartido, **RadioService** SX1262×2 (SPI, protocolo + paquetes/RX-continuo/eventos DIO1 por instancia, §7bis) y **SensorService** BME280+ICM-42670 (I²C0, §7ter), ruteo de endpoints con dos canales, build/flash, marcadores y checklist de replicación. |

## Resumen ejecutivo — hallazgos que condicionan el diseño

Estos cuatro hechos, verificados en el código, son las restricciones duras de la
migración (detalle y citas en los documentos 10/20):

1. **El RISC-V es un Syntacore SCR1, `rv32imc`/`ilp32`.** Toolchain
   `riscv-none-embed-gcc 10.2.0`. El artefacto es `rtthread.bin`, cargado en
   `0x40000`.
   → *(SDK)* `sysdrv/source/mcu/rt-thread/bsp/rockchip/rv1106-mcu/rtconfig.py:3,28,38`.

2. **Memoria muy ajustada.** La RAM del MCU son **240 KB** (`link.lds`:
   `ORIGIN=0x40000, LENGTH=0x3c000`). La SRAM compartida documentada para el
   HPMCU son **8 KB** (`hpmcu_sram@0xff6fe000`). Cualquier driver migrado debe
   caber en este presupuesto junto a RT-Thread.
   → *(SDK)* `.../rv1106-mcu/link.lds:4-6`; `sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi` (nodo `hpmcu_sram`).

3. **SPI en el MCU es solo HAL, no está habilitado; y SPI0 lo posee Linux.**
   `RT_USING_SPI` está desactivado en `rtconfig.h`, pero existen `hal_spi.c` y
   `drv_spi.c`. El SX1262 (Fase 10, prioridad) es SPI → requiere habilitar SPI en
   el MCU **y** transferir la propiedad del controlador SPI desde Linux.
   → *(SDK)* `.../rv1106-mcu/rtconfig.h:98`; HAL `.../common/hal/lib/hal/src/hal_spi.c`.

4. **El stack de IPC existe en el SDK pero NO está cableado para el RV1106.**
   - El driver Linux `rockchip_rpmsg.c` solo declara `rk3562`/`rk3568` en su tabla
     de compatibles (no `rv1106`).
   - No existe directorio de *porting* `platform/RV1106/` para RPMsg-Lite (se usa
     el de `RK3568` como plantilla).
   - No hay nodo `rpmsg` ni `reserved-memory` para vrings en el device tree del
     RV1106.
   - **Sí** existen el hardware Mailbox (`@0xff5c0000`, habilitado) y la librería
     RPMsg-Lite completa.
   → *(SDK)* `sysdrv/source/kernel/drivers/rpmsg/rockchip_rpmsg.c` (tabla de match);
     `.../rpmsg-lite/lib/.../porting/platform/RK3568/`.

**Consecuencia de diseño.** La migración no es "recompilar el driver para
RISC-V". Es: (a) habilitar/portar periféricos en el MCU, (b) **cablear** el canal
RPMsg para RV1106 en ambos lados (kernel match + porting + device tree), y (c)
reescribir la *glue* de Linux de cada driver sobre la HAL del MCU, reutilizando la
lógica de hardware portable. El orden propuesto (doc 50) empieza por habilitar el
transporte IPC y SPI, antes de tocar el primer sensor.
