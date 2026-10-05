# Estado físico y siguiente punto de trabajo

Última prueba microSD: imagen
`B94E2C12D466AEE3BAC25E11AF853399EDA5132DCD36346A08ACE3C7A6BB37CB`.
La fotografía y su transcripción están en `evidence/`.

La pantalla mostró preparación de plataforma `stage=11`, `map=Success`,
`prepare=Success`, `ready=1`; después, `MICROSD: stage=3 status=Success published=1`
y capacidad de 60 456 960 sectores. Terminó con:

```text
MICROSD: no filesystem (Not Found); read test not run.
Windows PE has NOT been started. Forced restart returns to Android.
```

Esto confirma preparación de plataforma, identificación de tarjeta y publicación
de Block I/O. Quedan pendientes una lectura CMD17 demostrada, acceso FAT, lectura
de un archivo marcador y la ejecución del gestor de arranque de Windows.
`blocks=0` se imprimió antes de `ConnectAll`, así que no basta para concluir qué
operaciones intentaron después los controladores de particiones y FAT.

## Para retomar Windows PE

1. Completar y comprobar el diagnóstico microSD V2: lecturas directas comparadas
   con sectores conocidos, conexión selectiva de controladores y errores después
   de conectar particiones/FAT.
2. Validar la lectura de FAT y del archivo marcador en la tarjeta real.
3. Integrar y validar RAM adicional, ACPI, SMBIOS y preparación de CPU. La base
   actual sigue limitada al mapa de memoria de aproximadamente 66 MiB; los
   candidatos de expansión de RAM no están incluidos en la imagen probada.
4. Preparar la cadena ARM64 `bootaa64.efi` / BCD / `boot.sdi` / `boot.wim` desde
   medios Windows PE aportados por el usuario y pasar al gestor de arranque.
5. Registrar por separado entrada al gestor, carga del WIM y entrada a Windows PE.

El archivo Windows PE local se analizó como ARM64, versión 19041.662. Los medios
de Windows no se redistribuyen en este repositorio. Los scripts de auditoría están
en `development/winpe_media/` y requieren las rutas de entrada del laboratorio.

## USB

El usuario confirmó alimentación sostenida del hub. La última evidencia muestra
un hub `1A40:0801`, una interfaz de hub y cero interfaces HID/teclado. El teclado
no respondía. La aparición de `Shell>` y los contadores de transferencia del hub
no demuestran entrada de teclado. USB quedó pausado al cambiar a microSD.

## Pausa de publicación

Esta instantánea conserva el proyecto tal como estaba al solicitar la publicación.
No se ha reiniciado, flasheado ni modificado la tarjeta durante la publicación.
