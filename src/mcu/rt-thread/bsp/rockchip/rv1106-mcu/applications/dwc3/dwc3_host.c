/*
 * dwc3_host.c — bind the ported U-Boot xHCI host stack to RT-Thread.
 *
 * The ported U-Boot host code (xhci*.c + usb_core.c + usb_hub.c) drives
 * enumeration; usb_init() -> usb_lowlevel_init() -> xhci_hcd_init(). U-Boot
 * expects the SoC glue to supply xhci_hcd_init(): point it at the DWC3 xHCI
 * register block, run the DWC3 core init and put the controller in HOST mode.
 * This mirrors xhci_dwc3.c's DM probe, minus the driver model.
 *
 * Clocks + the innosilicon USB2 PHY (0xff3e0000/GRF) are brought up on the
 * probe.c side (HAL_CRU) before dwc3_host_up() is called — same split as the
 * gadget path.
 */

#include <common.h>
#include <usb.h>
#include <usb/xhci.h>
#include <linux/usb/dwc3.h>

#define RV1106_DWC3_BASE 0xffb00000UL

/* defined (global) in the ported xhci_dwc3.c */
extern int  dwc3_core_init(struct dwc3 *dwc3_reg);
extern void dwc3_set_mode(struct dwc3 *dwc3_reg, u32 mode);

/* U-Boot's non-DM host controller hook, called from usb_lowlevel_init(). */
int xhci_hcd_init(int index, struct xhci_hccr **ret_hccr,
                  struct xhci_hcor **ret_hcor)
{
    struct xhci_hccr *hccr = (struct xhci_hccr *)RV1106_DWC3_BASE;
    struct xhci_hcor *hcor = (struct xhci_hcor *)((uintptr_t)hccr +
                             HC_LENGTH(xhci_readl(&hccr->cr_capbase)));
    struct dwc3 *dwc3_reg = (struct dwc3 *)((char *)hccr + DWC3_REG_OFFSET);
    u32 reg;

    (void)index;

    HDBG(0xA0);
    dwc3_core_init(dwc3_reg);
    HDBG(0xA1);

    /* Replicate Linux's live host golden exactly (captured with the mouse working
     * through the hub). GUCTL1 (0xffb0c11c) = 0x9505018A and GUCTL (0xffb0c12c) =
     * 0x02004010. The low 16 bits already match after dwc3_core_init; this adds
     * TX_IPGAP_LINECHECK_DIS (GUCTL1 bit28) + the device L1/L2 bits Linux sets,
     * and — the important one for host — GUCTL.HSTINAUTORETRY (bit14), the host
     * IN-transaction auto-retry Linux enables (needed for the FS mouse across the
     * hub TT). Note Linux does NOT set PARKMODE_DISABLE_HS (bit17); the earlier
     * Polling stall was the PHY pre-emphasis bug, not park mode. */
    writel(0x9505018AUL, (void *)0xffb0c11cUL);   /* GUCTL1  */
    writel(0x02004010UL, (void *)0xffb0c12cUL);   /* GUCTL (matches Linux golden) */
    /* GUCTL2: the one golden host register dwc3_core_init leaves at default and
     * that we never set. Linux's live host golden reads 0x0000040D here. Its
     * RST_ACTBITLATER bit (and disable-U2-exit-LFPS bits) affect control/split
     * transaction sequencing — a candidate for the deterministic FS DATA-IN
     * split halt. Match the golden. */
    writel(0x0000040DUL, (void *)0xffb0c19cUL);   /* GUCTL2  */

    /* UTMI 16-bit wide + the RV1106 quirks (same as dr_mode=host DTS). */
    reg = readl(&dwc3_reg->g_usb2phycfg[0]);
    reg |= DWC3_GUSB2PHYCFG_PHYIF;
    reg &= ~DWC3_GUSB2PHYCFG_USBTRDTIM_MASK;
    reg |= DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT;
    reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
    reg &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;
    reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
    writel(reg, &dwc3_reg->g_usb2phycfg[0]);

    dwc3_set_mode(dwc3_reg, DWC3_GCTL_PRTCAP_HOST);
    HDBG(0xA2);

    *ret_hccr = hccr;
    *ret_hcor = hcor;
    return 0;
}

void xhci_hcd_stop(int index)
{
    (void)index;
}

/* entry point for the usbprobe thread: enumerate the bus (hub + devices).
 * Returns the number of enumerated USB devices (root hub's children count;
 * a hub + a mouse => 2+), or a negative errno from usb_init(). */
int dwc3_host_up(void)
{
    int r = usb_init();     /* scans, enumerates everything attached */
    int n = 0;
    if (r < 0)
        return r;
    while (usb_get_dev_index(n) != NULL)
        n++;
    return n;
}

/* (idVendor<<16)|idProduct of the i-th enumerated device, 0 if none. */
unsigned int dwc3_host_dev_id(int i)
{
    struct usb_device *d = usb_get_dev_index(i);
    if (!d)
        return 0;
    return ((unsigned int)d->descriptor.idVendor << 16) | d->descriptor.idProduct;
}

/* ---- tiny U-Boot/RT-Thread symbol stubs the host code references ---- */
char *env_get(const char *name) { (void)name; return 0; }   /* no U-Boot env */
long  simple_strtol(const char *c, char **e, unsigned int b) { return strtol(c, e, b); }
void *rt_console_get_device(void) { return 0; }             /* console off on MCU */
