"""Carrier conversion and bridge tests, with no physical GPIO/I2C writes."""
import importlib.util
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

from test_camera_features import bridge_module, bridge, Server

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('carrier_test', ROOT/'cm5_setup/carrier_control.py')
carrier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(carrier)


def state(enabled=False):
    return dict(available=True, camera_enabled=enabled, camera_fault_active=False)


class CarrierTests(unittest.TestCase):
    def test_signed_conversions_and_shunt_units(self):
        self.assertEqual(carrier.signed16(0xFFFF), -1)
        self.assertEqual(carrier.signed16(0xF380)/128, -25)
        sample = carrier.ina_values(4000, 9600)
        self.assertAlmostEqual(sample['current_A'], 1)
        self.assertAlmostEqual(sample['bus_V'], 12)
        self.assertAlmostEqual(sample['load_V'], 11.99)
        self.assertLess(carrier.ina_values(0xFFFF, 0)['current_A'], 0)

    def test_missing_sensor_does_not_hide_other_readings(self):
        with patch.object(carrier, 'monitor', side_effect=OSError('NACK')), \
             patch.object(carrier, 'temperature', return_value={'temperature_C': 22}):
            result = carrier.readings()
        self.assertIn('error', result['camera_12V'])
        self.assertEqual(result['temperature']['temperature_C'], 22)

    def test_local_socket_partial_sensor_error_is_not_service_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            address = str(Path(directory)/'carrier.sock')
            server = socket.socket(socket.AF_UNIX)
            server.bind(address)
            server.listen()
            def answer():
                with server.accept()[0] as client:
                    self.assertEqual(client.recv(64), b'read\n')
                    client.sendall(json.dumps(dict(state(),temperature={'error':'NACK'})).encode()+b'\n')
            thread = threading.Thread(target=answer)
            thread.start()
            try:
                with patch.object(bridge_module, 'CARRIER_SOCKET', address):
                    result = bridge_module.carrier_request()
                self.assertTrue(result['available'])
                self.assertEqual(result['temperature']['error'], 'NACK')
            finally:
                thread.join(3)
                server.close()

    def test_platform_detection(self):
        for compatible, expected in [(b'radxa,cm5\0rockchip,rk3588', 'radxa_cm5'),
                                     (b'beagle,beagley-ai\0ti,j722s', 'beagley_ai')]:
            with patch.object(bridge_module.Path, 'read_bytes', return_value=compatible):
                self.assertEqual(bridge_module.host_platform(), expected)

    def test_beagley_never_probes_or_switches_cm5_service(self):
        b = bridge()
        with patch.object(bridge_module, 'host_platform', return_value='beagley_ai'), \
             patch.object(bridge_module.socket, 'socket') as connection:
            response = b.status()
            self.assertFalse(response['capabilities']['carrier_power'])
            self.assertEqual(response['capabilities']['platform'], 'beagley_ai')
            self.assertFalse(b.camera_power({'enabled':True})['ok'])
            connection.assert_not_called()

    def test_status_with_camera_off_and_no_sdk_handle(self):
        b = bridge_module.ASIBridge()
        with patch.object(bridge_module, 'carrier_request', return_value=state()):
            response = b.status()
        self.assertTrue(response['ok'])
        self.assertFalse(response['connected'])
        self.assertTrue(response['carrier']['available'])

    def test_power_off_closes_sdk_and_power_on_allows_retry(self):
        b = bridge()
        camera = b.cam
        camera.close = Mock()
        with patch.object(bridge_module, 'carrier_request', side_effect=[state(True), state(False)]) as request:
            response = b.camera_power({'enabled':False})
        self.assertTrue(response['ok'])
        self.assertIsNone(b.cam)
        camera.close.assert_called_once()
        self.assertEqual([x.args[0] for x in request.call_args_list], ['status','off'])
        with patch.object(bridge_module, 'carrier_request', side_effect=[state(False),state(True)]):
            self.assertTrue(b.camera_power({'enabled':True})['ok'])
        with patch.object(bridge_module, 'carrier_request', return_value=state(True)), \
             patch.object(b, 'init_camera') as initialize:
            b.info = {'Name':'test'}
            b.retry_camera_once()
            initialize.assert_called_once()

    def test_invalid_value_live_session_and_unavailable_service_cannot_switch(self):
        b = bridge()
        with patch.object(bridge_module, 'carrier_request') as request:
            self.assertFalse(b.camera_power({'enabled':'false'})['ok'])
            b.live_owner = object()
            self.assertFalse(b.camera_power({'enabled':False})['ok'])
            request.assert_not_called()
        b.live_owner = None
        with patch.object(bridge_module, 'carrier_request', return_value={'available':False,'error':'missing'}) as request:
            self.assertFalse(b.camera_power({'enabled':False})['ok'])
            self.assertEqual(request.call_count,1)
        self.assertIsNotNone(b.cam)

    def test_retry_skips_intentionally_disabled_camera(self):
        b = bridge_module.ASIBridge()
        with patch.object(bridge_module, 'carrier_request', return_value=state()), \
             patch.object(b, 'init_camera') as initialize:
            self.assertFalse(b.retry_camera_once())
            initialize.assert_not_called()

    def test_busy_capture_rejects_power_but_allows_carrier_read(self):
        b = bridge()
        server = Server(lambda conn, addr: bridge_module.client_thread(b,conn,addr))
        b.lock.acquire()
        try:
            with patch.object(bridge_module, 'carrier_request', return_value=state()) as request, \
                 socket.create_connection(('127.0.0.1',server.port),timeout=2) as client:
                stream = client.makefile('rb')
                client.sendall(b'{"cmd":"camera_power","enabled":false}\n')
                response=json.loads(stream.readline())
                self.assertFalse(response['ok'])
                request.assert_not_called()
                client.sendall(b'{"cmd":"carrier_read"}\n')
                self.assertTrue(json.loads(stream.readline())['carrier']['available'])
                request.assert_called_once_with()
                stream.close()
        finally:
            b.lock.release()
            server.close()


if __name__ == '__main__':
    unittest.main()
