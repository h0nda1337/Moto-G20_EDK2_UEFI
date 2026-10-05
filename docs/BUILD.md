# Compilar y empaquetar

Ruta verificada: Debian WSL, Python 3.13.5, GCC ARM64 14.2.0 y destino
`DEBUG_GCCNOLTO / AARCH64`. El teléfono no es necesario para esta ruta.

## Dependencias

```bash
sudo apt-get update
sudo apt-get install git build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  python3 uuid-dev nasm acpica-tools device-tree-compiler
git submodule update --init --recursive
```

`dependencies.lock.json` fija EDK2 en
`b03a21a63e3bd001f52c527e5a57feddb53a690b`. El script de compilación comprueba esa
revisión y aplica una sola vez `patches/edk2-motog20.patch`. El parche conserva
cambios de BaseTools y telemetría DXE del laboratorio; no modifica la revisión Git
fijada del submódulo. Después de compilar es normal que EDK2 muestre esos archivos
modificados.

## Pruebas y módulos

```bash
bash scripts/test_host.sh
bash scripts/build_microsd.sh
```

Las pruebas cubren 35 escenarios del núcleo SD, 25 de plataforma con MMIO simulado
y 31 del lector de archivos con servicios UEFI simulados. No prueban el hardware.

El DSC portable compila seis módulos:

1. `MotoG20SdPlatformDxe`
2. `MotoG20SdDxe`
3. `BdsDxe`
4. `DiskIoDxe`
5. `PartitionDxe`
6. `Fat`

La compilación usa los núcleos disponibles (`JOBS=4 bash scripts/build_microsd.sh`
permite elegir otro número). Los registros, hashes de fuentes y versión del
compilador se guardan en `.build/reports/`; los módulos en
`.build/BUILD/DEBUG_GCCNOLTO/AARCH64/`.

Esta ruta reconstruye los módulos microSD y los incorpora a una base UEFI
preexistente. No reconstruye desde cero todos los componentes de esa base ni el
wrapper Android. Las fuentes históricas correspondientes se conservan en
`MotoG20Pkg/`; sus otros DSC mantienen las rutas del laboratorio y no forman parte
del comando portable verificado.

## Imagen Android de diagnóstico

```bash
python3 scripts/fetch_baseline.py
python3 scripts/package_microsd.py
```

La base se descarga desde la release del propio proyecto y se comprueba por
SHA256. También se puede pasar una copia local:

```bash
python3 scripts/package_microsd.py --baseline /ruta/base.img --output .build/candidato-02
```

El empaquetador exige una compilación terminada y fuentes sin cambios desde ella.
Reconstruye el FV base byte por byte, mantiene sus 22 FFS ajenos a BDS, reemplaza
BDS y agrega cinco FFS: SD, plataforma, DiskIo, Partition y FAT. Comprueba los
headers ARM64, DEPEX, orden de GUID, checksums, colocación y cambios permitidos.

Contrato del contenedor:

- FV: 4 MiB; total de 28 FFS.
- Transporte: `0x8C2314` bytes, con cola cero.
- Offset de transporte: `0x1224000`.
- Imagen Android: 64 MiB.
- SHA256 del transporte e identidad canónica recalculados.

La imagen se guarda en `.build/packaged/MOTO_G20_MICROSD_HOST_ONLY.img` junto con
`PACKAGING_REPORT.json`. Un resultado `PASS_OFFLINE_STRUCTURE_ONLY` confirma el
contenedor en PC; la imagen nueva sigue sin validación física.

El artefacto original de la última prueba se distribuye por separado en la
release, con su SHA256 y resultado físico. Una recompilación puede producir una
identidad diferente; no se presupone igualdad binaria con la imagen original.
