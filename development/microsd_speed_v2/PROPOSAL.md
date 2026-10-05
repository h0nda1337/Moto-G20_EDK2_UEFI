# Velocidad legacy posterior a la primera prueba SD

Separado de la imagen V1 a 382.352 Hz. Ningún archivo aquí cambia MMIO, protocolos, fuentes del candidato ni el teléfono.

El primer aumento propuesto conserva la fuente ext-26 MHz y usa divisor0, que en Unisoc entrega 13 MHz. Mantiene un bit, 3 V, SDR12, sin CMD6/ACMD6/CMD11, sin PLL nueva y sin DMA. Para 562.366.404 bytes, el límite ideal del enlace pasa de unos 196 minutos a 346 segundos; los comandos, sondeos y accesos MMIO aumentarán ambos tiempos. No son tiempos medidos.

`sd_plan_legacy_clock` ya implementa un cálculo puro. Sólo acepta un `sdro_host` identificado, seleccionado, sin fallo y aún a velocidad inicial. Contrasta OCR listo/3 V/CCS, CSD y capacidad. Decodifica TRAN_SPEED y command classes; limita el resultado a lo anunciado por la tarjeta, al objetivo y a 13 MHz. El CSD real guardado de SA32G declara 25 MHz, por lo que admite el plan de 13 MHz. Esto no demuestra estabilidad física a esa velocidad.

Referencia de CSD: [Motorola sd.c, commit fijado](https://github.com/MotorolaMobilityLLC/kernel-sprd/blob/261390cdfff66efef9082bd70c1af0abd21d77f2/drivers/mmc/core/sd.c), `tran_exp`, `tran_mant`, `mmc_decode_csd`. El proveedor host existente documenta el divisor de SDHC. El driver Motorola aplica delay legacy del DT sólo cuando el reloj solicitado es exactamente 25 MHz; este plan a 13 MHz conserva delay0 e inversiones CMD/POSRD actuales, pendiente de prueba.

## Cambio de API propuesto, todavía no aplicado

Añadir una revisión2 de protocolo plataforma con `SetTransferClock(This, TargetHz, ActualHz)` y `Stop(This)`. La revisión1 permanece válida para sólo 382 kHz. El BlockIo controla exclusión y llama al cambio únicamente después de CMD13=TRANSFER/READY y antes de publicar el handle; no entrega al proveedor comandos arbitrarios ni estructuras mutables de tarjeta.

El motor necesita guardar divisor y reloj activos: `access_ready` actualmente exige 0x1107 y debe compararlos con el valor confirmado, no relajarse a cualquier reloj. El cambio debe comprobar gates, CD e inhibitCMD/DAT, detener sólo SDCLK, configurar divisor, esperar INTCLK estable con límite, volver a habilitar, y mantener timeout0xe/3V/bus1/DLLoff. Ante fallo intentar restaurar exclusivamente el reloj inicial mientras los gates y CD sigan accesibles, y reportar fallo sin actualizar `host.clock_hz`.

Antes de cambiar leer y guardar MBR/BPB/marcador a 382 kHz. Después verificar CMD13 y releer esos sectores al reloj nuevo, comparar exactamente. Sólo entonces actualizar el reloj efectivo y comenzar lectura de WIM. Un fallo de transferencia a velocidad nueva invalida la sesión actual del núcleo; no debe ocultarse con un simple retorno a velocidad baja o borrar `faulted`. Se requerirá parada/reset/identificación limpia antes de otra sesión. La primera imagen V1 no incorpora este cambio.

Tests del plan independientes en `outputs/microsd_speed_v2/`: casos de CSD real, objetivos menores, límites por tarjeta, OCR inconsistente, CSD inválido, estado sin identificar, ausencia de class block-read y host inmutable. Falta implementar y probar la transición física después de validar V1.
