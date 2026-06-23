# RV1106 SDK - Package System

[Español](pkg-system.es.md)

## Architecture

```
pkg/
├── pkg.sh              # Package system entry point / CLI
├── package-config      # Menuconfig-based package selection
└── available/
    └── <name>/
        └── package.mk  # Package metadata and build rules
```

## API Reference

| Command         | Description                                             |
|-----------------|---------------------------------------------------------|
| `list`          | List all available and enabled packages                 |
| `info`          | Show detailed information about a package               |
| `enable`        | Mark a package as enabled in the configuration          |
| `disable`       | Mark a package as disabled in the configuration         |
| `register`      | Register a package into the build system                |
| `build`         | Build a single package (download, extract, compile)     |
| `build-all`     | Build all enabled packages                              |
| `install`       | Install a single package into the staging directory     |
| `install-all`   | Install all built packages into the staging directory   |
| `clean`         | Clean a single package's build artifacts                |
| `clean-all`     | Clean all package build artifacts                       |
| `menuconfig`    | Interactive package selection via ncurses menu          |

## `package.mk` Format

```make
PKG_NAME        := example
PKG_VERSION     := 1.0.0
PKG_SOURCE      := https://example.com/example-1.0.0.tar.gz
PKG_LICENSE     := GPL-2.0
PKG_DEPENDS     := zlib openssl
PKG_DESCRIPTION := Example package description

define pkg_build
    $(MAKE) -C $(PKG_BUILD_DIR) all
endef

define pkg_install
    $(MAKE) -C $(PKG_BUILD_DIR) DESTDIR=$(PKG_INSTALL_DIR) install
endef
```

## Seed Packages

- dropbear — lightweight SSH server and client
- openssl — cryptography and SSL/TLS toolkit
- zlib — compression library
- mtd-utils — Memory Technology Device utilities
- alsa-lib / alsa-utils — ALSA sound library and utilities
- lzo — real-time data compression library
- i2c-tools — I2C bus inspection and manipulation tools
- strace — system call tracer
- coreutils — basic GNU file, shell and text utilities

---

[Español](pkg-system.es.md)
