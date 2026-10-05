# Empaquetado microSD offline

Este directorio no ejecuta ADB, fastboot ni operaciones sobre la tarjeta. No se ha
generado un candidato físico. `test_packaging.py` verifica los binarios compilados
disponibles y el FV anterior, además de mutaciones deliberadas.

## Requisitos para crear candidato

1. El proveedor `MotoG20SdPlatformDxe` debe existir en el workspace nuevo y haber
   compilado junto con BDS, MotoG20SdDxe, DiskIoDxe, PartitionDxe y Fat. El GUID del
   proveedor esperado es `6311F7E2-0881-4D02-9DDE-1FE254B62090`.
2. El DSC debe contener exactamente esos seis componentes. El proveedor debe
   consumir/publicar el protocolo de plataforma cuya representación GUID aparece
   también en MotoG20SdDxe. Esta comprobación binaria no demuestra su funcionamiento.
3. El registro `REPORTS/BUILD_<tag>` debe conservar `exit_code.txt=0`, el log con
   `- Done -`, `sources.json` y `toolchain.txt`. El workspace actual debe coincidir
   con el inventario SHA256 anterior a esa compilación. No puede existir el lock
   `BUILD_TRANCHE.lock`.
4. Se conservan intactos el padre de consola B158, su FV `AB9E…689D`, el manifiesto
   congelado USB V6 y el registro de ejecuciones físicas. El registro se sella y
   vuelve a verificar al empaquetar; ningún comando aquí lo modifica.
5. Python 3.11 en Windows, Debian WSL y las herramientas EDK2 `GenSec`, `GenFfs` y
   `GenFv` ya instaladas en el laboratorio. Las rutas C:, G: y demás unidades de
   Windows se convierten a `/mnt/<letra>/…`.

## Uso

Desde el workspace, reemplazar `NN` por la nueva compilación que incluya proveedor:

```powershell
$py = 'C:/Users/Angelo/AppData/Local/Programs/Python/Python311/python.exe'
$script = 'work/microsd_v1/packaging/package_microsd_candidate.py'
& $py $script audit-inputs --tag NN
& $py $script seal-build --tag NN --acceptance 'G:/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_20261002_V1/REPORTS/ACCEPTED_NN.json'
& $py $script package --acceptance 'G:/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_20261002_V1/REPORTS/ACCEPTED_NN.json' --output 'G:/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_CANDIDATE_NN'
& $py $script replay --output 'G:/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_CANDIDATE_NN'
```

`--build-root` permite un nuevo workspace completo. `--provider-inf` permite otra
ubicación dentro de MotoG20Pkg, manteniendo nombre, GUID y tipo esperados. El sellado
requiere un archivo nuevo y el empaquetado exige un directorio nuevo. Si falla,
se conserva el directorio parcial para diagnóstico; el script no lo borra ni lo
sobreescribe. Seleccionar otro nombre después de corregir la causa.

## Contrato de salida

- Parte del FV de consola de **23 FFS**, no del FV USB de 26.
- Reproduce primero el FV de consola byte por byte con GenFv.
- Conserva los **22 FFS ajenos a BDS** exactamente. Reemplaza sólo PE/DEPEX de BDS.
- Agrega **cinco FFS**: SD, proveedor, DiskIo, Partition y Fat; total **28**.
- Excluye los GUID conocidos de HC USB, UsbBusDxe y UsbKbDxe; la lista completa de
  FFS queda cerrada contra el padre y los seis módulos autorizados.
- FV de **4 MiB**, transporte de `0x8C2314` bytes con cola cero en offset
  `0x1224000`, imagen Android de **64 MiB**.
- Sólo cambian FV, digest de transporte y campo de identidad canónica SHA256.
  El resto de la imagen conserva los bytes del padre. Se rechazan identidades
  ya registradas. `replay` repite las verificaciones sobre archivos guardados.
- Se guardan hashes, headers PE ARM64, fuentes/configuración/artefactos capturados,
  comandos Gen*, FV, imagen, plantilla sin identidad, transporte y wrapper.

## Límites de evidencia

`PASS_HOST_STATIC_NOT_PHYSICALLY_TESTED` significa únicamente comprobaciones en PC.
No confirma detección de tarjeta, sector legible, FAT, marcador ni arranque WinPE.
Las bibliotecas compartidas y configuración se capturan al sellar; el script no
afirma que hizo una reconstrucción independiente y reproducible del toolchain.
Permanecen los límites de memoria de 66 MiB y ACPI de la base actual. Tampoco se
añade `startup.nsh`, autolanzamiento de Windows ni acceso de escritura a SD.

El controlador físico para probar una identidad nueva es una tarea separada.
