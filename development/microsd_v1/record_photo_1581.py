"""Record the user's physical result, keeping host and device proof separate."""
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

BASE = Path(__file__).resolve().parents[2]
LAB = Path(r'C:\MOTO_G20_WOA_LAB')
OUT = BASE / 'outputs/microsd_v1_20261002'


def main():
    photo = Path(r'C:\Users\Angelo\Downloads\IMG_1581.jpeg')
    dest = OUT / 'PHOTO_1581'
    dest.mkdir(exist_ok=False)
    raw = photo.read_bytes()
    (dest / photo.name).write_bytes(raw)
    evidence = {'utc': datetime.now(timezone.utc).isoformat(),
                'source': str(photo), 'sha256': hashlib.sha256(raw).hexdigest().upper(),
                'candidate': 'B94E2C12D466AEE3BAC25E11AF853399EDA5132DCD36346A08ACE3C7A6BB37CB',
                'transcription': ['SDHW: stage=11 map=Success prepare=Success ready=1',
                    'SDHW: reg=7110002C expected=00000004 actual=00000004 cleanup=0',
                    'SDHW: CD=0000000B mux=00000001 clock=000E1107 timeout=14',
                    'SDHW: rails=0000/00B4/0000/00B4 elapsed-us=123235',
                    'SDHW: version=00010000 caps=1E5D0080 present=01F80000 DLL=00202000',
                    'SDHW: inherited pins CMD/D0/D1/CLK/D2/D3=0/0/0/0/0/0',
                    'MICROSD: stage=3 status=Success published=1',
                    'MICROSD: cmd=13 irq=00000001 response=00000900 commands=29 blocks=0 capacity=60456960',
                    'MICROSD: no filesystem (Not Found); read test not run.',
                    'Windows PE has NOT been started. Forced restart returns to Android.'],
                'verified': ['native SD platform preparation', 'card identification and capacity', 'BlockIo publication'],
                'not_verified': ['CMD17 sector read', 'FAT filesystem', 'marker file', 'Windows PE'],
                'important_limit': 'blocks=0 is printed BEFORE ConnectAll; failure counters were not printed after filesystem connection.',
                'user_followup': 'ya esta de nuevo por adb'}
    (dest / 'RESULT.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    old = (OUT / 'CURRENT_STATUS.json').read_bytes()
    (dest / 'PREVIOUS_STATUS.json').write_bytes(old)
    state = json.loads(old)
    state.update(state='MICROSD_V1_CARD_IDENTIFIED_NO_FILESYSTEM',
                 utc=evidence['utc'], phone_state='ANDROID_ADB_CONFIRMED_AFTER_USER_RESET',
                 physical_sd_uefi_read='CARD_IDENTIFIED_BLOCKIO_PUBLISHED_SECTOR_READ_NOT_VERIFIED',
                 photo_result=str(dest / 'RESULT.json'),
                 next='Diagnose storage driver dispatch/connection and capture raw read + post-connect status; continue RAM/ACPI/Windows loader offline.')
    (OUT / 'CURRENT_STATUS.json').write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
    note = f'''# Resultado físico microSD V1: tarjeta identificada, falta filesystem

Actualización 2026-10-04, foto IMG_1581. Candidato `{evidence['candidate']}` consumido; NO repetir.
SDHW stage11/ready1/Success; BlockIo stage3/published1/Success y capacidad60456960.
El paso filesystem devolvió NotFound. NO afirmar lectura de sectores, marcador o arranque WinPE.
El contador blocks0 se imprimió ANTES ConnectAll: falta diagnóstico después de conectar.
Usuario hizo reinicio forzado y ADB del serial ZY32CVND77 volvió a confirmarse.

Prueba: boot_a escrito una vez y hash verificado desde Android B; boot_b conserva hashC0CF...DF7F3.
Misc32bytes armados/readbackuna vez; reboot de prueba una vez. Intento:
`C:/MOTO_G20_WOA_LAB/AUTOMATION/RESULTS/MICROSD_PHYSICAL_ATTEMPT1_B94E2C12D466`.
Imagen y365archivos congelados: `G:/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_CANDIDATE_05`.
Build05: proveedor nativo, SDcore, BDSdiagantesShell, DiskIo/Partition/FAT. HotplugBIT2 corregido.
Fuentes dev: `work/microsd_v1`; evidencia: `outputs/microsd_v1_20261002/PHOTO_1581/RESULT.json`.
WinPE aún no iniciado. Seguir FAT/rawreads, memoria, ACPI/SMBIOS y carga automática.
USB sigue pausado. La SD FAT32 C003-08EC con681archivos ya verificados NO se reformatea.
Los18eventos del registro histórico se complementan con claims deV6 ySDV1; consultar ambos.

'''
    handoff = LAB / 'HANDOFF/RESUME_MICROSD_20261002.md'
    prior = handoff.read_text(encoding='utf-8')
    handoff.write_text(note + '---\nHistorial anterior:\n\n' + prior, encoding='utf-8')
    pointer = LAB / 'LEER_PARA_CONTINUAR.md'
    text = pointer.read_text(encoding='utf-8')
    start, end = text.index('<!-- MICROSD_CURRENT_BEGIN -->'), text.index('<!-- MICROSD_CURRENT_END -->') + len('<!-- MICROSD_CURRENT_END -->')
    pointer.write_text(text[:start] + '<!-- MICROSD_CURRENT_BEGIN -->\n' + note + '<!-- MICROSD_CURRENT_END -->' + text[end:], encoding='utf-8')
    print(state['state'])


if __name__ == '__main__':
    main()
