# Proveedor microSD SD0, primera prueba de hardware

Esta capa implementa `MOTO_G20_SD_PLATFORM.Prepare` para el controlador extraíble `0x71100000`. No constituye evidencia de funcionamiento físico: todavía hace falta verificar CMD8/CID/CSD y leer el marcador desde la tarjeta. No contiene comandos de escritura/borrado de SD, referencias a eMMC/Wi-Fi, ni código USB.

## Secuencia

1. DXE registra sólo las ocho regiones MMIO necesarias como UC, rechaza conflictos con RAM y comprueba los descriptores después de configurar sus atributos. No realiza MMIO antes de completar todo el mapa.
2. Activa ADI y EIC mediante alias SET y conserva los bits originales. GPIO19 del EIC SYNC se lee como presente alto y se comprueba nuevamente tras 1 ms. No configura DMSK de la variante debounce.
3. Activa SD0 AHB/32k y gates AON 1x/2x, apaga primero el reloj de salida del controlador, selecciona ext-26 MHz sin cambiar PLL y aplica reset ALL con límite de 100 ms.
4. Guarda los 12 registros pinmux COMMON/MISC heredados (CMD/D0/D1/CLK/D2/D3). **No modifica ni certifica su selección de función/pulls/drive.** Valores todo-uno se rechazan; otros valores quedan como evidencia de una hipótesis que debe resolver la identificación real de la tarjeta.
5. ADI usa sondeos acotados a 1–2 ms y verifica eco de dirección. Conserva sólo PD bit0 y VSEL bits7:0 de SDCORE/SDIO. Si no estaban ya a 3 V habilitados, apaga ambos, programa selector 0xb4 y los habilita en orden core → 200 us → IO → 5 ms. Cada escritura se lee de vuelta. Conserva/restaura la clave de protección; no afirma que una lectura de la clave pruebe su desbloqueo.
6. Configura protección hotplug, polaridad de CD y debounce 3 conservando otros bits. `detect_pol` bit2 se establece: Motorola usa los flags crudos `cd-gpios=0`, distintos de `MMC_CAP2_CD_ACTIVE_HIGH` usado para interpretar EIC. Configura bus1, PIO, SDR12/3 V, timeout hardware 0xe, DLL apagada, delay0 y CMD/POSRD invertidos para legacy. No programa PowerControl SDHCI, marcado no utilizado por Motorola.
7. Div17 desde 26 MHz da 382.352 Hz. Espera estabilidad interna como máximo 100 ms y habilita salida; no publica configuración lista si falla una comprobación. El núcleo separado todavía debe identificar la tarjeta antes de publicar BlockIo.

`access_ready` comprueba gates y mux antes de leer el reloj SDHC. Los callbacks sólo exponen offsets alineados dentro de SD0. Se enclava una retirada observada; el DXE muestrea únicamente EIC cada 50 ms sin imprimir nada, además de comprobar presencia durante las operaciones. Una retirada/reinserción más breve que el muestreo puede no observarse: esto no es una garantía física de detección infinita. El driver no intenta reconectar en este prototipo.

En fallo y ExitBootServices se deshabilitan señales/estado de interrupción y reloj, se restauran los campos propios de rails, clave, mux, hotplug y gates en orden inverso. No se restaura una sesión DMA heredada. Si ADI perdió su gate no se intenta acceder a ese periférico para limpiar: se informa restauración incompleta. Los mapas GCD UC permanecen registrados; retirarlos mientras otros componentes los usan sería una acción diferente a restaurar registros.

## Diagnóstico

`MotoG20SdPlatformDiagnostic.h`, GUID `153ab53f-4e1c-475e-89a9-12441d7c3e62`, revisión1. Incluye estado EFI de mapa/preparación/salida y snapshot portable: etapa, código `sdro_status`, último registro/máscara esperado/valor leído, tiempo acumulado de esperas, rails antes/después, pinmux heredado, GPIO, mux, clock, timeout, versión/capacidades SDHC y DLL. Las etapas se definen en `sd_platform_engine.h`; READY=11 sólo significa preparación de plataforma, no tarjeta ni filesystem confirmados.

INF `MotoG20SdPlatformDxe.inf`, FILE_GUID `6311f7e2-0881-4d02-9dde-1fe254b62090`. Integrar junto con `MotoG20SdPlatform.h` del driver y `sdhci_ro.h` del núcleo en el mismo directorio del módulo. Añadir el componente al DSC y después su EFI a la FV. Necesita BaseMemoryLib, IoLib, TimerLib, UefiBootServicesTableLib, DxeServicesTableLib y UefiDriverEntryPoint. El proveedor instala protocolo y diagnóstico sin tocar hardware: el primer `Prepare` ejecuta la secuencia.

## Pruebas

`run_tests.sh <directorio-salida>`: 25 escenarios host, ASAN/UBSAN, advertencias estrictas y compilación freestanding AArch64. Incluyen mapa fallido sin MMIO, ausencia de tarjeta, gates/mux fallidos, reset y reloj atascados, ADI timeout/eco/readback, preservación de rails/clave/campos ajenos, pinmux sin modificaciones, retirada enclavada, offsets inválidos, pérdida de gate antes de MMIO y rollback imposible con ADI inaccesible. Esto modela el contrato de acceso y la secuencia; no verifica características físicas del bus, PIO, pins, reloj o tarjeta.

Resultados iniciales: `outputs/microsd_v1_20261002/PLATFORM_TESTS/platform_tests.txt` y hashes adyacentes. Fuentes de registros: los 17 archivos Motorola fijados al commit `261390cdfff66efef9082bd70c1af0abd21d77f2` del manifiesto existente y el DT real guardado. La adaptación se conserva como GPL-2.0-only conforme a las fuentes utilizadas.
