# Step 0 — IPC transport bring-up (status and recipe)

> Implementation status of **Phase 10, Step 0** (RPMsg transport A7↔RISC-V).
> Distinguishes what is **verified (compiles)** from what remains for the **on-board session**.
> Decisions taken: cede the buses to the RISC-V; our own `platform/RV1106/` porting;
> MCU without console (verification via IPC); advance Step 0 in software.

## 1. Done and verified (compiles)

**MCU firmware base** (Phase 10 base):
- Vendored RT-Thread tree in `src/mcu/`; toolchain `toolchain/riscv/` (`riscv-none-embed-gcc 10.2.0`).
- Minimal board `src/mcu/.../rv1106-mcu/board/pwncube/` (no camera/ISP; **no uart2** — Linux owns the console `ttyFIQ0`).
- `scripts/06-build-mcu.sh` + `./build.sh mcu` → `output/mcu/rtthread.bin` (~20 KB).

**rpmsg-lite RISC-V porting** (the hardest piece — compiles):
- `src/mcu/.../rpmsg-lite/lib/include/platform/RV1106/rpmsg_config.h` — RV1106 mailbox IRQ (a single AP/BB pair, not 8 per channel).
- `.../include/platform/RV1106/rpmsg_platform.h` — agnostic copy of RK3568.
- `.../rpmsg_lite/porting/platform/RV1106/rpmsg_platform.c` — **rewritten for RISC-V**:
  no SMP/affinity/`HAL_CPU_TOPOLOGY`, `MBOX0`→`MBOX`, single IRQ `MAILBOX0_BB_IRQn`,
  `cpsid/cpsie`→`rt_hw_interrupt_disable/enable`, `platform_in_isr`→`rt_interrupt_get_nest`.
- Portability fix: `.../rpmsg-lite/lib/include/rpmsg_compiler.h` — `MEM_BARRIER()` uses
  `fence` (RISC-V) instead of `dsb` (ARM) under `#if defined(__riscv)`.
- Build selection: `.../rpmsg-lite/SConscript` and `.../common/drivers/Kconfig`
  (`RT_USING_RPMSG_LITE` `depends on ... || SOC_RV1106`).

> **`RT_USING_RPMSG_LITE` stays DISABLED by default** in `board/pwncube/defconfig`
> to keep the base compiling until the link + on-board behavior are validated.

**Board data (read over serial, non-destructive):**
- DDR = **256 MB** at `0x00000000–0x0fffffff` (all System RAM; no `reserved-memory`).
- Linux console = `ttyFIQ0` (uart2). `/dev/ttyUSB0` @115200 gives root shell. sudo `1334`.

## 2. Shared memory map (proposal to fix on-board)

Reserve **1 MB at the top of DDR** for the RPMsg vrings, identical on both sides:

| Symbol | Proposed value | Notes |
|---------|-----------------|-------|
| Shared region base | `0x0FF00000` | Top of 256 MB. **Verify it does not collide with OP-TEE/trust.** |
| Size | `0x00100000` (1 MB) | Covers 1 instance (2 vrings × 0x8000 = 0x10000) with slack. |

- **Linux:** `reserved-memory` `no-map` node at `0x0FF00000` + `reg` of the `rpmsg` node.
- **MCU:** linker symbols `__linux_share_rpmsg_start__/__end__` in `src/mcu/.../rv1106-mcu/link.lds`
  pointing to the **same** address (NOT the SRAM 0x40000; it is DDR).
- `VRING_ALIGN=0x1000` mandatory (Linux requirement).

## 3. Parameters that MUST match on both sides (co-design on-board)

| Parameter | Proposal | Risk / to resolve |
|-----------|-----------|---------------------|
| `link-id` | `0x04` (kernel) ↔ MCU encoding | With `RL_GET_R_CPU_ID(0x04)=4` the channel index **runs off** the MCU's array of 4 channels. Reconcile to a valid channel 0–3. |
| Mailbox channel | rx=ch0 / tx=ch3 (kernel) ↔ `RL_RV1106_MBOX_CHAN=0` (MCU) | Kernel uses 2 channels per direction; rpmsg-lite uses 1 bidirectional channel (A2B/B2A). **Mismatch to resolve.** |
| A2B/B2A direction | MCU = remote (B2A sends, receives A2B) | Validate the semantics of `HAL_MBOX` on hardware. |
| NS channel name | `rpmsg-ap3-ch0` | The in-kernel driver `rockchip_rpmsg_test.c` binds by this exact name → the MCU must `rpmsg_ns_announce("rpmsg-ap3-ch0")`. |
| `RL_RPMSG_MAGIC` | `0x524D5347` ("RMSG") | Already equal on both. ✔ |

## 4. Pending — Linux side (recipe ready, not applied)

1. **Kernel match** `src/kernel/drivers/rpmsg/rockchip_rpmsg.c`: add `RV1106` to the enum
   (`:29-32`) and `{ .compatible = "rockchip,rv1106-rpmsg", .data = (void *)RV1106 }` to the
   table (`:402-406`). (The `chip` field is cosmetic; no per-chip data.)
2. **Device tree** (in `dts/`, first resolve the **double include** of `rv1106-amp.dtsi`
   via `rv1106-evb.dtsi` *and* `rv1106-sdk-ipc.dtsi`):
   - `rpmsg@0ff00000` node (`compatible="rockchip,rv1106-rpmsg"`, `mbox-names="rpmsg-rx","rpmsg-tx"`,
     `mboxes=<&mailbox 0 &mailbox 3>`, `rockchip,link-id`, `rockchip,vdev-nums=<1>`,
     `reg=<0x0ff00000 0x20000>`, `memory-region=<&rpmsg_dma_reserved>`).
   - `reserved-memory`: `rpmsg_reserved@0ff00000` (`no-map`) + `rpmsg_dma_reserved` (`shared-dma-pool`).
   - `&mailbox { status = "okay"; };` (today `disabled` in `rv1106.dtsi`).
   - include `rv1106-amp.dtsi` (clocks `CLK_CORE_MCU`/`PCLK_MAILBOX`) a single time.
3. **defconfig** `configs/kernel/rv1106_minimal_defconfig`: add
   `CONFIG_MAILBOX=y`, `CONFIG_ROCKCHIP_MBOX=y`, `CONFIG_RPMSG_ROCKCHIP=y`,
   `CONFIG_RPMSG_ROCKCHIP_TEST=y`, `CONFIG_RPMSG_VIRTIO=y` (`ROCKCHIP_AMP` is already there).
   After `make oldconfig` confirm that `VIRTIO`/`RPMSG` stay active.

## 5. Pending — MCU side

1. **Linker** `src/mcu/.../rv1106-mcu/link.lds`: define
   `__linux_share_rpmsg_start__`/`__linux_share_rpmsg_end__` at `0x0FF00000` (DDR, outside the MCU's SRAM RAM).
2. **PING/ECHO dispatcher** (Style B, direct rpmsg-lite — avoids the nonexistent `rpmsg_base.h`):
   new source group + `INIT_APP_EXPORT(ping_echo_init)`:
   ```c
   instance = rpmsg_lite_remote_init((void*)RPMSG_LINUX_MEM_BASE,
                  RL_PLATFORM_SET_LINK_ID(0 /*A7*/, R /*MCU*/), RL_NO_FLAGS);
   rpmsg_lite_wait_for_link_up(instance);
   ept = rpmsg_lite_create_ept(instance, EPT_ADDR, echo_cb, instance);
   rpmsg_ns_announce(instance, ept, "rpmsg-ap3-ch0", RL_NS_CREATE);
   /* echo_cb: rpmsg_lite_send(instance, ept, src, payload, len, RL_BLOCK); */
   ```
   Model: `src/mcu/.../hal/project/rk3562-mcu/src/test_demo.c:467-526`.
3. **defconfig** `board/pwncube/defconfig`: `CONFIG_RT_USING_RPMSG_LITE=y`
   (+ the dispatcher symbol). `HAL_MBOX_MODULE_ENABLED` is already in `hal_conf.h`.

## 6. Flash and test (on-board session, together)

1. `./build.sh mcu` → `rtthread.bin`; integrate as `LOADER2=Hpmcu` in the rkbin flow
   (the CubeSat RV1106 INI already supports `Hpmcu`) and `./build.sh` + `./build.sh flash`
   (maskrom + `upgrade_tool`).
2. Boot; on Linux watch `dmesg` (the in-kernel test binds to the channel `rpmsg-ap3-ch0`,
   prints "new channel" and the pingpong). There is no `/dev/rpmsg`; it is an in-kernel test.
3. Iterate over the co-design parameters (§3) and the IRQ/mailbox semantics of
   `rpmsg_platform.c` (marked *UNVERIFIED*) until the echo is achieved.

## 6bis. HPMCU boot — real mechanism (on-board findings, 2026-06-30)

> **⚠️ SUPERSEDED (2026-07-02).** This section is HISTORICAL: the definitive
> boot (bootrom→0x40000 + FIT `mcu0` with direct release, WITHOUT wrap nor
> trampoline) and the final transport are in docs **80** and **90**. The
> findings "the bootrom did not work" and "the A7 cannot write 0x40000" were
> artifacts of the bugs of that time (kernel@0x8000, MCU1 without release, DCACHE).

After an extensive on-board bring-up the exact mechanism and where the blockage is were determined:

- **Firmware load:** the HPMCU firmware runs from the **MCU's IRAM at 0x40000**
  (`link.lds` ORIGIN=0x40000). The A7 **cannot** write that IRAM directly (in the A7's
  map, 0x40000 is DDR/System RAM). That is why Rockchip interposes the **`rv1106_hpmcu_wrap`** in
  `hpmcu_sram` (`0xff6fe000`, 8 KB, accessible by A7).
- **Core release:** U-Boot SPL `spl_fit_standalone_release("mcu0"/"mcu1", 0xff6fe000)` (generated
  from the trust INI `[MCU]` set to `okay`) sets `SGRF_HPMCU_BOOT_ADDR` (`0xff076044`) and, for `mcu0`,
  deasserts the reset. **Verified on-board:** `0xff076044 = 0xff6fe000`. The core boots the wrap.
- **The wrap is a MAILBOX COMMAND SERVER (not a flash loader):** disassembly
  (`rv1106_hpmcu_wrap_v1.70.bin`) — initializes clocks/cache (`0xff6ff004`), and enters a loop
  (`0xff6fe494`) that **polls `A2B_CMD(0)`/`A2B_DAT(0)` of mailbox `0xff5c0000`** and dispatches via an
  18-command table: one receives `(address,length)` and **writes data to memory** (loads the
  firmware to 0x40000), another jumps to the entry. That is, **the A7 must send it the firmware
  command by command over mailbox and order it to jump**.
- **BLOCKAGE:** that A7 driver (which drives the wrap over mailbox) **is not in the open-source U-Boot/kernel**
  of the SDK; Rockchip drives it from a closed component in its camera/AOV path.
  With the correct TB config the wrap is released (`SGRF` set) but stays **waiting for commands that
  nobody sends** → our rtthread never gets loaded (`mailbox A2B_INTEN`=0, heartbeat at 0xff6ff800 not
  written). The Linux rpmsg side, by contrast, **does work** (`rpmsg host is online`).
- **DO NOT:** release the core to a **DDR** address (e.g. `mcu0=rtthread,0x0fe00000`) **hangs
  the SPL and bricks** the boot (recovery: short the CLK pin of the SPI-NAND → bootrom maskrom
  → `upgrade_tool UF` with `[MCU]` disabled). The IRAM/SRAM (0x40000 / 0xff6fe000) is safe.

### Definitive finding (verified on-board)
- **The A7 (U-Boot SPL) CANNOT write the HPMCU's IRAM at 0x40000.** The clean route
  `MCU0=rtthread,0x40000,okay` was tested (FIT standalone mcu0 → `spl_fit_standalone_release` loads to 0x40000 +
  full reset release): **Linux boots without brick** (releasing to IRAM is SAFE, unlike
  DDR), `SGRF` set, **but rtthread `main()` does NOT run** — a heartbeat written to the register
  `B2A_DAT(0)` of the mailbox (`0xff5c0034`, read with certainty by the A7) **and** in `hpmcu_sram`
  (`0xff6ff800`) stays at 0 / garbage, `A2B_INTEN`=0. The SPL load to 0x40000 lands in **A7's DDR**
  (useless for the MCU) and the released core runs an empty IRAM. **The MCU's IRAM can only be
  filled by the MCU itself (the wrap) or the bootrom.**
- **The A7 driver that drives the wrap is CLOSED** (`rockit.ko` / `mcu.S` in Luckfox): `mcu_send_message`
  writes `A2B_CMD0=0xff5c0008`/`A2B_DAT0=0xff5c000c`; deciphered opcodes: `cmd6`=config,
  `cmd1`=(physical DDR address of the firmware), `cmd5`=start, `cmd9`=reset. The wrap **copies the firmware from
  that DDR address to the IRAM 0x40000 and boots it**.
- Packaging note: putting rtthread into the FIT of `uboot.img` requires `CONFIG_SPL_FIT_IMAGE_KB` that
  does not exceed the 256K `uboot` partition (512 pads it to 512K → flashing error "Image larger than
  partition"; 256 does fit).

### Path to run OUR firmware (replicate the wrap)
Write **our own trampoline** (RISC-V, linked for `0xff6fe000`, <8 KB) that **replaces** the
closed wrap in the FIT `mcu0`: it copies `rtthread.bin` from a reserved DDR region (where the
SPL/idblock loads it) to the IRAM `0x40000` and jumps. This avoids the closed mailbox protocol and stays under
our control. (Alternative: port/write the A7 driver that sends the commands to the wrap.) The trampoline
route is safe to iterate: releasing to SRAM `0xff6fe000` boots Linux even if the MCU fails.

## 7. Open risks (resolve on-board)
- Collision of `0x0FF00000` with OP-TEE/trust (review where `trust.img` loads).
- Mapping `link-id`↔channel↔direction (§3) — the most likely point of initial failure.
- IRQ acking semantics in SCR1/PLIC (the porting omits `rt_hw_interrupt_ack`, absent in scr1).
- Cache coherence in the shared DDR region (the MCU uses `fence`; validate A7↔MCU visibility).
