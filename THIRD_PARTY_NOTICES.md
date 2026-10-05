# Licencias y procedencia

Se conservan los avisos de copyright y los identificadores SPDX de los archivos.
No se concede una licencia uniforme a los archivos que no declaran una.

- **TianoCore EDK2**: dependencia Git pública, revisión
  `b03a21a63e3bd001f52c527e5a57feddb53a690b`.
  Su licencia principal `BSD-2-Clause-Patent` se reproduce en
  `licenses/edk2-License.txt`; los componentes y submódulos mantienen sus propias
  licencias. Los cambios locales se conservan en `patches/edk2-motog20.patch`.
- **Motorola kernel-sprd**: referencia fijada a
  `261390cdfff66efef9082bd70c1af0abd21d77f2`. Los proveedores de plataforma SD/USB
  derivados de esos registros declaran `GPL-2.0-only`. El texto GPL se conserva
  en `licenses/GPL-2.0.txt`. `docs/evidence/motorola-source-manifest.json` contiene
  URLs y hashes de las referencias consultadas.
- **Android mkbootimg**: herramienta histórica fijada a
  `d2bb0af5ba6d3198a3e99529c97eda1be0b5a093`; URL en `dependencies.lock.json`.
  No se necesita para el empaquetado portable que reutiliza el contenedor base.
- **Imágenes del proyecto**: las releases incluyen firmware experimental EDK2 y
  el wrapper de arranque específico del Moto G20. Los binarios mantienen las
  licencias de sus componentes; publicar una imagen no cambia esas condiciones.
  Las fuentes del proveedor SD incluido están en `MotoG20Pkg/Drivers/`.

La publicación contiene código y artefactos necesarios del port. Los medios de
Windows PE, el firmware completo de fábrica y las copias de particiones del
teléfono no forman parte de esta distribución.
