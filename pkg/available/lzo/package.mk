PKG_NAME="lzo"
PKG_VERSION="2.10"
PKG_SOURCE="https://www.oberhumer.com/opensource/lzo/download/lzo-${PKG_VERSION}.tar.gz"
PKG_LICENSE="GPL-2.0"
PKG_DEPENDS=""
PKG_DESCRIPTION="LZO compression library"
PKG_URL="https://www.oberhumer.com/opensource/lzo/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/lzo-${PKG_VERSION}.tar.gz" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/lzo-${PKG_VERSION}"
    ./configure --host=${CROSS_COMPILE%-} --prefix=/usr --enable-static
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/lzo-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
