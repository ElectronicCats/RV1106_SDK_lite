PKG_NAME="alsa-utils"
PKG_VERSION="1.2.8"
PKG_SOURCE="https://www.alsa-project.org/files/pub/utils/alsa-utils-${PKG_VERSION}.tar.bz2"
PKG_LICENSE="GPL-2.0"
PKG_DEPENDS="alsa-lib"
PKG_DESCRIPTION="ALSA utilities (alsactl, aplay, amixer)"
PKG_URL="https://www.alsa-project.org/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/alsa-utils-${PKG_VERSION}.tar.bz2" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/alsa-utils-${PKG_VERSION}"
    ./configure --host=${CROSS_COMPILE%-} --prefix=/usr \
        --disable-alsaconf --disable-alsalogd --disable-nls
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/alsa-utils-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
