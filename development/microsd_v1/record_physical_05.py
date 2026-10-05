"""Record the dispatched SD test without claiming a screen or WinPE result."""
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAB = Path(r'C:\MOTO_G20_WOA_LAB')
BUILD = Path(r'G:\MOTO_G20_WOA_LAB\BUILD_ARTIFACTS\MICROSD_20261002_V1')
CANDIDATE = Path(r'G:\MOTO_G20_WOA_LAB\BUILD_ARTIFACTS\MICROSD_CANDIDATE_05')
ATTEMPT = LAB / 'AUTOMATION/RESULTS/MICROSD_PHYSICAL_ATTEMPT1_B94E2C12D466'
OUT = ROOT / 'outputs/microsd_v1_20261002'


def main():
    previous = json.loads((OUT / 'CURRENT_STATUS.json').read_text())
    assert previous['physical_sd_uefi_read'] == 'NOT_TESTED'
    dispatch = json.loads((ATTEMPT / '08_DISPATCHED_ONCE.json').read_text())
    readback = json.loads((ATTEMPT / '04_FLASH_READBACK_PASS.json').read_text())
    assert dispatch['candidate_sha256'] == readback['live']['boot_sha256']['boot_a']
    assert readback['live']['boot_sha256']['boot_b'] == 'C0CFBCF4574F20F4028D7A4C5EE201FAD2C28F841201F94A0F48DD5ED76DF7F3'
    assert dispatch['boot_a_flashes'] == dispatch['misc_writes'] == dispatch['test_reboots'] == 1
    state = {
        'state': 'MICROSD_V1_DISPATCHED_ONCE_AWAITING_SCREEN',
        'utc': datetime.now(timezone.utc).isoformat(), 'usb_work': 'PAUSED_BY_USER',
        'candidate_sha256': dispatch['candidate_sha256'], 'candidate_root': str(CANDIDATE),
        'edk2_build': str(BUILD / 'REPORTS/BUILD_05'), 'attempt': str(ATTEMPT),
        'physical_sd_uefi_read': 'AWAITING_OBSERVATION', 'winpe_boot': 'NOT_STARTED',
        'phone_state': 'REBOOT_DISPATCHED_SCREEN_NOT_YET_OBSERVED',
        'sd_card': previous['sd_card'], 'raw_sd_reference': previous['raw_sd_reference'],
        'platform_host_tests': 25, 'core_host_tests': 35, 'probe_host_tests': 31,
        'firmware_modules': 28, 'preserved_baseline_modules': 22,
        'retry': 'FORBIDDEN_FOR_CONSUMED_B94E_CANDIDATE',
        'safe_android_b_verified_after_flash': True,
        'source_provenance_limit': 'Custom sources and recorded artifacts frozen; shared library dependencies are not all independently frozen or rebuilt.',
        'next': 'Read the SDHW/MICROSD screen. Continue RAM/ACPI/SMBIOS and automatic Windows loader work separately.'}
    archive = OUT / 'STATUS_BEFORE_PHYSICAL_05.json'
    assert not archive.exists()
    archive.write_bytes((OUT / 'CURRENT_STATUS.json').read_bytes())
    (OUT / 'CURRENT_STATUS.json').write_text(json.dumps(state, indent=2) + '\n')
    note = f'''# Prueba física microSD V1 enviada; Windows PE aún no iniciado

UTC: {state['utc']}. Imagen `{state['candidate_sha256']}`.
Build05 incluye proveedor nativo completo y la corrección hotplug BIT2 verificada contra Motorola.
28 FFS, 22 preservados; USB ausente. Manifiesto de365 archivos en `{CANDIDATE}`.
Se escribió boot_a una vez; Android B volvió y ambos boot se leyeron/compararon.
Se armó misc una vez, se verificaron32 bytes y se envió un reinicio de prueba.
Evidencia: `{ATTEMPT}`. El claim/dispatch está consumido: NO repetir esta imagen.

Esperando pantalla SDHW/MICROSD; no afirmar identificación, FAT o marcador antes de verla.
La imagen retiene diagnóstico antes de Shell (watchdog desactivado si SetWatchdogTimer tiene éxito).
El usuario ya tiene pregunta pendiente. Mantener tarjeta/cable; no se solicitó reinicio forzado aún.
Windows no se intenta en V1. Desarrollo RAM/ACPI/SMBIOS y arranque directo sigue en
`work/winpe_platform_boot`; aumento legacy13MHz separado en `work/microsd_speed_v2`.

La tarjeta C003-08EC ya está FAT32 con681 archivos verificados: NO formatear otra vez.
Registro histórico tiene18 eventos; además consultar claims, que incluyen USB V6 y esta SDV1.
No modificar la evidencia congelada ni interpretar pruebas simuladas como éxito físico.

'''
    handoff = LAB / 'HANDOFF/RESUME_MICROSD_20261002.md'
    handoff.write_text(note + '---\n\nEstado anterior conservado como historial:\n\n' + handoff.read_text(encoding='utf-8'), encoding='utf-8')
    pointer = LAB / 'LEER_PARA_CONTINUAR.md'
    text = pointer.read_text(encoding='utf-8')
    start, end = text.index('<!-- MICROSD_CURRENT_BEGIN -->'), text.index('<!-- MICROSD_CURRENT_END -->') + len('<!-- MICROSD_CURRENT_END -->')
    block = '<!-- MICROSD_CURRENT_BEGIN -->\n' + note + '<!-- MICROSD_CURRENT_END -->'
    pointer.write_text(text[:start] + block + text[end:], encoding='utf-8')
    print(state['state'])


if __name__ == '__main__':
    main()
