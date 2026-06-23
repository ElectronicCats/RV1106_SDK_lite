PKG_NAME="openssl"
PKG_VERSION="3.0.13"
PKG_SOURCE="https://github.com/openssl/openssl/releases/download/openssl-${PKG_VERSION}/openssl-${PKG_VERSION}.tar.gz"
PKG_LICENSE="Apache-2.0"
PKG_DEPENDS=""
PKG_DESCRIPTION="Cryptography and SSL/TLS toolkit"
PKG_URL="https://www.openssl.org/"

pkg_build() {
    tar xf "${PKG_SOURCE_DIR}/openssl-${PKG_VERSION}.tar.gz" -C "${PKG_BUILD_DIR}"
    cd "${PKG_BUILD_DIR}/openssl-${PKG_VERSION}"
    ./Configure linux-armv4 --prefix=/usr --cross-compile-prefix=${CROSS_COMPILE} \
        no-asm no-shared no-tests no-docs
    make -j$(nproc)
}

pkg_install() {
    cd "${PKG_BUILD_DIR}/openssl-${PKG_VERSION}"
    make install DESTDIR="${PKG_INSTALL_DIR}"
}
