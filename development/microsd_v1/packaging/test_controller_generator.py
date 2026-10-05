"""Pure rendering + inherited fake-transport failure injection, no device I/O."""
import ast
import io
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

import prepare_microsd_physical_controller as g

IDENTITY = 'F' * 64


def rendered_module():
    source = g.render_controller(g.BASE.read_text(), Path('C:/OFFLINE_TEST_ONLY'), IDENTITY, 'A' * 64, 8)
    module = types.ModuleType('microsd_physical_once')
    module.__file__ = str(Path(__file__).resolve())
    exec(compile(source, '<offline-rendered-controller>', 'exec'), module.__dict__)
    return source, module


class GeneratorTests(unittest.TestCase):
    def test_baseline_hashes(self):
        self.assertEqual(g.sha(g.BASE.read_bytes()), g.BASE_SHA)
        self.assertEqual(g.sha(g.BASE_TESTS.read_bytes()), g.BASE_TESTS_SHA)

    def test_render_has_correct_physical_parent_and_charge_policy(self):
        text, module = rendered_module()
        self.assertEqual(module.OLD_SHA, g.PHYSICAL_PARENT)
        self.assertEqual(module.NEW_SHA, IDENTITY)
        self.assertEqual(module.SAFE_B, g.SAFE_B)
        self.assertEqual(module.REGISTRY_SHA, g.REGISTRY_SHA)
        self.assertNotIn('level >= 70', text)
        self.assertIn('temp <= 420', text)
        self.assertIn('MICROSD_PHYSICAL_ATTEMPT1_', str(module.ATTEMPT))
        self.assertIn('MOTO_G20_MICROSD_HOST_ONLY.img', str(module.CANDIDATE))

    def test_render_preserves_dispatch_functions_except_authorized_constants(self):
        text, _ = rendered_module()
        before, after = ast.parse(g.BASE.read_text()), ast.parse(text)
        functions = lambda tree: {node.name: ast.dump(node, include_attributes=False) for node in tree.body if isinstance(node, ast.FunctionDef)}
        old, new = functions(before), functions(after)
        for function in ('flash', 'boot', 'wait_fastboot', 'misc_read', 'run', 'claim_value', 'wait_android', 'shell', 'once', 'intent'):
            with self.subTest(function=function):
                self.assertEqual(old[function], new[function])

    def test_identity_rejects_all_consumed_baselines(self):
        for value in (g.PHYSICAL_PARENT, g.SAFE_B, g.package.PARENT_SHA):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'parent/safe'):
                g.validate_identity(value, b'')

    def test_identity_rejects_ledger_and_malformed(self):
        with self.assertRaisesRegex(ValueError, 'already in ledger'):
            g.validate_identity(IDENTITY, IDENTITY.encode())
        with self.assertRaisesRegex(ValueError, 'Malformed'):
            g.validate_identity('BAD', b'')

    def test_manifest_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, 'escapes'):
                g.validate_manifest(root, [{'relative_path': '../escape'}], IDENTITY)

    def test_manifest_duplicate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'artifact').write_bytes(b'abc')
            row = {'relative_path': 'artifact', 'size': 3, 'sha256': g.sha(b'abc')}
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                g.validate_manifest(root, [row, row], IDENTITY)

    def test_manifest_omission(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'artifact').write_bytes(b'abc')
            row = {'relative_path': 'artifact', 'size': 3, 'sha256': g.sha(b'abc')}
            with self.assertRaisesRegex(ValueError, 'Required'):
                g.validate_manifest(root, [row], IDENTITY)


def suite():
    _, controller = rendered_module()
    generated_tests = types.ModuleType('offline_generated_controller_tests')
    generated_tests.__file__ = __file__
    with patch.dict(sys.modules, {'microsd_physical_once': controller}):
        exec(compile(g.render_tests(g.BASE_TESTS.read_text()), '<offline-generated-tests>', 'exec'), generated_tests.__dict__)
    loader = unittest.TestLoader()
    result = unittest.TestSuite()
    result.addTests(loader.loadTestsFromTestCase(GeneratorTests))
    result.addTests(loader.loadTestsFromModule(generated_tests))
    return result


if __name__ == '__main__':
    with patch('subprocess.run', side_effect=AssertionError('External process forbidden in offline controller test')):
        result = unittest.TextTestRunner(verbosity=2).run(suite())
    raise SystemExit(0 if result.wasSuccessful() else 1)
