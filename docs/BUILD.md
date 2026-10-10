# Compilar y empaquetar

Ruta verificada: Debian WSL, Python 3.13.5, GCC ARM64 14.2.0 y destino
`DEBUG_GCCNOLTO / AARCH64`. La entrada predeterminada es microSD V4. El teléfono
no es necesario para compilar, ejecutar pruebas de host o empaquetar.

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

Antes de compilar las pruebas, `scripts/check_current_sources.py` compara 28
pares de fuentes y cabeceras de `MotoG20Pkg/` con el snapshot V4 de
`development/microsd_sdma_v4/`, incluidos los duplicados compartidos. Solo
normaliza finales de línea. También comprueba el diagnóstico V4, la prueba de
sectores anterior a la conexión del filesystem y la espera permanente antes de
lanzar Shell. Se puede ejecutar por separado con Python 3.

La suite usa sanitizadores de memoria/comportamiento y cubre:

| Componente | Cobertura de host |
| --- | --- |
| Núcleo SD V4 | 86 comprobaciones |
| Buffer SDMA y guardas | 3621 comprobaciones |
| Comparación de sectores | 3155 comprobaciones |
| Conexión de almacenamiento | 64 comprobaciones y 10000 mutaciones |
| Lectura de archivos BDS | 37 comprobaciones |
| Plataforma SD con MMIO simulado | 25 comprobaciones |
| Planes de RAM y CPU conservados | 40 fixtures de RAM, 3000 mutaciones y pruebas del plan de CPU |

El runner de plataforma procede de V1; el checker exige que su motor C y su
cabecera coincidan con V4 antes de ejecutarlo. Los planes de RAM/CPU son trabajo
separado y sus pruebas no los incorporan al firmware. Los resultados de V4 se
guardan en `.build/tests/v4/`; el runner histórico de RAM/CPU también escribe en
`development/winpe_platform_boot/evidence/` (ignorado por Git). Ninguna de estas
pruebas simula por completo el controlador físico ni confirma lectura de la SD.

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

## Alcance del diagnóstico V4

El firmware conserva el bootstrap de **66 MiB**. La página reservada del buffer
SDMA se obtiene dentro de esa región por debajo de 4 GiB. Las propuestas de
expansión de RAM, CPU y ACPI en `development/` no están activadas por esta ruta.

La prueba inicial compara exactamente los 512 bytes de los sectores LBA 0, 2048
y 99072 con `MotoG20SdSectorReferences.h`. Esas referencias pertenecen a la
tarjeta preparada en el laboratorio, no a cualquier tarjeta FAT32. Si las tres
lecturas no coinciden, BDS no intenta conectar el filesystem. Después se valida
el archivo `G20SD.TXT`: sus 31 bytes deben ser exactamente
`MOTO G20 MICROSD READ TEST V1\r\n` y debe observarse el fin del archivo. El
diagnóstico termina esperando una captura, sin ejecutar Windows PE.

Para adaptar el diagnóstico a otra tarjeta, se necesitan referencias obtenidas
independientemente de esa tarjeta y revisar sus límites y contenido. Una lectura
correcta de otra tarjeta puede fallar la comparación exacta. Esta publicación no
incluye una imagen de tarjeta ni los archivos de Windows PE.

La última prueba física corresponde al build original `01_20261008`, conservado
en la publicación del 2026-10-09. Se identificó la tarjeta, pero LBA 0 devolvió
`Device Error`: un intento SDMA, cero completados, guardas intactas y fallo
registrado. El comando leído fue `0x113A0013`, frente a `0x113A0011` programado.
La igualdad estricta del registro puede rechazar el bit adicional `0x2` antes de
esperar la transferencia; esta evidencia **no demuestra que el hardware SDMA
sea incapaz de leer**. No se ha confirmado ningún sector, FAT ni arranque de
Windows PE. La publicación conserva ese código y ese resultado; no incorpora
una corrección posterior a la prueba.

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
