# Moto G20 EDK2 UEFI

Port experimental de UEFI para Motorola Moto G20 (Unisoc UMS512), con consola
gráfica y desarrollo de controladores USB y microSD. El objetivo pendiente es
arrancar Windows PE ARM64 desde microSD, sin depender de USB.

**Estado publicado: 5 de octubre de 2026. Windows PE todavía no ha arrancado.**
El desarrollo quedó pausado para publicar esta instantánea.

| Componente | Evidencia actual |
| --- | --- |
| UEFI DXE, framebuffer y Shell | Arranque observado en el teléfono |
| Alimentación USB y hub | LED sostenido; hub `1A40:0801` enumerado |
| Teclado USB | Sin entrada de teclas; `HID=0`, `boot-kbd=0`. Trabajo pausado |
| Plataforma microSD | Preparación nativa observada con `ready=1` |
| Tarjeta microSD / Block I/O | Identificación, capacidad de 60 456 960 sectores y protocolo publicados |
| Lectura de sectores / FAT | Sin demostración física; la última prueba terminó con `no filesystem (Not Found)` |
| RAM adicional, ACPI y preparación de CPU | Fuentes y pruebas en PC; integración y validación física pendientes |
| Windows PE | Pendiente |

La última imagen de diagnóstico **no contiene Windows**. Su prueba identifica la
tarjeta, busca el sistema de archivos e informa el resultado. Un `Success` local
no demuestra que la cadena completa de arranque funcione.

## Compilación

En Debian/Linux o Debian WSL, con Git, Python 3, GCC para ARM64 y las dependencias
indicadas en [BUILD.md](docs/BUILD.md):

```bash
git clone --recurse-submodules https://github.com/h0nda1337/Moto-G20_EDK2_UEFI.git
cd Moto-G20_EDK2_UEFI
bash scripts/test_host.sh
bash scripts/build_microsd.sh
python3 scripts/fetch_baseline.py
python3 scripts/package_microsd.py
```

La salida se guarda en `.build/`. Los scripts de esta ruta compilan y empaquetan
archivos en el PC; no ejecutan operaciones sobre el teléfono.

## Contenido

- `MotoG20Pkg/`: fuentes de la última compilación microSD V1 probada, incluyendo
  PrePi, MMU, GOP, telemetría, BDS, controlador SD y proveedor de plataforma.
- `edk2/`: dependencia Git fijada a una revisión concreta, con los cambios locales
  conservados en `patches/edk2-motog20.patch`.
- `config/`, `scripts/`: configuración de compilación y empaquetado portable.
- `development/`: trabajo actual de USB V6, microSD V2, velocidad SD y plataforma
  Windows, además de herramientas de laboratorio. Sus límites están en
  [DEVELOPMENT.md](docs/DEVELOPMENT.md).
- `docs/evidence/`: resultado físico, procedencia de fuentes e inventario de la
  importación. [STATUS.md](docs/STATUS.md) indica qué falta comprobar.

Las [descargas de la instantánea](https://github.com/h0nda1337/Moto-G20_EDK2_UEFI/releases/tag/snapshot-2026-10-05)
incluyen la última imagen microSD y la base UEFI necesaria para empaquetar.
Los respaldos de particiones, el firmware completo de Motorola y los archivos de
Windows PE se conservan en el laboratorio local.

## Licencias

Se conservan las licencias y avisos de cada archivo. Hay componentes con
`BSD-2-Clause-Patent` y adaptaciones con `GPL-2.0-only`; este repositorio no aplica
una licencia única a todo el árbol. Véase [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
