"""Run simulated controller tests and preserve separate post-freeze evidence."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
root = Path(r'C:\MOTO_G20_WOA_LAB\AUTOMATION\BUILD_ARTIFACTS\USB_HOST_20260929_V6')
out = root / 'PHYSICAL_CONTROLLER'
assert not out.exists()
prep = json.loads((root / 'CONTROLLER_PREPARATION.json').read_text())
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest().upper()
assert sha(here / 'usb_physical_once.py') == prep['controller_sha256']
assert sha(here / 'test_usb_physical_once.py') == prep['tests_sha256']
run = subprocess.run([sys.executable, '-B', str(here / 'test_usb_physical_once.py')], capture_output=True, text=True)
out.mkdir()
(out / 'TEST_OUTPUT.txt').write_text(run.stdout + run.stderr, encoding='utf-8')
passed = run.returncode == 0 and 'Ran 14 tests' in run.stderr and '\nOK\n' in run.stderr
report = {'status': 'PASS_SIMULATED_CONTROLLER_ONLY' if passed else 'FAILED',
    'test_count': 14 if passed else None, 'exit_code': run.returncode,
    'controller_sha256': prep['controller_sha256'], 'tests_sha256': prep['tests_sha256'],
    'test_output_sha256': sha(out / 'TEST_OUTPUT.txt'), 'physical_result': 'NOT_TESTED'}
for name in ('usb_physical_once.py', 'test_usb_physical_once.py', 'validate_physical_controller.py'):
    (out / name).write_bytes((here / name).read_bytes())
(out / 'TEST_OBSERVATION.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, indent=2))
assert passed, run.stdout + run.stderr
