"""Pin public Motorola reference sources and inspect saved kernel configuration."""
import concurrent.futures
import hashlib
import json
import urllib.request
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'outputs/microsd_v1_20261002'
COMMIT = '261390cdfff66efef9082bd70c1af0abd21d77f2'
BASE = f'https://raw.githubusercontent.com/MotorolaMobilityLLC/kernel-sprd/{COMMIT}/'
PATHS = [
    'drivers/mmc/host/sprd-sdhcr11.c', 'drivers/mmc/host/sprd-sdhcr11.h',
    'drivers/mmc/host/sdhci.h', 'drivers/mmc/core/slot-gpio.c',
    'drivers/gpio/gpio-eic-sprd.c', 'drivers/gpio/gpio-sprd.c',
    'drivers/regulator/sc2730-regulator.c', 'drivers/clk/sprd/mux.c',
    'drivers/clk/sprd/sharkl5pro-clk.c',
    'include/dt-bindings/clock/sprd,sharkl5pro-clk.h',
    'drivers/clk/sprd/gate.c', 'drivers/clk/sprd/gate.h',
    'include/linux/mmc/mmc.h', 'drivers/mmc/core/host.c',
    'drivers/pinctrl/sprd/pinctrl-sprd.c', 'drivers/pinctrl/sprd/pinctrl-sprd.h',
    'drivers/pinctrl/sprd/pinctrl-sprd-sharkl5Pro.c',
]


def fetch(relative):
    url = BASE + relative
    target = OUT / 'PRIMARY_SOURCES' / relative.replace('/', '_')
    with urllib.request.urlopen(url, timeout=30) as response:
        data = response.read(2 * 1024 * 1024)
    assert len(data) < 2 * 1024 * 1024
    if target.exists(): assert target.read_bytes() == data, f'Source changed: {target}'
    else: target.write_bytes(data)
    return {'upstream_path': relative, 'url': url, 'sha256': hashlib.sha256(data).hexdigest().upper(),
            'size': len(data), 'local_path': str(target)}


def main():
    (OUT / 'PRIMARY_SOURCES').mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        rows = list(pool.map(fetch, PATHS))
    kernel = Path(r'C:\MOTO_G20_WOA_LAB\06_BOOT_ANALYSIS\aosp_unpack\kernel')
    raw = kernel.read_bytes()
    start = raw.index(b'IKCFG_ST') + 8
    config = zlib.decompress(raw[start:], 16 + zlib.MAX_WBITS).decode()
    selected = [line for line in config.splitlines() if any(term in line for term in
                ('CONFIG_MMC', 'CONFIG_SPRD_REG_ACCESS', 'CONFIG_KEXEC', 'CONFIG_EFI', 'CONFIG_SPRD_GPIO'))]
    result = {'status': 'PINNED_SOURCE_BYTES_VERIFIED', 'commit': COMMIT, 'sources': rows,
              'kernel_sha256': hashlib.sha256(raw).hexdigest().upper(),
              'saved_kernel_config': selected, 'device_io': 'NONE'}
    (OUT / 'SOURCE_MANIFEST.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'files': len(rows), 'config': selected}, indent=2))


if __name__ == '__main__': main()
