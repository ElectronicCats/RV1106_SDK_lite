#!/bin/bash
# RV1106 SDK - Full clean build
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/functions.sh
command -v "${CROSS_COMPILE}gcc" &>/dev/null || err "Toolchain not configured. Run: source scripts/00-setup-toolchain.sh"
log "=== RV1106 SDK Full Build ==="
log "[clean] Removing output/"; rm -rf "$OUTPUT_DIR"; prepare_dirs
log "[toolchain] ${CROSS_COMPILE}"
log "[01] Building U-Boot...";  BOOT_MEDIUM="${RK_BOOT_MEDIUM:-}" bash scripts/01-build-uboot.sh
log "[02] Building Kernel..."; bash scripts/02-build-kernel.sh
log "[03] Building packages..."; cd pkg; bash pkg.sh build-all && bash pkg.sh install-all; cd ..
log "[04] Building Rootfs..."; bash scripts/03-build-rootfs.sh
log "[05] Packing images...";  bash scripts/04-pack-image.sh
log "=== Build complete ==="
log "Output: ${IMAGE_DIR}/update.img"
ls -lh "$IMAGE_DIR"/*.img "$IMAGE_DIR"/download.bin 2>/dev/null || true
echo "Flash:  sudo ./build.sh flash"
