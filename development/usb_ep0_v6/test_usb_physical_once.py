"""Failure injection against a fake transport: no device commands are executed."""
import tempfile
import unittest
from pathlib import Path
from datetime import timedelta
from unittest.mock import patch
import usb_physical_once as c

class FailureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.attempt = root / 'attempt'
        self.state = root / 'state'
        self.attempt.mkdir()
        self.state.mkdir()
        self.claim = self.state / (c.NEW_SHA + '.claim.json')
        self.consumed = self.state / (c.NEW_SHA + '.dispatch-consumed.json')
        self.patches = [patch.object(c, 'ATTEMPT', self.attempt), patch.object(c, 'STATE', self.state),
            patch.object(c, 'CLAIM', self.claim), patch.object(c, 'CONSUMED', self.consumed),
            patch.object(c, 'host', return_value={}), patch.object(c, 'live', return_value={'battery_percent': 70})]
        for p in self.patches:
            p.start()
        self.actions = []

    def tearDown(self):
        for p in reversed(self.patches):
            p.stop()
        self.temp.cleanup()

    def ready(self):
        c.once(self.attempt / '00_READY.json', {'utc': c.now(), 'controller_sha256': c.sha(Path(c.__file__).read_bytes())})

    def boot_ready(self):
        c.once(self.claim, {'candidate_sha256': c.NEW_SHA, 'attempt_root': str(self.attempt),
                           'controller_sha256': c.sha(Path(c.__file__).read_bytes())})
        c.once(self.attempt / '04_FLASH_READBACK_PASS.json', {})

    def test_flash_timeout_cannot_retry(self):
        self.ready()
        def fake_run(label, argv, **kwargs):
            self.actions.append(label)
            if label == 'flash_boot_a':
                self.assertTrue((self.attempt / '02_FLASH_BOOT_A.intent.json').exists())
                raise TimeoutError('Delivery unknown')
            return '', 0
        with patch.object(c, 'run', side_effect=fake_run), patch.object(c, 'wait_fastboot'), patch.object(c, 'preflash_basis'):
            with self.assertRaises(TimeoutError): c.flash()
            with self.assertRaisesRegex(RuntimeError, 'already claimed'): c.flash()
        self.assertEqual(self.actions.count('flash_boot_a'), 1)
        self.assertNotIn('fastboot_reboot_b', self.actions)

    def test_misc_timeout_prevents_reboot_and_rewrite(self):
        self.boot_ready()
        def fake_run(label, argv, **kwargs):
            self.actions.append(label)
            return '', 0
        def fake_shell(label, command, **kwargs):
            if label == 'remote_one_hash': return c.ONE_SHA + ' file'
            if label == 'arm_misc':
                self.actions.append(label)
                self.assertTrue((self.attempt / '05_ARM_MISC.intent.json').exists())
                raise TimeoutError('Write may have completed')
            raise AssertionError(label)
        with patch.object(c, 'run', side_effect=fake_run), patch.object(c, 'shell', side_effect=fake_shell), patch.object(c, 'misc_read', return_value=c.NORMAL_BYTES):
            with self.assertRaises(TimeoutError): c.boot()
            with self.assertRaisesRegex(RuntimeError, 'already armed'): c.boot()
        self.assertEqual(self.actions.count('arm_misc'), 1)
        self.assertNotIn('test_reboot', self.actions)

    def test_reboot_timeout_is_consumed_and_not_retried(self):
        self.boot_ready()
        def fake_run(label, argv, **kwargs):
            self.actions.append(label)
            if label == 'test_reboot':
                self.assertTrue(self.consumed.exists())
                raise TimeoutError('Reboot delivery unknown')
            return '', 0
        def fake_shell(label, command, **kwargs):
            return c.ONE_SHA + ' file' if label == 'remote_one_hash' else ''
        with patch.object(c, 'run', side_effect=fake_run), patch.object(c, 'shell', side_effect=fake_shell), patch.object(c, 'misc_read', side_effect=[c.NORMAL_BYTES,c.ONE_BYTES]):
            with self.assertRaises(TimeoutError): c.boot()
            with self.assertRaisesRegex(RuntimeError, 'already armed'): c.boot()
        self.assertEqual(self.actions.count('test_reboot'), 1)


    def fastboot_claim(self, age_seconds=0, update=None):
        stamp = (c.datetime.now(c.timezone.utc) - timedelta(seconds=age_seconds)).isoformat()
        prior = {'utc': stamp, 'serial': c.SERIAL,
            'boot_sha256': {'boot_a': c.OLD_SHA, 'boot_b': c.SAFE_B},
            'properties': {'ro.boot.slot_suffix': '_b', 'sys.boot_completed': '1'},
            'misc_hex': c.NORMAL_BYTES.hex(), 'links': {'boot_a': '/dev/block/mmcblk0p43',
                'boot_b': '/dev/block/mmcblk0p44', 'misc': '/dev/block/mmcblk0p3'},
            'battery_percent': 70, 'temperature_c': 30, 'usb_powered': True}
        if update: prior.update(update)
        c.once(self.claim, {'candidate_sha256': c.NEW_SHA, 'attempt_root': str(self.attempt),
            'controller_sha256': c.sha(Path(c.__file__).read_bytes()), 'authorization': c.AUTH,
            'utc': stamp, 'live_before': prior})

    def fastboot_reply(self, reply):
        def fake_run(label, argv, **kwargs):
            if label == 'fastboot_devices': return c.SERIAL + '\tfastboot', 0
            if label == 'fastboot_slot':
                self.assertTrue(kwargs.get('combined'))
                return reply, 0
            raise AssertionError(label)
        with patch.object(c, 'run', side_effect=fake_run): c.wait_fastboot()

    def test_known_fastboot_blank_field_with_real_stderr_is_recorded(self):
        self.fastboot_claim()
        self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')
        self.assertTrue((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())
        observation = c.json.loads((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').read_text())
        self.assertEqual(observation['slot_reported_by_fastboot'], 'UNAVAILABLE')
        self.assertEqual(observation['claim_sha256'], c.sha(self.claim.read_bytes()))
        self.assertLessEqual(observation['preclaim_age_seconds'], 900)

    def test_reported_slot_b_is_accepted(self):
        self.fastboot_claim()
        self.fastboot_reply('current-slot: b\r\nFinished. Total time: 0.000s\r\n')
        self.assertFalse((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())

    def test_reported_slot_a_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'slot other than B'):
            self.fastboot_reply('current-slot: a\nFinished. Total time: 0.000s\n')

    def test_empty_malformed_and_conflicting_replies_are_rejected(self):
        for reply in ('', 'Finished. Total time: 0.000s', 'current-slot b',
                      'current-slot: b\ncurrent-slot: a', 'current-slot: \ncurrent-slot: b'):
            with self.subTest(reply=reply):
                with self.assertRaisesRegex(RuntimeError, 'Malformed or conflicting'):
                    self.fastboot_reply(reply)
        self.assertFalse((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())

    def test_blank_field_without_claim_is_rejected(self):
        with self.assertRaises(FileNotFoundError):
            self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')
        self.assertFalse((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())

    def test_blank_field_with_stale_claim_is_rejected(self):
        self.fastboot_claim(age_seconds=901)
        with self.assertRaisesRegex(RuntimeError, 'older than 15 minutes'):
            self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')
        self.assertFalse((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())

    def test_blank_field_with_wrong_preclaim_hash_is_rejected(self):
        self.fastboot_claim(update={'boot_sha256': {'boot_a': c.NEW_SHA, 'boot_b': c.SAFE_B}})
        with self.assertRaisesRegex(RuntimeError, 'Preclaim hash mismatch'):
            self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')
        self.assertFalse((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())

    def test_blank_field_with_future_claim_is_rejected(self):
        self.fastboot_claim(age_seconds=-60)
        with self.assertRaisesRegex(RuntimeError, 'timestamps out of order'):
            self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')

    def test_blank_field_with_low_charge_is_accepted_under_user_policy(self):
        self.fastboot_claim(update={'battery_percent': 0})
        self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')
        self.assertTrue((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())

    def test_phone_temperature_gate_still_applies(self):
        self.fastboot_claim(update={'battery_percent': 0, 'temperature_c': 43})
        with self.assertRaisesRegex(RuntimeError, 'temperature/USB gate'):
            self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')

    def test_usb_connection_gate_still_applies(self):
        self.fastboot_claim(update={'battery_percent': 0, 'usb_powered': False})
        with self.assertRaisesRegex(RuntimeError, 'temperature/USB gate'):
            self.fastboot_reply('current-slot: \nFinished. Total time: 0.000s\n')

if __name__ == '__main__':
    unittest.main(verbosity=2)
