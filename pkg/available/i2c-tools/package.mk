PKG_NAME="i2c-tools"
PKG_VERSION="4.3"
PKG_SOURCE="https://github.com/kobolabs/i2c-tools/archive/refs/tags/v${PKG_VERSION}.tar.gz"
PKG_LICENSE="GPL-2.0"
PKG_DEPENDS=""
PKG_DESCRIPTION="I2C bus tools (i2cdetect, i2cget, i2cset)"
PKG_URL="https://i2c.wiki.kernel.org/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/v${PKG_VERSION}.tar.gz" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/i2c-tools-${PKG_VERSION}"
    make CC=${CROSS_COMPILE}gcc AR=${CROSS_COMPILE}ar STRIP=${CROSS_COMPILE}strip -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/i2c-tools-${PKG_VERSION}"
    make DESTDIR="${PKG_INSTALL_DIR}" PREFIX=/usr install
}
