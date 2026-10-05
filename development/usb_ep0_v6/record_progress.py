"""Record V6 progress from artifacts; no phone IO or registry mutation."""
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAB = Path(r'C:\MOTO_G20_WOA_LAB')
ROOT = LAB / 'AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260929_V6'
OUT = HERE.parents[1] / 'outputs/usb_ep0_v6_20260929'
OUT.mkdir(exist_ok=True)

def read(path):
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else None

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper() if path.exists() else None

validation = read(ROOT / 'REPORTS/VALIDATION_V6.json')
audit = read(ROOT / 'REPORTS/USB_CANDIDATE_HOST_AUDIT.json')
candidate = audit['artifacts']['MOTO_G20_USB_HOST_HOST_ONLY.img']['sha256'] if audit else None
attempt = LAB / 'AUTOMATION/RESULTS' / ('USB_HOST_PHYSICAL_ATTEMPT1_' + candidate[:12]) if candidate else None
claims = LAB / 'AUTOMATION/STATE' / (attempt.name + '_EXECUTION_CLAIMS') if attempt else None
dispatch = read(attempt / '08_DISPATCHED_ONCE.json') if attempt else None
stage = 'SOURCE_IN_PROGRESS_NOT_BUILT'
if validation: stage = 'HOST_TESTS_PASS_BUILD_OR_PACKAGING_PENDING'
if audit: stage = 'CANDIDATE_PACKAGED_NOT_DISPATCHED'
controller_test = read(ROOT / 'PHYSICAL_CONTROLLER/TEST_OBSERVATION.json')
if controller_test and controller_test['status'] == 'PASS_SIMULATED_CONTROLLER_ONLY':
    stage = 'CONTROLLER_VALIDATED_NOT_DISPATCHED'
if attempt and (attempt / '04_FLASH_READBACK_PASS.json').exists(): stage = 'FLASHED_FULL_READBACK_PASS_NOT_DISPATCHED'
if dispatch: stage = 'DISPATCHED_ONCE_AWAITING_USER_OBSERVATION'
registry = LAB / 'AUTOMATION/STATE/candidate_execution_registry.jsonl'
value = {'utc': datetime.now(timezone.utc).isoformat(), 'status': stage,
    'build_root': str(ROOT), 'candidate_sha256': candidate,
    'manifest_sha256': sha(ROOT / 'FREEZE_MANIFEST.csv'),
    'controller_sha256': sha(HERE / 'usb_physical_once.py'),
    'attempt': str(attempt) if attempt else None, 'claims': str(claims) if claims else None,
    'dispatch': dispatch, 'validation': validation,
    'registry_sha256': sha(registry), 'registry_events': len(registry.read_bytes().splitlines()),
    'physical_keyboard_result': 'NOT_VERIFIED', 'battery_percent_minimum': None,
    'v5_observation': str(HERE.parents[1] / 'outputs/usb_v5_observada_20260928/OBSERVATION.json'),
    'v5_recovery': str(HERE.parents[1] / 'outputs/recuperacion_usb_20260929T214100Z/RECOVERY.json'),
    'device_io_by_this_recorder': 'NONE'}
(OUT / 'ESTADO.json').write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
report = f'''<!-- USB_V6_CURRENT_BEGIN -->
# Estado vigente: USB V6, causa de fallo EP0 y mensajes tras desconectar

UTC: {value['utc']}. Estado: {stage}.
Imagen: {candidate}. Manifiesto: {value['manifest_sha256']}.
Controlador: {value['controller_sha256']}. Intento: {value['attempt']}.
Registro: {value['registry_events']} eventos, SHA {value['registry_sha256']}.

V5 E1E431DA consumida y observada: LED permaneció encendido según usuario;
ninguna tecla. SET_ADDRESS(1), dirección0, SETUP, SYSTEM0x100, sin interfaces.
No confundir el estado tras desconectar con el instante del fallo. Evidencia:
{value['v5_observation']}
Android B posterior verificado, bootA V5 y bootB seguro, misc normal/CRC,
registro intacto. Mecanismo de reinicio no establecido; pstore pequeño de
Android no demuestra un fallo UEFI. Recuperación: {value['v5_recovery']}

V6 distingue las salidas de WaitEp0 con muestras previas a limpieza y validez;
no MMIO tras perder acceso. Muestra histórica adicional al completar reset raíz.
Observer corrige espera desacoplada de HcPublished; no inventa Shell> ni lee
teclas. PHY, transferencias y tiempos conservados. Reintentos sin límite
vitalicio, desconexión estable500ms, plazo1100ms y apagado fatal conservados.

Pruebas host: {validation['tests'] if validation else 'pendientes'}.
Modelos/compilación no prueban teclado físico. No mínimo de batería.
Antes de mutar el teléfono revisar journal y claims; nunca repetir candidatos
consumidos o etapas reclamadas. Seguir prepare, flash/readback, boot una vez.
Tras despacho probar hub+teclado en MISMO arranque, help+Enter y foto de líneas
last failure/EP0/root reset/inventory. Avisar si requiere reinicio forzado.
Herramientas: {HERE}
Informe: {OUT / 'ESTADO.json'}
<!-- USB_V6_CURRENT_END -->
'''
(OUT / 'INFORME.md').write_text(report, encoding='utf-8')
for path in (LAB / 'LEER_PARA_CONTINUAR.md', LAB / 'HANDOFF/RESUME_USB_20260925.md',
             LAB / 'HANDOFF/RESUME_CONSOLE_DIAGNOSTIC_20260914.md'):
    old = path.read_text(encoding='utf-8')
    start, end = '<!-- USB_V6_CURRENT_BEGIN -->', '<!-- USB_V6_CURRENT_END -->'
    if start in old:
        assert old.count(start) == old.count(end) == 1
        a, b = old.index(start), old.index(end) + len(end)
        old = old[:a] + old[b:]
    path.write_text(report + '\n' + old.lstrip(), encoding='utf-8')
print(json.dumps({k: value[k] for k in ('status', 'candidate_sha256', 'manifest_sha256')}, indent=2))
