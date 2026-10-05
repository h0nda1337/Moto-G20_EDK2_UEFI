# Propuesta aislada: cesión del monitor a BDS

Estado: seis archivos propuestos, diff aplicable y pruebas C de control de flujo aprobadas. No se han modificado fuentes vivas, freezes, registros de ejecución ni el teléfono. No se ha construido firmware ARM en esta subtarea.

El panel observado en IMG_1485.jpeg acredita el bitmap acumulado 0x3F. No acredita una consola física conectada ni Shell. ProgressDxe mantiene notificaciones que pueden ejecutar de nuevo su checkpoint después de BDS; su copia estática de `mDisplayReleased` es distinta de la copia de BDS. Por eso el `ReleaseDisplay()` anterior en BootManager no silencia ProgressDxe.

## Cambio

ProgressDxe publica un protocolo privado de revisión 1, después de registrar sus seis eventos. `Quiesce()` ejecuta dentro de la imagen del monitor: sube de TPL_APPLICATION a TPL_NOTIFY, fija su latch `mQuiesced`, libera su propia instancia de telemetría, captura el bitmap, cierra eventos y restaura el TPL. La guarda del callback mantiene la supresión aunque un evento no pueda cerrarse. Una segunda llamada solo vuelve a intentar cerrar eventos pendientes.

Además, cada bit observado se informa una sola vez. Antes del handoff, una segunda instalación de SimpleTextOut ya observado deja de reemplazar checkpoints posteriores. Esto no registra cantidad ni identidad de todas las instancias; conserva el significado de bitmap acumulado de tipos de protocolo.

`PlatformBootManagerBeforeConsole()` exige ese protocolo, valida revisión/método y ejecuta `Quiesce()` antes de configurar/conectar consola. Su checkpoint BEFORE CONSOLE contiene estado real de la cesión, bitmap observado y puntero al protocolo de control. Es evidencia de llegada a ese callback BDS. Una ausencia/incompatibilidad/error se muestra como fallo de BEFORE CONSOLE en vez de seguir con la propiedad de pantalla indeterminada.

## TPL y reentrada

Los eventos del monitor se registran a TPL_CALLBACK. `CoreRestoreTpl()` solo despacha eventos cuyo TPL es superior al destino al que se restaura; mantener TPL_NOTIFY durante el latch y los cierres excluye estos callbacks. `CoreCloseEvent()` elimina notificaciones en cola y desregistra el evento de la base de protocolos antes de liberar memoria. Al restaurar TPL_APPLICATION, cualquier evento que no se haya cerrado encuentra `mQuiesced` antes de consultar protocolos o dibujar.

El contrato del protocolo es BSP, TPL_APPLICATION, Boot Services. Es el contexto de BeforeConsole dentro de BDS. No se llama desde excepciones ni desde DXE temprano. La biblioteca de telemetría no recibe gBS, asignaciones, búsquedas de protocolo ni otras dependencias nuevas; conserva su uso previo a Boot Services y su publicación fatal. No se reserva memoria física nueva.

Fuentes locales revisadas: `edk2/MdeModulePkg/Core/Dxe/Event/Tpl.c` (`CoreRestoreTpl`), `Event.c` (`CoreCloseEvent`), `edk2/MdeModulePkg/Universal/BdsDxe/BdsEntry.c` (BeforeConsole antes de ConnectConsole/AfterConsole), `MotoG20DxeTelemetryLib.c` (bandera estática y fallo que puede recuperar la pantalla). El DxeCore actual hace sus checkpoints normales antes de transferir control a BDS; sus capturas posteriores de entrada de imagen no publican checkpoints normales.

## Pruebas

`tests/progress_test.c` incluye la fuente C propuesta real, con mocks de Boot Services y telemetría. GCC del host compila con `-Wall -Wextra -Werror`; se omite solamente el aviso de parámetros sin usar que ya existen en la firma EFI. `tests/RESULT.txt` contiene ocho PASS:

- Duplicados no consultan ni redibujan.
- Quiesce libera la instancia del monitor y cierra eventos protegido por TPL.
- Callback en cola o tardío no redibuja.
- Segunda cesión no vuelve a cerrar eventos cerrados.
- Un tipo aún no observado no se incorpora ni dibuja después de ceder la pantalla.
- Error CloseEvent conserva el latch incluso cuando RestoreTPL despacha ese evento pendiente.
- Segunda cesión reintenta solamente el evento no cerrado.
- Parámetros inválidos no cambian propiedad/TPL/eventos.

`git apply --check --ignore-space-change changes.patch` pasa contra `original`. `MANIFEST.json` fija SHA256 originales y propuestos. Son validaciones del cambio y control de flujo; no prueban servicios firmware reales ni ejecución física.

## Integración pendiente

El agente raíz integrará diagnóstico de BdsEntry, conexión de backend físico y StartImage. Esta propuesta no transforma `gST->ConOut != NULL` ni un OutputString exitoso en prueba de consola física: ConSplitter puede publicar una interfaz virtual vacía y devolver éxito. Tampoco confirma la hipótesis de panel superpuesto de la fotografía anterior. La compilación ARM, auditoría de binarios y construcción de candidato distinto siguen pendientes fuera de esta subtarea.
