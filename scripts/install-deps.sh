#!/bin/bash
# ---------------------------------------------------------------------------
# install-deps.sh — install ALL host build dependencies in one command.
#
#   ./scripts/install-deps.sh        (or:  ./build.sh deps)
#
# Targets Debian/Ubuntu (apt). The toolchains are vendored in the repo, so this
# only installs the system packages needed to build the kernel, U-Boot, the
# rootfs and the RISC-V MCU firmware. Safe to re-run.
# ---------------------------------------------------------------------------
set -euo pipefail

# One list, kept in sync with docs/getting-started and the README.
PKGS="git make gcc g++ bc cpio rsync fakeroot bison flex \
libssl-dev device-tree-compiler scons \
gawk texinfo cmake unzip gperf autoconf \
libncurses5-dev pkg-config python3"

if ! command -v apt-get >/dev/null 2>&1; then
    echo "[deps] apt-get not found — this installer targets Debian/Ubuntu." >&2
    echo "[deps] Install these packages with your distro's package manager:" >&2
    echo "       ${PKGS}" >&2
    exit 1
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

echo "[deps] Installing host build dependencies (apt)..."
${SUDO} apt-get update
# shellcheck disable=SC2086
${SUDO} apt-get install -y ${PKGS}

echo "[deps] Done. Next:"
echo "       source scripts/00-setup-toolchain.sh"
echo "       ./build.sh"
