# Moto G20 EDK2 UEFI

Port experimental de UEFI para Motorola Moto G20 (Unisoc UMS512), con consola
gráfica y controladores USB y microSD en desarrollo. El objetivo es arrancar
Windows PE ARM64 desde microSD, sin depender de USB.

**Actualización: 9 de octubre de 2026. MicroSD V4 probada en el teléfono;
Windows PE todavía no ha arrancado.**

| Componente | Evidencia actual |
| --- | --- |
| UEFI DXE, framebuffer y Shell | Arranque observado en el teléfono |
| Alimentación USB y hub | Alimentación sostenida; hub `1A40:0801` enumerado |
| Teclado USB | Sin entrada de teclas; `HID=0`, `boot-kbd=0`. Desarrollo pausado |
| Plataforma microSD | Preparación nativa observada con `ready=1` |
| Tarjeta microSD / Block I/O | Identificación, capacidad de 60 456 960 sectores y protocolo publicados |
| Lectura de sectores | V4 intenta SDMA; la primera lectura termina en `Device Error` al comprobar el registro de comando |
| FAT y archivo de prueba | Sin validar; V4 omite esta fase al fallar la comparación de sectores |
| RAM adicional, ACPI, SMBIOS y preparación de CPU | Trabajo separado; integración y validación física pendientes |
| Windows PE | Sin arrancar |

## Última prueba: microSD V4

La V4 incorpora lectura SDMA de un bloque, mantenimiento de caché y una página
reservada con guardas para detectar escrituras fuera del bloque. La comprobación
física exige comparar tres sectores completos con referencias de la tarjeta antes
de intentar FAT y leer el archivo marcador.

En la prueba fotografiada, la tarjeta se identificó y se reservó el búfer, pero
la lectura del primer sector falló. El código programó `CMD=113A0011` y leyó
`113A0013`; su comparación exacta rechazó esa diferencia antes de esperar la
finalización de DMA. Se observaron `tries=1`, `complete=0`, `guards=1`, `fault=1`.
Esto **no demuestra una lectura SDMA correcta ni un fallo del hardware DMA**.
El siguiente paso es investigar el bit 1 de ese registro y repetir la prueba
con un candidato nuevo. [Resultado y próximos pasos](docs/STATUS.md).

La imagen publicada es un diagnóstico UEFI: no contiene Windows y se detiene
mostrando el resultado. El mapa de RAM de esta imagen sigue siendo de unos
66 MiB. Los sectores de referencia pertenecen a la tarjeta de laboratorio;
otra tarjeta necesita sus propias referencias, como explica [BUILD.md](docs/BUILD.md).

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

La salida se guarda en `.build/`. Estos scripts compilan, comprueban y empaquetan
en el PC; no ejecutan operaciones sobre el teléfono. Las pruebas de PC y la
compilación no sustituyen la comprobación física de sectores y archivos.

## Contenido y descargas

- `MotoG20Pkg/`: fuentes de la compilación microSD V4, incluidos el controlador
  SDMA, el proveedor de plataforma y el diagnóstico BDS.
- `edk2/`: revisión fijada, con cambios conservados en `patches/edk2-motog20.patch`.
- `config/`, `scripts/`: configuración y herramientas de compilación portable.
- `development/`: revisiones SD V1–V4, USB V6 y propuestas de velocidad SD y
  plataforma Windows. [Alcance de cada carpeta](docs/DEVELOPMENT.md).
- `docs/evidence/`: resultados físicos V1–V4, procedencia y validación de publicación.

La [prerelease microSD V4](https://github.com/h0nda1337/Moto-G20_EDK2_UEFI/releases/tag/microsd-v4-2026-10-09)
conserva la imagen exacta de la última prueba, su resultado y las sumas SHA-256.
La [instantánea inicial](https://github.com/h0nda1337/Moto-G20_EDK2_UEFI/releases/tag/snapshot-2026-10-05)
mantiene la base UEFI fijada que utiliza el empaquetador.
Los respaldos de particiones, el firmware completo de Motorola y los medios de
Windows PE no se publican.

## Licencias

Se conservan las licencias y avisos de cada archivo. Hay componentes con
`BSD-2-Clause-Patent` y adaptaciones con `GPL-2.0-only`; este repositorio no aplica
una licencia única a todo el árbol. Véase [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
