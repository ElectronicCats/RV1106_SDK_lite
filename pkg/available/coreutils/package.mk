PKG_NAME="coreutils"
PKG_VERSION="9.4"
PKG_SOURCE="https://ftp.gnu.org/gnu/coreutils/coreutils-${PKG_VERSION}.tar.xz"
PKG_LICENSE="GPL-3.0"
PKG_DEPENDS=""
PKG_DESCRIPTION="GNU core utilities (cat, ls, rm, mv, etc.)"
PKG_URL="https://www.gnu.org/software/coreutils/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/coreutils-${PKG_VERSION}.tar.xz" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/coreutils-${PKG_VERSION}"
    ./configure --host=${CROSS_COMPILE%-} --prefix=/usr \
        --enable-single-binary=symlinks --without-gmp
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/coreutils-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
