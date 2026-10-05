"""Protect the actual boot path: Shell must not swallow the SD read proof."""
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import integrate_workspace as integration


class IntegrationTests(unittest.TestCase):
    def test_probe_precedes_shell_and_result_is_held(self):
        _, source = integration.sd_diagnostic_boot_policy()
        callback = source[source.index('PlatformBootManagerAfterConsole ('):]
        probe = callback.index('  MotoG20SdProbe ();')
        shell = callback.index('  Status = LaunchBuiltInShell ();')
        self.assertLess(callback.index('SetWatchdogTimer (0, 0, 0, NULL)'), probe)
        self.assertLess(probe, callback.index('capture this screen.'))
        self.assertLess(callback.index('for (;;)'), shell)
        self.assertEqual(source.count('  MotoG20SdProbe ();'), 1)

    def test_plan_has_provider_and_no_unreachable_bds_probe(self):
        plan = integration.plan_sources(integration.ROOT)
        by_name = {p.relative_to(integration.ROOT).as_posix(): b for p, b in plan.items()}
        prefix = 'WORKSPACE/MotoG20Pkg/'
        bds = by_name[prefix + 'Diagnostics/BdsDxe/BdsEntry.c'].decode()
        self.assertNotIn('MotoG20SdProbe ();', bds)
        dsc = by_name[prefix + 'Foundation/MotoG20MicroSd.dsc'].decode()
        self.assertEqual(dsc.count('MotoG20SdPlatformDxe.inf'), 1)
        self.assertNotIn('Usb', dsc)
        self.assertNotIn('USB', dsc)
        for name in ('MotoG20SdPlatformDxe.c', 'MotoG20SdPlatformDxe.inf',
                     'sd_platform_engine.c', 'sd_platform_engine.h',
                     'MotoG20SdPlatform.h', 'sdhci_ro.h'):
            self.assertIn(prefix + 'Drivers/MotoG20SdPlatformDxe/' + name, by_name)


if __name__ == '__main__':
    unittest.main()
