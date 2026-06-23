#!/bin/bash
#
# Build U-Boot for RV1106
#
# This script builds U-Boot 2017.09 for the Rockchip RV1106 SoC,
# producing uboot.img, trust.img, idblock.img, and download.bin.
#
# Usage:
#   ./scripts/01-build-uboot.sh                 # uses default CROSS_COMPILE
#   CROSS_COMPILE=... ./scripts/01-build-uboot.sh  # override toolchain
#
# Prerequisites:
#   - arm-rockchip830-linux-uclibcgnueabihf- cross-compiler on PATH
#   - Device tree compiler (dtc) installed
#   - Python 2 (for FIT image generation)

set -euo pipefail

# --- Paths ----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UBOOT_SRC="${SDK_DIR}/src/u-boot"
RKBIN_DIR="${SDK_DIR}/src/rkbin"
CONFIGS_DIR="${SDK_DIR}/configs/uboot"
OUTPUT_DIR="${SDK_DIR}/output/images"

DEFCONFIG_NAME="rv1106_sdk_defconfig"
DEFCONFIG_SRC="${CONFIGS_DIR}/${DEFCONFIG_NAME}"
DEFCONFIG_DST="${UBOOT_SRC}/configs/${DEFCONFIG_NAME}"

# --- Toolchain ------------------------------------------------------------
CROSS_COMPILE="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
export CROSS_COMPILE

# --- Build step -----------------------------------------------------------
echo "=========================================="
echo "U-Boot Build Script for RV1106"
echo "=========================================="
echo "Source:      ${UBOOT_SRC}"
echo "Defconfig:   ${DEFCONFIG_SRC}"
echo "rkbin:       ${RKBIN_DIR}"
echo "Output:      ${OUTPUT_DIR}"
echo "CROSS_COMPILE: ${CROSS_COMPILE}"
echo ""

# 1) Ensure output directory exists
mkdir -p "${OUTPUT_DIR}"

# 2) Copy defconfig from SDK configs into U-Boot configs/
if [ -f "${DEFCONFIG_DST}" ]; then
    echo "[*] Defconfig already present in U-Boot configs/, removing first"
    rm -f "${DEFCONFIG_DST}"
fi
echo "[*] Copying defconfig to U-Boot source tree"
cp -v "${DEFCONFIG_SRC}" "${DEFCONFIG_DST}"

# 3) Run defconfig step via make.sh
echo ""
echo "[*] Running: make -C ${UBOOT_SRC} ${DEFCONFIG_NAME}"
make -C "${UBOOT_SRC}" "${DEFCONFIG_NAME}"

# 3b) Override SPL boot order for SPI NAND only (skip MMC to avoid timeout delays)
# Set BOOT_MEDIUM=spi_nand in environment or board config to enable
if [ "${BOOT_MEDIUM:-}" = "spi_nand" ]; then
    dtsi="${UBOOT_SRC}/arch/arm/dts/rv1106-u-boot.dtsi"
    dtsi_bak="${UBOOT_SRC}/arch/arm/dts/rv1106-u-boot.dtsi.bak"
    if [ -f "$dtsi" ] && ! grep -q "= &spi_nand, &emmc" "$dtsi"; then
        cp "$dtsi" "$dtsi_bak"
        sed -i 's/u-boot,spl-boot-order = .*;/u-boot,spl-boot-order = \&spi_nand, \&emmc;/' "$dtsi"
        echo "[*] SPL boot order set to: spi_nand, emmc"
    fi
fi

# 4) Build U-Boot with Rockchip's make.sh wrapper
#    --spl-new tells make.sh to pack the newly built SPL into the loader image
echo ""
echo "[*] Running: cd ${UBOOT_SRC} && ./make.sh --spl-new CROSS_COMPILE=${CROSS_COMPILE}"
cd "${UBOOT_SRC}"
./make.sh --spl-new "CROSS_COMPILE=${CROSS_COMPILE}"
cd "${SCRIPT_DIR}"

# 5) Locate and copy generated images to output/
echo ""
echo "[*] Copying output images to ${OUTPUT_DIR}"

# uboot.img -- U-Boot proper binary
if [ -f "${UBOOT_SRC}/uboot.img" ]; then
    cp -v "${UBOOT_SRC}/uboot.img" "${OUTPUT_DIR}/uboot.img"
else
    echo "WARNING: uboot.img not found!"
fi

# trust.img -- Trusted Execution Environment (OP-TEE / TEE)
if [ -f "${UBOOT_SRC}/trust.img" ]; then
    cp -v "${UBOOT_SRC}/trust.img" "${OUTPUT_DIR}/trust.img"
else
    echo "WARNING: trust.img not found!"
fi

# idblock.img -- IDBlock (DDR init + SPL combined, 1KB-aligned)
# Make.sh generates rv1106_idblock_*.img
for f in "${UBOOT_SRC}"/rv1106_idblock*.img "${UBOOT_SRC}"/*idblock*.img; do
    [ -f "$f" ] && cp -v "$f" "${OUTPUT_DIR}/idblock.img" && break
done
if [ ! -f "${OUTPUT_DIR}/idblock.img" ]; then
    echo "WARNING: idblock.img not found!"
fi

# download.bin -- Full bootable download image (DDR + USB plug + SPL)
# Make.sh generates rv1106_download_*.bin
for f in "${UBOOT_SRC}"/rv1106_download*.bin "${UBOOT_SRC}"/download*.bin; do
    [ -f "$f" ] && cp -v "$f" "${OUTPUT_DIR}/download.bin" && break
done
if [ ! -f "${OUTPUT_DIR}/download.bin" ]; then
    echo "WARNING: download.bin not found!"
fi

# trust.img -- may be embedded in uboot.img; tee.bin is the raw TEE blob
if [ -f "${UBOOT_SRC}/trust.img" ]; then
    cp -v "${UBOOT_SRC}/trust.img" "${OUTPUT_DIR}/trust.img"
elif [ -f "${UBOOT_SRC}/tee.bin" ]; then
    cp -v "${UBOOT_SRC}/tee.bin" "${OUTPUT_DIR}/trust.img"
    echo "NOTE: trust.img created from tee.bin"
fi

# Also copy any loader/*.bin artifacts
if ls "${UBOOT_SRC}"/*loader*.bin 1>/dev/null 2>&1; then
    cp -v "${UBOOT_SRC}"/*loader*.bin "${OUTPUT_DIR}/" || true
fi

# 6) Clean up build artifacts from source tree (but keep the source itself)
echo ""
echo "[*] Cleaning source tree build artifacts"
if [ -f "${UBOOT_SRC}/arch/arm/dts/rv1106-u-boot.dtsi.bak" ]; then
    mv "${UBOOT_SRC}/arch/arm/dts/rv1106-u-boot.dtsi.bak" "${UBOOT_SRC}/arch/arm/dts/rv1106-u-boot.dtsi"
    echo "[*] Restored original rv1106-u-boot.dtsi"
fi
cd "${UBOOT_SRC}"
make distclean 2>/dev/null || true
rm -f "${DEFCONFIG_DST}"  # remove our local copy of the defconfig
rm -f uboot.img trust.img tee.bin *idblock*.img *download*.bin rv1106_idblock*.img rv1106_download*.bin *loader*.bin
rm -f .config .config.old .cc
rm -rf spl tpl
echo "[*] Cleanup done"

echo ""
echo "=========================================="
echo "U-Boot build complete!"
echo "Output files in: ${OUTPUT_DIR}"
ls -la "${OUTPUT_DIR}"
echo "=========================================="
