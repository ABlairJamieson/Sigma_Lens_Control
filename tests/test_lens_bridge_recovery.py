"""Recovery only signals the expected same-user systemd process, at most once/30s."""
import signal
import types
import unittest
from unittest.mock import MagicMock, patch
from test_camera_features import bridge_module as module


class LensRecovery(unittest.TestCase):
    def setUp(self):
        module._lens_last_restart = float('-inf')

    def call(self, *, pid='123', restart='always', uid=None, argv=b'python\0/repo/teensy_bridge/teensy_bridge.py\0'):
        proc = MagicMock()
        proc.stat.return_value.st_uid = module.os.geteuid() if uid is None else uid
        proc.__truediv__.return_value.read_bytes.return_value = argv
        root = MagicMock()
        root.__truediv__.return_value = proc
        result = types.SimpleNamespace(stdout=f'MainPID={pid}\nRestart={restart}\n')
        with patch.object(module.subprocess, 'run', return_value=result), patch.object(module, 'Path', return_value=root), patch.object(module.os, 'kill') as kill:
            response = module.restart_lens_bridge()
            return response, kill.call_args_list

    def test_restart_once_and_rate_limit(self):
        response, calls = self.call()
        self.assertTrue(response['ok'])
        self.assertEqual(calls[0].args, (123, signal.SIGTERM))
        response, calls = self.call()
        self.assertFalse(response['ok'])
        self.assertEqual(calls, [])

    def test_refuse_unrelated_or_privileged_process(self):
        for overrides in ({'argv': b'python\0other.py\0'}, {'uid': module.os.geteuid()+1}, {'pid': '0'}, {'restart': 'on-failure'}):
            with self.subTest(overrides=overrides):
                response, calls = self.call(**overrides)
                self.assertFalse(response['ok'])
                self.assertEqual(calls, [])

    def test_request_does_not_open_camera(self):
        with patch.object(module, 'restart_lens_bridge', return_value={'ok': True}) as restart:
            result = module.ASIBridge().handle_request({'cmd': 'lens_bridge_restart'}, None)
            self.assertTrue(result['ok'])
            restart.assert_called_once()

class SerialDisconnect(unittest.IsolatedAsyncioTestCase):
    async def test_lost_device_exits_reader_for_systemd_restart(self):
        import importlib.util
        from pathlib import Path
        spec = importlib.util.spec_from_file_location('teensy_recovery_test', Path(__file__).resolve().parents[1]/'teensy_bridge/teensy_bridge.py')
        bridge = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bridge)
        ser = MagicMock()
        ser.read.side_effect = OSError('device unplugged')
        with self.assertRaisesRegex(RuntimeError, 'Teensy serial device lost'):
            await bridge.Bridge(ser).serial_reader()
        ser.read.assert_called_once()


if __name__ == '__main__':
    unittest.main()
