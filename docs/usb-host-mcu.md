# USB host on the RISC-V MCU — bare-metal xHCI, hub + FS device (tutorial)

This is the companion to [`usb-host-linux-golden.md`](usb-host-linux-golden.md)
(the register reference). It explains **how** the MCU drives the DWC3 controller
as a USB host, **why** each piece is there, and **why it works** — so you can
change it. Everything here is verified on hardware: the MCU enumerates a
Full-Speed mouse through a High-Speed hub, `NDEV=3` (root hub + hub + mouse),
first attempt.

## 1. What runs, and where

The RV1106 has a single DWC3 USB2 controller at `0xffb00000` (`usb@ffb00000`,
high-speed UTMI, IRQ GIC_SPI 54 = MCU vector 54). RT-Thread has no DWC3/xHCI
host framework, so we **ported U-Boot's xHCI host stack in NON-DM (legacy) mode**
and bound it to the SoC with a thin glue layer.

```
Cortex-A7 (Linux)        SCR1 RISC-V MCU (RT-Thread, DCACHE OFF)
      │                        │
      │  hands off DWC3        │  owns DWC3 while bound to the MCU
      ▼                        ▼
  dwc3 unbind ───────────► ported U-Boot xHCI  (applications/dwc3/)
```

Source (`src/mcu/rt-thread/bsp/rockchip/rv1106-mcu/applications/`):

| File | Role |
|------|------|
| `usb_dwc3_probe.c` | Test driver: watcher thread polls a mailbox command word; `cmd=5` runs the host bring-up. Owns the **marker map** the A7 reads with `devmem`. |
| `dwc3/dwc3_host.c` | SoC glue: `xhci_hcd_init()` (DWC3 core init + host mode + golden registers), `dwc3_host_up()` → `usb_init()`, `dwc3_host_dev_id(i)` reports enumerated VID:PID. |
| `dwc3/xhci*.c` | Ported U-Boot xHCI (rings, memory/contexts, commands). |
| `dwc3/usb_core.c` | Ported U-Boot core enumeration (`usb_new_device`, descriptors, address, config). |
| `dwc3/usb_hub.c` | Ported U-Boot **hub class** — the subject of §3–§5. |

DCACHE is **off** on the MCU (`RT_USING_CACHE` unset), so MCU memory is coherent
with the controller's DMA without explicit flush/invalidate. Keep it off, or the
ring/context memory needs cache maintenance everywhere.

## 2. How to build, load, and test (no reflash)

```sh
# build
cd src/mcu/rt-thread/bsp/rockchip/rv1106-mcu
RTT_EXEC_PATH=<repo>/toolchain/riscv/xpack-riscv-none-embed-gcc-10.2.0-1.2/bin scons -j8
# -> rtthread.bin

# on the board: hand DWC3 from Linux to the MCU and hot-load (no reflash)
mcutool execute /tmp/rtthread.bin        # loads + starts the firmware
devmem 0xff6ff980 32 5                    # cmd=5 -> host bring-up + 60 s scan window
```

The MCU never reboots between iterations — `mcutool execute` re-loads the binary
into MCU RAM live. `/tmp` is tmpfs, so re-`scp`/transfer `rtthread.bin` and
`mcutool` if the board rebooted.

**Marker map** (A7 reads with `devmem <addr>`), written by the firmware as it
enumerates:

| Addr | Name | Meaning |
|------|------|---------|
| `0xff6ff980` | CMD | command in; firmware sets `0xD0FE` when the scan finishes |
| `0xff6ff98c` | NDEV | number of enumerated devices (root hub counts as 1) |
| `0xff6ff994` | DEV1 | VID:PID of device 1 (the hub) |
| `0xff6ff9e4` | DEV2 | VID:PID of device 2 (the device on the hub) |
| `0xff6ff9e8` | DEV3 | VID:PID of device 3 |
| `0xff6ff9e0` | DEVRES | `usb_new_device()` return (0 = success) |
| `0xff6ff9f8` | TRIES | `0x71E5_00nn` written **only if** an enumeration retry ran (`nn`=tries left) |

A successful FS-mouse run reads: `CMD=0xD0FE`, `NDEV=3`, `DEV1=0x1A400101`
(SL2.1A hub), `DEV2=0x258A0029` (SINOWEALTH mouse), `DEV3=0`, `DEVRES=0`,
`TRIES=0` (no retry needed).

> **The FS device needs a fresh connect event.** After a reload, open the scan
> (`cmd=5`) then **physically replug** the device once during the 60 s window.
> The window is `hub->query_delay + 60000` ms in `usb_hub_configure`, sized for
> hand-timed replugs; shrink it for production.

## 3. How enumeration actually works (host-driven, through the hub)

A common misconception is that the host "looks for" a mouse/keyboard and that a
hub enumerates its own children. **Neither is true.** USB has exactly one
enumerator — the **host**. A hub is a repeater plus a control/status interface;
for a Full-/Low-Speed device behind a High-Speed hub it also carries the host's
transactions as **split transactions** through its **Transaction Translator
(TT)**, but it never assigns addresses or reads descriptors itself.

The real sequence, and where our code does each step:

| Step | What the host does | Our code (`usb_hub.c` unless noted) |
|------|--------------------|--------------------------|
| 1 | Enumerate the hub like any device; see `bDeviceClass = HUB` | `usb_hub_probe` (`usb_core.c:1265`), `USB_CLASS_HUB` check (`:1048`) |
| 2 | Load the hub driver | `usb_hub_configure` (`:760`) |
| 3 | `GET_DESCRIPTOR(HUB)` → number of ports, power, TT think-time | `usb_get_hub_descriptor` (`:90`), `bNbrPorts` → `dev->maxchild` (`:814`) |
| 4 | For each port: `GET_PORT_STATUS`; is something connected? | `usb_get_port_status` (`:165`), loop over `maxchild` |
| 5 | For each occupied port: `SET_PORT_FEATURE(RESET)` → `SET_ADDRESS` → `GET_DESCRIPTOR` → `GET_CONFIG` | `usb_hub_port_reset` (`:334`) → `usb_new_device` (`:519`) |

`usb_set_port_feature`/`usb_clear_port_feature` (`:109`/`:102`) issue the
port-feature control transfers. If any of these were missing, the symptom would
be the *opposite* of a stall: the hub enumerates, prints "N ports," and then
**never** resets or addresses anything downstream. We see a reset and a
successful `SET_ADDRESS`, so the hub class is present and exercised — the whole
sequence above runs.

## 4. The real fix — the two mandatory settle delays

The sequence was complete, but the FS mouse's **first `GET_DESCRIPTOR` through
the TT stalled non-deterministically** (`COMP_STALL`, sometimes on the first
try, sometimes not). Root cause: we jumped straight from *detecting the connect*
→ port reset → `SET_ADDRESS`, **skipping two delays the USB spec mandates in the
hub sequence**. Reset too soon after a connect and a FS/LS device behind a TT is
left in an unstable state; its first split transfer then stalls.

Both delays live in `usb_hub_port_connect_change` (`usb_hub.c`), around the port
reset:

```c
/* Connection debounce (USB 2.0 §7.1.7.3, TATTDB = 100 ms). Let the newly
 * connected device's line state settle before driving the port reset. */
mdelay(100);

ret = usb_hub_port_reset(dev, port, &portstatus);
...
/* Reset recovery (USB 2.0 §7.1.7.5, TRSTRCY >= 10 ms): give the device time
 * after reset before the first control transfer (SET_ADDRESS). */
mdelay(50);
```

**Why it works:** the 100 ms debounce means the reset drives a device whose D+
pull-up and TT state have settled, so the reset produces a clean, stable port;
the 50 ms recovery gives the device its spec-mandated quiet time before the first
control transfer, so the first `GET_DESCRIPTOR` split completes instead of
stalling. Result: **first attempt, no retry** (`TRIES=0`), and the scan converges
cleanly (`CMD=0xD0FE`) instead of the connect-window being re-extended by the
churn of a failing-and-retrying device.

**Knobs:** these are the tuning values for the physical world (a marginal hub,
a long cable, a slow device). 100/50 ms are generous; the spec minimums are
100 ms / 10 ms. Lower them only if you measure margin.

## 5. Safety net — enumeration retry (non-DM path)

Independently, the non-DM path had **no enumeration retry**: U-Boot's `retry:`
loop was compiled only under `#if CONFIG_IS_ENABLED(DM_USB)`. So a single
residual stall killed enumeration outright. We added a retry in the `#else`
(non-DM) branch of `usb_hub_port_connect_change`: on `usb_new_device` failure,
free the device, do a **fresh port reset** (which re-inits the device to address
0 and resets the hub TT state), and retry — up to 6 times. This mirrors Linux's
`hub_port_init` / `PORT_INIT_TRIES`.

```c
int enum_tries = 6;
enum_retry:
    ret = usb_alloc_new_device(dev->controller, &usb);
    ...
    ret = usb_new_device(usb);
    if (ret < 0) {
        usb_free_device(dev->controller);
        dev->children[port] = NULL;
        if (--enum_tries > 0) {
            *(volatile unsigned int*)0xff6ff9f8U = 0x71E50000U | (enum_tries & 0xff);
            if (usb_hub_port_reset(dev, port, &portstatus) >= 0)
                goto enum_retry;   /* fresh reset re-inits device + TT */
        }
    }
```

A per-EP0-level retry does **not** help here — once a split stalls, resetting
just the endpoint (`reset_ep` + `CLEAR_TT_BUFFER`) re-hits the same stall; only a
fresh **port** reset clears it. With the §4 delays in place the retry almost
never fires (`TRIES=0`), but it covers a residual marginal stall. The `TRIES`
marker (`0xff6ff9f8`) lets you see whether it ran.

## 6. Exact device count — de-duplicating connect bounces

A physical replug can bounce: `C_CONNECTION` re-asserts while the device is still
connected and already enumerated. The ported `usb_hub_port_connect_change` block
labelled "Disconnect any existing devices under this port" only returned on
*disconnect* (`CONNECTION=0`); on a still-connected re-report it fell through and
**re-enumerated**, allocating a second device slot for the same physical device
(`NDEV=4`, `DEV2==DEV3`). Fix — treat a re-report of an already-enumerated,
still-connected port as a no-op:

```c
if (!(portstatus & USB_PORT_STAT_CONNECTION))
    return -ENOTCONN;                 /* really gone */
/* Still connected AND this port already has an enumerated child: duplicate
 * connect-change (contact bounce). Re-enumerating would double-count the same
 * device, so treat the re-report as a no-op. */
if (usb_device_has_child_on_port(dev, port))
    return 0;
```

`usb_alloc_new_device`/`usb_free_device` (`usb_core.c`) is a LIFO stack allocator
(`dev_index`), so *not* allocating a duplicate is cleaner than allocating then
freeing a mid-stack slot. Result: `NDEV=3` exact, `DEV3=0`.

## 7. Physical topology note

RV1106 OTG → **FSUSB30MUX** (HS analog D+/D− mux, physical USB/HUB switch) →
**SL2.1A** hub → device. The switch's **USB** position routes to the *charging*
port (device/sink, no host VBUS) — the board can only host **through the hub**
(switch in **HUB**). A direct root-port host test is not possible on this board;
the port can't source VBUS (`PORTSC` CCS=0, `avalid=0`). This is why all host
testing goes through the SL2.1A and its TT.

## Summary

- Ported U-Boot xHCI (non-DM) + full hub class on the MCU; DCACHE off for DMA coherency.
- Enumeration is host-driven through the hub's TT (splits); the hub never enumerates its own children.
- FS-through-TT reliability = the two spec-mandated hub settle delays (§4). This is the root-cause fix.
- Enumeration retry (§5) and connect-bounce de-dup (§6) make it robust and the count exact.
- Verified: `NDEV=3`, mouse `258a:0029` behind SL2.1A `1a40:0101`, first attempt.
