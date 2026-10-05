# Fuentes de desarrollo conservadas

`MotoG20Pkg/` es la entrada de la compilación microSD V1 que se publica.
`development/` conserva implementaciones, pruebas y herramientas existentes en el
laboratorio. No se usa automáticamente en el firmware, salvo las funciones de
validación de empaquetado y las pruebas V1 indicadas en `BUILD.md`.

| Directorio | Estado |
| --- | --- |
| `microsd_v1` | Núcleo SD, plataforma nativa, probe BDS, pruebas y herramientas originales |
| `microsd_fs_v2` | Diagnóstico de sectores y conexión de almacenamiento en desarrollo; no se anuncia como firmware validado |
| `microsd_speed_v2` | Plan y cambios de reloj SD en desarrollo; sin validación física |
| `winpe_platform_boot` | Expansión de RAM, tablas ACPI y preparación de CPU; sin arranque Windows confirmado |
| `usb_ep0_v6` | MUSB, Type-C/VBUS, reconexión y telemetría; teclado pendiente |
| `console_backend_proposal`, `console_progress_proposal` | Propuestas y pruebas de la consola anteriores al cambio a microSD |
| `winpe_media` | Auditorías del ZIP, arquitectura y cadena de arranque local |

Los scripts de laboratorio pueden contener rutas absolutas, controles de una sola
prueba, números de serie y dependencias de registros locales. Se conservan como
fuentes históricas; los comandos portables verificados están en `scripts/`.
Los README dentro de estos directorios describen su revisión original. Para el
estado físico vigente, consultar `STATUS.md`.

`docs/evidence/import-manifest.json` registra hashes de origen y publicación. Los
ajustes de publicación cambian rutas de salida/fixtures y finales de línea; no
continúan la implementación de los controladores pausados.
