# RV1106 SDK - Sistema de Paquetes

[English](pkg-system.en.md)

## Arquitectura

```
pkg/
├── pkg.sh              # Punto de entrada / CLI del sistema de paquetes
├── package-config      # Selección de paquetes mediante menuconfig
└── available/
    └── <nombre>/
        └── package.mk  # Metadatos del paquete y reglas de compilación
```

## Referencia de API

| Comando         | Descripción                                              |
|-----------------|----------------------------------------------------------|
| `list`          | Lista todos los paquetes disponibles y habilitados       |
| `info`          | Muestra información detallada de un paquete              |
| `enable`        | Marca un paquete como habilitado en la configuración     |
| `disable`       | Marca un paquete como deshabilitado en la configuración  |
| `register`      | Registra un paquete en el sistema de compilación         |
| `build`         | Compila un paquete (descarga, extrae, compila)           |
| `build-all`     | Compila todos los paquetes habilitados                   |
| `install`       | Instala un paquete en el directorio de staging           |
| `install-all`   | Instala todos los paquetes compilados en staging         |
| `clean`         | Limpia los artefactos de compilación de un paquete       |
| `clean-all`     | Limpia los artefactos de compilación de todos            |
| `menuconfig`    | Selección interactiva de paquetes mediante ncurses       |

## Formato `package.mk`

```make
PKG_NAME        := ejemplo
PKG_VERSION     := 1.0.0
PKG_SOURCE      := https://ejemplo.com/ejemplo-1.0.0.tar.gz
PKG_LICENSE     := GPL-2.0
PKG_DEPENDS     := zlib openssl
PKG_DESCRIPTION := Descripción del paquete ejemplo

define pkg_build
    $(MAKE) -C $(PKG_BUILD_DIR) all
endef

define pkg_install
    $(MAKE) -C $(PKG_BUILD_DIR) DESTDIR=$(PKG_INSTALL_DIR) install
endef
```

## Paquetes Semilla

- dropbear — servidor y cliente SSH ligero
- openssl — conjunto de herramientas de criptografía y SSL/TLS
- zlib — biblioteca de compresión
- mtd-utils — utilidades para dispositivos de memoria
- alsa-lib / alsa-utils — biblioteca y utilidades de sonido ALSA
- lzo — biblioteca de compresión en tiempo real
- i2c-tools — herramientas de inspección y manipulación del bus I2C
- strace — trazador de llamadas al sistema
- coreutils — utilidades básicas GNU de archivos, shell y texto

---

[English](pkg-system.en.md)
