PKG_NAME="dropbear"
PKG_VERSION="2024.85"
PKG_SOURCE="https://matt.ucc.asn.au/dropbear/releases/dropbear-${PKG_VERSION}.tar.bz2"
PKG_LICENSE="MIT"
PKG_DEPENDS=""
PKG_DESCRIPTION="SSH server and client for embedded systems"
PKG_URL="https://matt.ucc.asn.au/dropbear/dropbear.html"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/dropbear-${PKG_VERSION}.tar.bz2" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/dropbear-${PKG_VERSION}"
    ./configure --host=${CROSS_COMPILE%-} --prefix=/usr --disable-zlib
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/dropbear-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
    mkdir -p "${PKG_INSTALL_DIR}/etc/dropbear"
}
