#!/bin/bash
# Board: RV1106 SDK (SPI NAND)
export RK_CHIP="rv1106"
export RK_ARCH="arm"
export RK_TOOLCHAIN_CROSS="arm-rockchip830-linux-uclibcgnueabihf"
export RK_KERNEL_DEFCONFIG="rv1106_minimal_defconfig"
export RK_KERNEL_DTS="rv1106g-sdk"
export RK_UBOOT_DEFCONFIG="rv1106_sdk_defconfig"
export RK_BOOT_MEDIUM="spi_nand"
export RK_PARTITION_CMD_IN_ENV="256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)"
export LF_TARGET_ROOTFS="busybox"
