# Detección TypeC y apagado para reconexión V3

Implementación local aislada, 26 septiembre 2026. No modifica los árboles V1/V2
ni las imágenes congeladas. Conserva la secuencia de encendido, el criterio
ATTACHED_SRC=5, los bloqueos de VBUS y los fallos que impiden volver a encender.
Añade una separación entre apagar el host para observar una reconexión y
restaurar por completo el detector al terminar.

## Contrato

`Library/MotoG20UsbPlatformLib.h` define `MOTO_G20_USB_ROLE_DIAGNOSTIC`, revisión 2,
y `MotoG20UsbPlatformGetRoleDiagnostic`. El getter copia el caché bajo
TPL_NOTIFY; no hace MMIO. Un puntero nulo devuelve EFI_INVALID_PARAMETER.
Antes de preparar el hardware devuelve un caché con máscaras/counters cero
y RoleReadStatus, SnapshotStatus, CalibrationStatus y LastStopStatus=EFI_NOT_READY.

- `Samples`: intentos de ReadRole.
- `SeenStates`: bitmap de estados de STATUS leídos correctamente por ReadRole.
- `SourceSeen`: cantidad de esos muestreos con estado5, incluso si EIC falla.
- `BoostAttempts`: intentos REG01=0x3A, contados antes de la escritura incierta.
  No incluye feeds0x7A ni apagados0x5A.
- `SnapshotAttempts` y `SnapshotFailures`: barridos de diagnóstico realizados
  y barridos con al menos una lectura ADI fallida.
- `RoleReadStatus`: resultado del último ReadRole, independiente de que el rol
  correcto habilite finalmente host.
- `SnapshotStatus`: primer error ADI del último barrido, o EFI_SUCCESS.
- `ValidMask/ErrorMask`: bits0..6 corresponden respectivamente a
  EN, MODE, INT_EN, RAW, MASK, STATUS y RTRIM. Un campo fallido vale cero en el
  almacenamiento, pero **ese cero no es una lectura válida**. El consumidor
  debe imprimirlo como inválido. Máscara cero con SnapshotStatus=NOT_READY
  significa que todavía no se ha intentado un barrido.
- `EfuseData/EfuseValid`, `RequestedTrim/TrimApplied`, `CalibrationStatus`
  registran el último intento de calibración. `TrimApplied` significa que la
  escritura y readback de ese intento fueron correctos; un Stop posterior
  puede restaurar el valor anterior. Son datos históricos, no lecturas vivas.
- `DriverActive` y `BoostRequested`: propiedad actual del driver, copiada bajo
  el mismo TPL que el resto del estado. No son mediciones de voltaje. Después
  de un apagado incierto, BoostRequested sigue a TRUE hasta confirmar OFF.
- `StopAttempts`: entradas a DisablePower, incluidas llamadas repetidas que
  ya estaban apagadas. No cuenta exclusivamente escrituras físicas OFF.
- `LastStopStatus`: resultado del último DisablePower o del Stop completo,
  incluyendo un error posterior al restaurar el detector. Puede ser SUCCESS
  después de limpiar un fallo anterior que sigue bloqueando el encendido.

`MotoG20UsbPlatformQuiesceForRetry` requiere que HC detenga primero sus accesos
y SESSION. Cancela el monitor de potencia, solicita OFF y restaura PHY/I2C,
pero conserva el detector calibrado y las dos entradas de infraestructura ADI.
Un Running/SUCCESS pasa a Stopped/NOT_READY cuando el apagado acaba bien.
Los errores primarios anteriores no se convierten en éxitos. Una restauración
fallida conserva sus recursos pendientes y bloquea nuevos encendidos.

`MotoG20UsbPlatformDetectRole` lee STATUS y EIC de nuevo, bajo TPL_NOTIFY, sólo
con el detector preparado, inactivo, sin boost pendiente y sin fallo. No
escribe registros PMIC, no toca el controlador MUSB, no alimenta watchdog y
no enciende boost. Los dos valores de salida se publican únicamente cuando
ambas lecturas terminan bien; un error conserva ambos valores del llamador
y bloquea nuevos encendidos. Los getters continúan siendo sólo de caché.

Un `Init` explícito es el único camino que vuelve a energizar el host. Esta
biblioteca no decide cuándo reintentar: los límites y la observación de una
desconexión estable pertenecen al controlador HC. `Stop`/`Shutdown` siguen
restaurando también MODE, INT_EN, RTRIM, EIC y la infraestructura inicial.

Un barrido se toma al primer ReadRole y al cambiar el estado o resultado de
lectura. Un error de preparación con transporte ya habilitado también puede
capturar un primer barrido. Un estado y resultado estables no causan barridos
adicionales; tampoco lo hace el getter. Los errores de diagnóstico quedan en
caché y no provocan reintentos completos cada1ms. El barrido secuencial no es
atómico: TypecStatus puede ser posterior al muestreo que lo disparó. Los counters
SeenStates/SourceSeen no incluyen esa lectura adicional del barrido.

## Registros y procedencia

Fuente fijada: Motorola kernel-sprd
`261390cdfff66efef9082bd70c1af0abd21d77f2`,
`PRIMARY_SOURCES/sc27xx_typec.c:24-31`:

| Campo | Dirección PMIC |
|---|---|
| EN | 0x0380 |
| MODE | 0x0384 |
| INT_EN | 0x038C |
| INT_RAW | 0x0394 |
| INT_MASK | 0x0398 |
| STATUS | 0x039C |
| RTRIM | 0x03BC |

La telemetría y DetectRole sólo leen estos registros mediante el transporte
ADI existente. **No se escribe ni se reconoce INT_CLR0x0390.** Su necesidad para
el fallo físico observado sigue sin demostrarse.

La lectura eFuse y la escritura de calibración ya existían; ahora se registran
sus resultados. Para SC2730, el cálculo es
`((data >> 5) & 31) | ((data & 31) << 5)`, aplicado sólo a los10bits de RTRIM.
El caso de prueba0x31EE→0x01CF procede de la lectura Android aportada por root,
`outputs/android_typec_20260925T203723Z/SUMMARY.json` y
`cc_calib_block13.bin`. El valor no se fija en la implementación.

## Verificación

Ejecutar `Tests/run.ps1`; usa el source C de producción con transporte simulado,
`gcc -std=c11 -Wall -Wextra -Werror`. Resultado: 35 escenarios PASS, incluidos los
24 de V2 y 11 de apagado/detección V3. Se comprueban getter sin MMIO,
validación explícita de campos fallidos, no repetir barridos por cada muestreo,
historial DRP9/3/0, conexión después de10min de espera simulada, error de lectura
sin inventar estado0, error eFuse, intento incierto de boost y preservación de
bits RTRIM ajenos al cálculo. Los casos V3 comprueban apagado sin estado activo
falso, conservación del detector, muestras frescas sin escrituras PMIC ni HC,
reintento explícito, salida intacta al fallar STATUS o EIC, guardas de propiedad
y fallo, propagación de errores OFF/PHY/timer/restauración, restauración final,
contadores reales y preservación del TPL del llamador en éxito y error.

Esto verifica comportamiento del modelo host. No demuestra valores MMIO reales,
VBUS físico, compatibilidad del hub, enumeración ni entrada de teclado en UEFI.
La compilación AARCH64 y prueba física corresponden a la integración posterior.
