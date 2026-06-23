PKG_NAME="alsa-lib"
PKG_VERSION="1.2.8"
PKG_SOURCE="https://www.alsa-project.org/files/pub/lib/alsa-lib-${PKG_VERSION}.tar.bz2"
PKG_LICENSE="LGPL-2.1"
PKG_DEPENDS=""
PKG_DESCRIPTION="ALSA userspace library"
PKG_URL="https://www.alsa-project.org/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/alsa-lib-${PKG_VERSION}.tar.bz2" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/alsa-lib-${PKG_VERSION}"
    ./configure --host=${CROSS_COMPILE%-} --prefix=/usr --disable-python
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/alsa-lib-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
