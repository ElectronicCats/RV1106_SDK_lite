PKG_NAME="strace"
PKG_VERSION="6.6"
PKG_SOURCE="https://github.com/strace/strace/releases/download/v${PKG_VERSION}/strace-${PKG_VERSION}.tar.xz"
PKG_LICENSE="LGPL-2.1"
PKG_DEPENDS=""
PKG_DESCRIPTION="System call tracer for debugging"
PKG_URL="https://strace.io/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/strace-${PKG_VERSION}.tar.xz" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/strace-${PKG_VERSION}"
    ./configure --host=${CROSS_COMPILE%-} --prefix=/usr --enable-mpers=no
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/strace-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
