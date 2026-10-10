# Fuentes de desarrollo conservadas

`MotoG20Pkg/` es la entrada de la compilación **microSD V4** publicada el
2026-10-09. Sus fuentes SD activas coinciden con el snapshot de
`development/microsd_sdma_v4/`. El checker y las pruebas descritos en `BUILD.md`
comprueban esa correspondencia antes de probar la lógica de host.

Los demás directorios de `development/` conservan implementaciones, pruebas y
herramientas del laboratorio. No se incorporan automáticamente al firmware. El
empaquetador reutiliza las funciones de validación del contenedor de V1 y la
suite reutiliza su runner de plataforma únicamente después de comprobar que el
motor coincide con V4.

| Directorio | Estado |
| --- | --- |
| `microsd_v1` | Primera identificación física de la tarjeta y publicación de Block I/O; sin prueba de lectura de sectores ni filesystem |
| `microsd_fs_v2` | Prueba física: falló la primera lectura CMD17 de LBA 0; sin FAT ni Windows PE |
| `microsd_pio_v3` | Prueba física: configuración de contador verificada, pero CMD17 de LBA 0 volvió a fallar sin datos PIO |
| `microsd_sdma_v4` | Fuente activa: lectura SDMA de un sector con buffer reservado y guardas; prueba física detenida en la comprobación del comando leído, sin sector confirmado |
| `microsd_speed_v2` | Plan y cambios de reloj SD en desarrollo; sin validación física |
| `winpe_platform_boot` | Expansión de RAM, tablas ACPI y preparación de CPU; sin arranque Windows confirmado |
| `usb_ep0_v6` | MUSB, Type-C/VBUS, reconexión y telemetría; teclado pendiente |
| `console_backend_proposal`, `console_progress_proposal` | Propuestas y pruebas de la consola anteriores al cambio a microSD |
| `winpe_media` | Auditorías del ZIP, arquitectura y cadena de arranque local |

## Estado de V4

El build físico original es `01_20261008`. En la última fotografía el comando
leído es `0x113A0013`, mientras que el código programa `0x113A0011` y exige
igualdad exacta. La diferencia `0x2` puede hacer fallar esa comprobación antes de
esperar la transferencia. Los contadores muestran un intento, cero completados,
guardas intactas y fallo registrado. No basta para concluir que SDMA falló en el
hardware: queda por resolver y probar la comprobación de readback.

La publicación conserva esta revisión para reproducirla. No corrige el driver ni
convierte las pruebas de host en validación física. Siguen sin confirmarse la
lectura de un sector, el montaje de FAT y el arranque de Windows PE.

El firmware sigue usando **66 MiB** de bootstrap. Los planes de memoria ampliada,
CPU y ACPI permanecen separados. Las referencias de sectores y el archivo
marcador del diagnóstico V4 pertenecen a la tarjeta del laboratorio; no definen
un medio genérico ni incluyen Windows PE.

## Alcance de los snapshots

Los scripts de laboratorio pueden contener rutas absolutas, controles de una sola
prueba, números de serie y dependencias de registros locales. Se conservan como
fuentes históricas; los comandos portables verificados están en `scripts/`.
Los README dentro de estos directorios describen su revisión original. Para el
estado físico vigente, consultar `STATUS.md`.

V3 y V4 contienen los directorios `core`, `driver`, `platform`, `bds` y `tests`
necesarios para revisar las fuentes conservadas. La importación V4 omite el
integrador de laboratorio, los controladores de prueba física y los runners
`test_integration.py` y `run_prefixed_negative.sh`: dependían de rutas locales o
de una combinación antigua de fixtures. El runner portable publica las pruebas
vigentes indicadas en `BUILD.md`.

`docs/evidence/import-manifest.json` registra hashes de origen y publicación. Los
ajustes de publicación cambian rutas de salida/fixtures y finales de línea; no
continúan la implementación de los controladores pausados. Las comprobaciones
de equivalencia de fuentes evitan probar una copia diferente de la usada por la
compilación predeterminada.
