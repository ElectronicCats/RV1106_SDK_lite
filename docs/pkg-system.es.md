# RV1106 SDK - Sistema de Paquetes

[English](pkg-system.en.md)

## Arquitectura

```
pkg/
├── pkg.sh              # Punto de entrada / CLI
├── package-config      # Selección de paquetes (texto plano)
└── available/
    └── <name>/
        └── package.mk  # Metadatos y reglas de construcción
```

## Uso

```bash
cd pkg
bash pkg.sh list               # lista paquetes (✓=habilitado ○=deshabilitado)
bash pkg.sh enable <name>      # habilita un paquete
bash pkg.sh disable <name>     # deshabilita un paquete
bash pkg.sh build-all          # construye todos los habilitados
bash pkg.sh install-all        # copia paquetes construidos al rootfs
```

## Ejemplo de `package.mk` (`hello-world`)

```bash
# Metadatos
PKG_NAME="hello-world"
PKG_VERSION="1.0"
PKG_LICENSE="MIT"
PKG_DEPENDS=""
PKG_DESCRIPTION="Paquete ejemplo: imprime Hello, RV1106!"

# Construcción: copia fuente de src/hello-world/ y compila
pkg_build() {
    cp -a "${BASE_DIR}"/src/hello-world/* "${PKG_BUILD_DIR}/"
    make -C "${PKG_BUILD_DIR}" \
        CC="${CROSS_COMPILE}gcc" \
        CFLAGS="-Os -Wall" \
        LDFLAGS="-static"
}

# Instalación: make install en PKG_INSTALL_DIR
pkg_install() {
    make -C "${PKG_BUILD_DIR}" \
        DESTDIR="${PKG_INSTALL_DIR}" \
        install
}
```

## Cómo Funciona

1. `build-one` ejecuta `source package.mk` y llama `pkg_build` (compila)
2. Luego llama `pkg_install` (copia artefactos a `PKG_INSTALL_DIR`)
3. `install-all` copia `PKG_INSTALL_DIR/*` → `${ROOTFS_DIR}/`
4. `03-build-rootfs.sh` crea la imagen ext4 desde `${ROOTFS_DIR}`

Variables disponibles en `package.mk`:
- `BASE_DIR` — raíz del SDK
- `CROSS_COMPILE` — prefijo del toolchain
- `PKG_BUILD_DIR` — directorio temporal de compilación
- `PKG_INSTALL_DIR` — directorio temporal de instalación
- `ROOTFS_DIR` — directorio de staging del rootfs

## Cómo Escribir un Paquete Nuevo

```
src/<name>/       # código fuente (Makefile + fuentes)
  ├── Makefile
  └── main.c

pkg/available/<name>/package.mk   # descriptor del paquete
```

Habilitar y construir:

```bash
bash pkg.sh enable <name>
bash pkg.sh build <name>
bash pkg.sh install <name>
```

---

[English](pkg-system.en.md)
