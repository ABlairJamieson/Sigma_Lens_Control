"""Provisioning tests use a temporary filesystem; no root, network or boot edits."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('cm5_camera_setup', Path(__file__).resolve().parents[1] / 'cm5_setup/camera_setup.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class CameraSetup(unittest.TestCase):
    def test_preserves_other_arguments_replaces_duplicates_and_is_idempotent(self):
        old = 'console=tty1 custom="keep this" swiotlb=1 root=UUID=test usbcore.usbfs_memory_mb=16 swiotlb=2\n'
        new = setup.kernel_arguments(old)
        self.assertIn('custom="keep this"', new)
        self.assertIn('root=UUID=test', new)
        self.assertEqual(new.count('swiotlb='), 1)
        self.assertIn('swiotlb=131072', new)
        self.assertIn('usbcore.usbfs_memory_mb=200', new)
        self.assertEqual(setup.kernel_arguments(new), new)

    def test_apply_backs_up_original_and_configures_live_limit_and_rules(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            config = root / 'etc/kernel/cmdline'
            config.parent.mkdir(parents=True)
            config.write_text('console=tty1 root=UUID=test\n')
            live = root / 'sys/module/usbcore/parameters/usbfs_memory_mb'
            live.parent.mkdir(parents=True)
            live.write_text('16\n')
            setup.configure(root)
            expected = config.read_text()
            backup = config.with_name(config.name + '.before-camera-setup')
            self.assertEqual(backup.read_text(), 'console=tty1 root=UUID=test\n')
            self.assertEqual(live.read_text(), '200\n')
            rules = (root / 'etc/udev/rules.d/99-sigma-asi.rules').read_text()
            self.assertIn('OWNER="photogrammetry", MODE="0660"', rules)
            self.assertIn('ATTR{power/control}="on"', rules)
            setup.configure(root)
            self.assertEqual(config.read_text(), expected)
            self.assertEqual(backup.read_text(), 'console=tty1 root=UUID=test\n')
            proc = root / 'proc/cmdline'
            proc.parent.mkdir()
            proc.write_text('root=UUID=test')
            self.assertFalse(all(row[0] for row in setup.boot_checks(root)))
            proc.write_text(expected)
            self.assertTrue(all(row[0] for row in setup.boot_checks(root)))

    def test_invalid_user_or_missing_boot_config_cannot_write_rules(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(ValueError):
                setup.configure(root, 'bad"name')
            with self.assertRaises(FileNotFoundError):
                setup.configure(root)
            self.assertFalse((root / 'etc/udev/rules.d').exists())
