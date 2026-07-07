# Bring-up del transporte IPC (estado y receta)

> Estado de implementación del **transporte RPMsg A7↔RISC-V**.
> Distingue lo **verificado (compila)** de lo que queda para la **sesión en placa**.
> Decisiones tomadas: ceder buses al RISC-V; porting `platform/RV1106/` propio;
> MCU sin consola (verificación por IPC); avanzar el bring-up en software.

## 1. Hecho y verificado (compila)

**Base de firmware MCU**:
- Árbol RT-Thread vendorizado en `src/mcu/`; toolchain `toolchain/riscv/` (`riscv-none-embed-gcc 10.2.0`).
- Board mínima `src/mcu/.../rv1106-mcu/board/pwncube/` (sin cámara/ISP; **sin uart2** — Linux posee la consola `ttyFIQ0`).
- `scripts/06-build-mcu.sh` + `./build.sh mcu` → `output/mcu/rtthread.bin` (~20 KB).

**Porting rpmsg-lite RISC-V** (la pieza más difícil — compila):
- `src/mcu/.../rpmsg-lite/lib/include/platform/RV1106/rpmsg_config.h` — IRQ de mailbox RV1106 (un solo par AP/BB, no 8 por canal).
- `.../include/platform/RV1106/rpmsg_platform.h` — copia agnóstica de RK3568.
- `.../rpmsg_lite/porting/platform/RV1106/rpmsg_platform.c` — **reescrito para RISC-V**:
  sin SMP/afinidad/`HAL_CPU_TOPOLOGY`, `MBOX0`→`MBOX`, IRQ único `MAILBOX0_BB_IRQn`,
  `cpsid/cpsie`→`rt_hw_interrupt_disable/enable`, `platform_in_isr`→`rt_interrupt_get_nest`.
- Fix portabilidad: `.../rpmsg-lite/lib/include/rpmsg_compiler.h` — `MEM_BARRIER()` usa
  `fence` (RISC-V) en vez de `dsb` (ARM) bajo `#if defined(__riscv)`.
- Selección de build: `.../rpmsg-lite/SConscript` y `.../common/drivers/Kconfig`
  (`RT_USING_RPMSG_LITE` `depends on ... || SOC_RV1106`).

> **`RT_USING_RPMSG_LITE` queda DESACTIVADO por defecto** en `board/pwncube/defconfig`
> para mantener la base compilando hasta validar el enlace + comportamiento en placa.

**Datos de la placa (leídos por serie, no destructivo):**
- DDR = **256 MB** en `0x00000000–0x0fffffff` (todo es System RAM; sin `reserved-memory`).
- Consola Linux = `ttyFIQ0` (uart2). `/dev/ttyUSB0` @115200 da shell root. sudo `1334`.

## 2. Mapa de memoria compartida (propuesta a fijar en placa)

Reservar **1 MB en el tope de la DDR** para los vrings RPMsg, idéntico en ambos lados:

| Símbolo | Valor propuesto | Notas |
|---------|-----------------|-------|
| Base región compartida | `0x0FF00000` | Tope de 256 MB. **Verificar que no colisiona con OP-TEE/trust.** |
| Tamaño | `0x00100000` (1 MB) | Cubre 1 instancia (2 vrings × 0x8000 = 0x10000) con holgura. |

- **Linux:** nodo `reserved-memory` `no-map` en `0x0FF00000` + `reg` del nodo `rpmsg`.
- **MCU:** símbolos de linker `__linux_share_rpmsg_start__/__end__` en `src/mcu/.../rv1106-mcu/link.lds`
  apuntando a la **misma** dirección (NO la SRAM 0x40000; es DDR).
- `VRING_ALIGN=0x1000` obligatorio (requisito de Linux).

## 3. Parámetros que DEBEN coincidir en ambos lados (co-diseño en placa)

| Parámetro | Propuesta | Riesgo / a resolver |
|-----------|-----------|---------------------|
| `link-id` | `0x04` (kernel) ↔ encoding MCU | Con `RL_GET_R_CPU_ID(0x04)=4` el índice de canal **se sale** del array de 4 canales del MCU. Reconciliar a un canal válido 0–3. |
| Canal mailbox | rx=ch0 / tx=ch3 (kernel) ↔ `RL_RV1106_MBOX_CHAN=0` (MCU) | Kernel usa 2 canales por dirección; rpmsg-lite usa 1 canal bidireccional (A2B/B2A). **Mismatch a resolver.** |
| Dirección A2B/B2A | MCU = remoto (B2A envía, recibe A2B) | Validar semántica de `HAL_MBOX` en hardware. |
| Nombre de canal NS | `rpmsg-ap3-ch0` | El driver in-kernel `rockchip_rpmsg_test.c` liga por este nombre exacto → el MCU debe `rpmsg_ns_announce("rpmsg-ap3-ch0")`. |
| `RL_RPMSG_MAGIC` | `0x524D5347` ("RMSG") | Ya igual en ambos. ✔ |

## 4. Pendiente — lado Linux (receta lista, sin aplicar)

1. **Kernel match** `src/kernel/drivers/rpmsg/rockchip_rpmsg.c`: añadir `RV1106` al enum
   (`:29-32`) y `{ .compatible = "rockchip,rv1106-rpmsg", .data = (void *)RV1106 }` a la
   tabla (`:402-406`). (El campo `chip` es cosmético; sin datos por-chip.)
2. **Device tree** (en `dts/`, resolver primero el **doble include** de `rv1106-amp.dtsi`
   vía `rv1106-evb.dtsi` *y* `rv1106-sdk-ipc.dtsi`):
   - nodo `rpmsg@0ff00000` (`compatible="rockchip,rv1106-rpmsg"`, `mbox-names="rpmsg-rx","rpmsg-tx"`,
     `mboxes=<&mailbox 0 &mailbox 3>`, `rockchip,link-id`, `rockchip,vdev-nums=<1>`,
     `reg=<0x0ff00000 0x20000>`, `memory-region=<&rpmsg_dma_reserved>`).
   - `reserved-memory`: `rpmsg_reserved@0ff00000` (`no-map`) + `rpmsg_dma_reserved` (`shared-dma-pool`).
   - `&mailbox { status = "okay"; };` (hoy `disabled` en `rv1106.dtsi`).
   - incluir `rv1106-amp.dtsi` (clocks `CLK_CORE_MCU`/`PCLK_MAILBOX`) una sola vez.
3. **defconfig** `configs/kernel/rv1106_minimal_defconfig`: añadir
   `CONFIG_MAILBOX=y`, `CONFIG_ROCKCHIP_MBOX=y`, `CONFIG_RPMSG_ROCKCHIP=y`,
   `CONFIG_RPMSG_ROCKCHIP_TEST=y`, `CONFIG_RPMSG_VIRTIO=y` (`ROCKCHIP_AMP` ya está).
   Tras `make oldconfig` confirmar que `VIRTIO`/`RPMSG` quedan activos.

## 5. Pendiente — lado MCU

1. **Linker** `src/mcu/.../rv1106-mcu/link.lds`: definir
   `__linux_share_rpmsg_start__`/`__linux_share_rpmsg_end__` en `0x0FF00000` (DDR, fuera de la RAM SRAM del MCU).
2. **Dispatcher PING/ECHO** (Estilo B, rpmsg-lite directo — evita el inexistente `rpmsg_base.h`):
   nuevo grupo de fuentes + `INIT_APP_EXPORT(ping_echo_init)`:
   ```c
   instance = rpmsg_lite_remote_init((void*)RPMSG_LINUX_MEM_BASE,
                  RL_PLATFORM_SET_LINK_ID(0 /*A7*/, R /*MCU*/), RL_NO_FLAGS);
   rpmsg_lite_wait_for_link_up(instance);
   ept = rpmsg_lite_create_ept(instance, EPT_ADDR, echo_cb, instance);
   rpmsg_ns_announce(instance, ept, "rpmsg-ap3-ch0", RL_NS_CREATE);
   /* echo_cb: rpmsg_lite_send(instance, ept, src, payload, len, RL_BLOCK); */
   ```
   Modelo: `src/mcu/.../hal/project/rk3562-mcu/src/test_demo.c:467-526`.
3. **defconfig** `board/pwncube/defconfig`: `CONFIG_RT_USING_RPMSG_LITE=y`
   (+ el símbolo del dispatcher). `HAL_MBOX_MODULE_ENABLED` ya está en `hal_conf.h`.

## 6. Flasheo y prueba (sesión en placa, juntos)

1. `./build.sh mcu` → `rtthread.bin`; integrar como `LOADER2=Hpmcu` en el flujo rkbin
   (INI RV1106 del CubeSat ya soporta `Hpmcu`) y `./build.sh` + `./build.sh flash`
   (maskrom + `upgrade_tool`).
2. Arrancar; en Linux observar `dmesg` (el test in-kernel liga al canal `rpmsg-ap3-ch0`,
   imprime "new channel" y el pingpong). No hay `/dev/rpmsg`; es test in-kernel.
3. Iterar sobre los parámetros de co-diseño (§3) y la semántica de IRQ/mailbox del
   `rpmsg_platform.c` (marcado *UNVERIFIED*) hasta lograr el eco.

## 6bis. Arranque del HPMCU — mecanismo real (hallazgos en placa)

> **⚠️ SUPERSEDED.** Esta sección es HISTÓRICA: el arranque
> definitivo (bootrom→0x40000 + FIT `mcu0` con release directo, SIN wrap ni
> trampolín) y el transporte final están en los docs **80** y **90**. Los
> hallazgos "el bootrom no funcionó" y "el A7 no puede escribir 0x40000" eran
> artefactos de los bugs de entonces (kernel@0x8000, MCU1 sin release, DCACHE).

Tras un bring-up extenso en placa se determinó el mecanismo exacto y dónde está el bloqueo:

- **Carga del firmware:** el firmware del HPMCU se ejecuta desde la **IRAM del MCU en 0x40000**
  (`link.lds` ORIGIN=0x40000). El A7 **no** puede escribir esa IRAM directamente (en el mapa del
  A7, 0x40000 es DDR/System RAM). Por eso Rockchip interpone el **`rv1106_hpmcu_wrap`** en
  `hpmcu_sram` (`0xff6fe000`, 8 KB, accesible por A7).
- **Release del núcleo:** U-Boot SPL `spl_fit_standalone_release("mcu0"/"mcu1", 0xff6fe000)` (generado
  desde el trust INI `[MCU]` puesto a `okay`) fija `SGRF_HPMCU_BOOT_ADDR` (`0xff076044`) y, para `mcu0`,
  hace deassert del reset. **Verificado en placa:** `0xff076044 = 0xff6fe000`. El núcleo arranca el wrap.
- **El wrap es un SERVIDOR DE COMANDOS POR MAILBOX (no un cargador de flash):** desensamblado
  (`rv1106_hpmcu_wrap_v1.70.bin`) — inicializa clocks/cache (`0xff6ff004`), y entra en un bucle
  (`0xff6fe494`) que **sondea `A2B_CMD(0)`/`A2B_DAT(0)` del mailbox `0xff5c0000`** y despacha por una
  tabla de 18 comandos: uno recibe `(dirección,longitud)` y **escribe datos en memoria** (carga el
  firmware a 0x40000), otro hace el salto al entry. Es decir, **el A7 debe enviarle el firmware
  comando a comando por mailbox y ordenarle saltar**.
- **BLOQUEO:** ese driver del A7 (que pilota el wrap por mailbox) **no está en el U-Boot/kernel
  open-source** del SDK; Rockchip lo conduce desde un componente cerrado en su ruta de cámara/AOV.
  Con la config TB correcta el wrap se libera (`SGRF` fijado) pero queda **esperando comandos que
  nadie envía** → nuestro rtthread nunca se carga (`mailbox A2B_INTEN`=0, heartbeat en 0xff6ff800 sin
  escribir). El lado Linux rpmsg, en cambio, **sí funciona** (`rpmsg host is online`).
- **NO HACER:** liberar el núcleo a una dirección **DDR** (p.ej. `mcu0=rtthread,0x0fe00000`) **cuelga
  el SPL y brickea** el arranque (recuperación: corto del pin CLK de la SPI-NAND → maskrom del bootrom
  → `upgrade_tool UF` con `[MCU]` disabled). La IRAM/SRAM (0x40000 / 0xff6fe000) es segura.

### Hallazgo definitivo (verificado en placa)
- **El A7 (U-Boot SPL) NO puede escribir la IRAM del HPMCU en 0x40000.** Probada la vía limpia
  `MCU0=rtthread,0x40000,okay` (FIT standalone mcu0 → `spl_fit_standalone_release` carga a 0x40000 +
  release completo del reset): **arranca Linux sin brick** (liberar a IRAM es SEGURO, a diferencia de
  DDR), `SGRF` fijado, **pero rtthread `main()` NO corre** — un heartbeat escrito en el registro
  `B2A_DAT(0)` del mailbox (`0xff5c0034`, leído con certeza por el A7) **y** en `hpmcu_sram`
  (`0xff6ff800`) queda en 0 / basura, `A2B_INTEN`=0. La carga del SPL a 0x40000 cae en **DDR del A7**
  (inútil para el MCU) y el núcleo liberado corre una IRAM vacía. **La IRAM del MCU solo la puede
  llenar el propio MCU (el wrap) o el bootrom.**
- **El driver del A7 que pilota el wrap es CERRADO** (`rockit.ko` / `mcu.S` en Luckfox): `mcu_send_message`
  escribe `A2B_CMD0=0xff5c0008`/`A2B_DAT0=0xff5c000c`; opcodes descifrados: `cmd6`=config,
  `cmd1`=(dir. física DDR del firmware), `cmd5`=start, `cmd9`=reset. El wrap **copia el firmware desde
  esa dirección DDR a la IRAM 0x40000 y arranca**.
- Nota de empaquetado: meter rtthread en el FIT de `uboot.img` requiere `CONFIG_SPL_FIT_IMAGE_KB` que
  no exceda la partición `uboot` de 256K (512 lo paddea a 512K → error de flasheo "Image larger than
  partition"; 256 sí cabe).

### Camino para ejecutar NUESTRO firmware (replicar el wrap)
Escribir un **trampolín propio** (RISC-V, enlazado para `0xff6fe000`, <8 KB) que **sustituya** al
wrap cerrado en el FIT `mcu0`: copia `rtthread.bin` desde una región DDR reservada (donde lo cargue
el SPL/idblock) a la IRAM `0x40000` y salta. Esto evita el protocolo de mailbox cerrado y queda bajo
nuestro control. (Alternativa: portar/escribir el driver A7 que envía los comandos al wrap.) La vía
del trampolín es segura de iterar: liberar a SRAM `0xff6fe000` arranca Linux aunque el MCU falle.

## 7. Riesgos abiertos (resolver en placa)
- Colisión de `0x0FF00000` con OP-TEE/trust (revisar dónde carga `trust.img`).
- Mapeo `link-id`↔canal↔dirección (§3) — el punto más probable de fallo inicial.
- Semántica de acking de IRQ en SCR1/PLIC (el porting omite `rt_hw_interrupt_ack`, ausente en scr1).
- Coherencia de caché en la región DDR compartida (el MCU usa `fence`; validar visibilidad A7↔MCU).
