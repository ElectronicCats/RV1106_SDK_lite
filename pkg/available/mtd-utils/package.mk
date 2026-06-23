PKG_NAME="mtd-utils"
PKG_VERSION="2.1.6"
PKG_SOURCE="https://github.com/sigma-star/mtd-utils/archive/refs/tags/v${PKG_VERSION}.tar.gz"
PKG_LICENSE="GPL-2.0"
PKG_DEPENDS="zlib lzo"
PKG_DESCRIPTION="MTD flash utilities (flash_erase, nanddump, ubinize, etc.)"
PKG_URL="http://www.linux-mtd.infradead.org/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/v${PKG_VERSION}.tar.gz" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/mtd-utils-${PKG_VERSION}"
    ./configure --host=${CROSS_COMPILE%-} --prefix=/usr \
        --without-libzstd --without-libuuid --without-libselinux \
        --without-libubifs --enable-install-all
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/mtd-utils-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
