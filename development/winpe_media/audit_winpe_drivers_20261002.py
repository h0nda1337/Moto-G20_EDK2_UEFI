from pathlib import Path
import json, re, sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'work/winpe_audit_deps'))
from Registry import Registry

BASE = ROOT / 'outputs/winpe_internal_20261002'
SELECTED = BASE / 'wim_selected'
listing = (BASE / 'WIM_CONTENTS.txt').read_text(encoding='utf-8-sig', errors='replace')
paths = [l.removeprefix('Path = ') for l in listing.splitlines() if l.startswith('Path = ')]

def simple(v):
    x = v.value()
    return x.hex() if isinstance(x, bytes) else x

system = Registry.Registry(str(SELECTED / 'Windows/System32/config/SYSTEM'))
software = Registry.Registry(str(SELECTED / 'Windows/System32/config/SOFTWARE'))
result = {'registry': {}, 'services': [], 'infs': [], 'wim_paths': {}}
for name, hive, key in [
    ('Select', system, 'Select'), ('Setup', system, 'Setup'),
    ('ControlNlsLanguage', system, 'ControlSet001\\Control\\Nls\\Language'),
    ('ControlMUI', system, 'ControlSet001\\Control\\MUI\\UILanguages'),
    ('CurrentVersion', software, 'Microsoft\\Windows NT\\CurrentVersion'),
    ('Winlogon', software, 'Microsoft\\Windows NT\\CurrentVersion\\Winlogon')]:
    try:
        k = hive.open(key)
        result['registry'][name] = {'values': {v.name(): simple(v) for v in k.values()}, 'subkeys': [s.name() for s in k.subkeys()]}
    except Exception as e:
        result['registry'][name] = {'error': str(e)}
for s in system.open('ControlSet001\\Services').subkeys():
    result['services'].append({'name': s.name(), 'values': {v.name(): simple(v) for v in s.values()}})
terms = re.compile(r'unisoc|spreadtrum|ums512|sc986[03]|motorola|moto.?g20|sprd', re.I)
for path in SELECTED.rglob('*.inf'):
    data = path.read_bytes()
    content = data.decode('utf-16') if data[:2] in (b'\xff\xfe', b'\xfe\xff') else data.decode('utf-8-sig', errors='replace')
    hits = [{'line': i, 'text': line} for i, line in enumerate(content.splitlines(), 1) if terms.search(line)]
    name = path.name.lower()
    if hits or re.search(r'^(acpi|acpipmi|arm|sdbus|sff|sd|mmc|disk|partmgr|volume|vhd|stor|uefi|basicdisplay|basicrender|usbxhci|usb)', name):
        result['infs'].append({'path': str(path.relative_to(SELECTED)), 'hits': hits, 'content': content[:120000]})
result['wim_paths']['platform_term_matches'] = [p for p in paths if terms.search(p)]
result['wim_paths']['all_inf'] = [p for p in paths if p.lower().endswith('.inf')]
result['wim_paths']['mydrivers'] = [p for p in paths if p.lower().startswith('pematerial\\mydrivers')]
result['wim_paths']['startup'] = [p for p in paths if re.search(r'winxshell\.exe$|pecmd\.exe$|logonadmin|startnet\.cmd$|winpeshl\.ini$', p, re.I)]
(BASE / 'WIM_DRIVERS_STARTUP_RAW.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({'registry': result['registry'], 'service_count': len(result['services']), 'storage_platform_services': [s for s in result['services'] if re.search(r'acpi|sd|mmc|arm|disk|stor|vhd|partmgr|uefi|sprd|unisoc|ums|sc986', s['name'], re.I)], 'platform_inf_hits': [{'path': i['path'], 'hits': i['hits']} for i in result['infs'] if i['hits']], 'wim_paths': {k: v for k,v in result['wim_paths'].items() if k != 'all_inf'}, 'inf_count': len(result['wim_paths']['all_inf'])}, ensure_ascii=False, indent=2))
