PKG_NAME="zlib"
PKG_VERSION="1.3.1"
PKG_SOURCE="https://zlib.net/zlib-${PKG_VERSION}.tar.gz"
PKG_LICENSE="Zlib"
PKG_DEPENDS=""
PKG_DESCRIPTION="Compression library (deflate/gzip/zlib)"
PKG_URL="https://zlib.net/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/zlib-${PKG_VERSION}.tar.gz" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/zlib-${PKG_VERSION}"
    CC=${CROSS_COMPILE}gcc AR=${CROSS_COMPILE}ar \
    ./configure --prefix=/usr --static
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/zlib-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
