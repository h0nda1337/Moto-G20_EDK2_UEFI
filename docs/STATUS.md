# Estado físico y siguiente punto de trabajo

Actualizado el 9 de octubre de 2026. La última prueba física es microSD V4,
compilación `01_20261008`, imagen SHA-256:

```text
5B09AEAAE1168D20478123A5C000596701C6A6BDA825B93BE7FF6A7D157B2ADA
```

El usuario aportó `IMG_1396.jpeg`; su transcripción, hash e interpretación están
en [microsd-v4-physical-result.json](evidence/microsd-v4-physical-result.json).
El original se conserva en el laboratorio. Windows PE no ha arrancado.

## Resultado V4

La pantalla muestra preparación de plataforma `stage=11`, `map=Success`,
`prepare=Success`, `ready=1`; identificación de tarjeta y publicación de Block I/O
con capacidad de 60 456 960 sectores. La asignación del búfer SDMA tuvo éxito.
Después aparece:

```text
SDRAW: LBA=0 read=Device Error.
SDCMD17: setup version=00040000 block=00000200 count-reg=00 count=1 mode=0011
SDMA: tries=1 complete=0 guards=1 fault=1
SDREG: valid=03FF phase=4 IRQ=00000000 IE=037F000B PRESENT=01F00000
SDREG: HC1=00000000 HC2=1000 CMD=113A0013 R1=00000900
SDREG: count=1 DMA=00000000:87200200
MICROSD: exact sector proof failed; filesystem test not run.
Windows PE has NOT been started.
```

El código escribe `0x113A0011` y exige que la lectura inmediata del registro sea
idéntica. La foto muestra `0x113A0013`: el bit 1 está añadido. Esa comprobación
provoca el rechazo antes del bucle que espera la finalización de DMA. Por tanto,
no está demostrado un fallo del motor DMA ni una lectura correcta. Las guardas
estaban intactas al inspeccionar el aborto; eso tampoco demuestra que DMA haya
terminado o esté detenido. No se entregó ningún bloque ni se intentó FAT.

Se publica el código que produjo ese resultado. El ajuste del registro aún no
está implementado. La reconstrucción realizada para publicar se valida por
separado y no tiene una prueba física propia.

## Evolución de las pruebas

| Revisión | Resultado físico |
| --- | --- |
| [V1](evidence/microsd-v1-physical-result.json) | Identificación y Block I/O; no se encontró un sistema de archivos. El contador se imprimió antes de conectar los controladores, así que no describe todos los intentos posteriores |
| [V2](evidence/microsd-v2-physical-result.json) | Primera lectura CMD17 con `Device Error`, IRQ `2` y cero bloques entregados |
| [V3](evidence/microsd-v3-physical-result.json) | Se confirmó contador de 32 bits en `0x00`, valor `1`; PIO volvió a fallar sin la señal de lectura que exigía el código |
| [V4](evidence/microsd-v4-physical-result.json) | Se añadió SDMA; se rechazó la lectura del registro de comando antes de esperar su finalización |

IRQ `2` es la señal de transferencia completa, no un bit de CRC o timeout.
En V2/V3 el software rechazó esa condición al faltar los requisitos de PIO.
`No Media` posterior correspondía al bloqueo de la sesión tras el fallo y no
demuestra una extracción física de la tarjeta.

## Trabajo pendiente para Windows PE

1. Investigar la semántica del bit 1 del registro de comando frente al controlador
   nativo de Motorola. Ajustar la comprobación únicamente con evidencia y generar
   una imagen nueva.
2. Demostrar la lectura exacta de los tres sectores de referencia: LBA 0, 2048 y
   99072. Después comprobar FAT y los 31 bytes de `G20SD.TXT`, incluido el fin de archivo.
3. Validar velocidad y estabilidad de lectura antes de cargar un WIM grande.
4. Integrar y validar RAM adicional, ACPI, SMBIOS y preparación de CPU. Esta imagen
   sigue limitada al mapa inicial de unos 66 MiB; las propuestas de expansión
   están separadas en `development/winpe_platform_boot/`.
5. Preparar la cadena ARM64 `bootaa64.efi` / BCD / `boot.sdi` / `boot.wim` y registrar
   por separado entrada al gestor, carga del WIM y entrada a Windows PE.

El Windows PE aportado por el usuario se analizó como ARM64, versión 19041.662.
Sus medios no se redistribuyen. Las herramientas históricas están en
`development/winpe_media/` y requieren entradas externas.

## USB

Se confirmó alimentación sostenida del hub `1A40:0801`, una interfaz de hub y
cero interfaces HID/teclado. El teclado no respondía. `Shell>` y las transferencias
del hub no demuestran entrada de teclado. USB está pausado mientras se trabaja
en microSD.

## Publicación

La publicación incluye código, herramientas, diagnóstico y evidencia. No se ha
reiniciado, flasheado ni modificado la tarjeta durante esta actualización de GitHub.
