"""No physical uploads: source selection, identity guards and service restoration."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('updater_test',ROOT/'tools/update_teensy.py')
u=importlib.util.module_from_spec(spec);spec.loader.exec_module(u)
RELEASE=json.loads((ROOT/'tools/firmware-release.json').read_text())
DEVICE=[{'serial':'19822090','pid':'0483'}]


class UpdaterTests(unittest.TestCase):
    def test_release_validation_rejects_mismatch_and_wrong_target(self):
        u.validate_release(RELEASE)
        for field,value in [('commit','x'),('ref','firmware/v99'),('fqbn','teensy:avr:teensy41')]:
            with self.assertRaises(ValueError):u.validate_release(dict(RELEASE,**{field:value}))

    def test_stages_released_source_without_changing_live_config(self):
        before=(ROOT/'backend/include/Config.h').read_bytes()
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)/'stage'
            sketch,record=u.prepare(RELEASE,7,out)
            self.assertIn('cameraID = 7;', (sketch/'include/Config.h').read_text())
            self.assertIn('"2.0.0"',(sketch/'include/FirmwareVersion.h').read_text())
            self.assertFalse((sketch/'.#cam_messages.txt').exists())
            self.assertFalse(record['upload_verified'])
            u.prepare(RELEASE,7,out)  # repeatable staging
            with self.assertRaises(RuntimeError):u.prepare(RELEASE,3,out)
        self.assertEqual(before,(ROOT/'backend/include/Config.h').read_bytes())

    def test_rejects_unowned_output_and_path_traversal(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)/'stage';out.mkdir();(out/'notes').write_text('keep')
            with self.assertRaises(RuntimeError):u.prepare(RELEASE,7,out)
            self.assertEqual((out/'notes').read_text(),'keep')
            archive=io.BytesIO()
            with tarfile.open(fileobj=archive,mode='w') as tar:
                info=tarfile.TarInfo('../escape');info.size=1;tar.addfile(info,io.BytesIO(b'x'))
            with patch.object(u,'git',return_value=archive.getvalue()):
                with self.assertRaises(ValueError):u.prepare(RELEASE,7,Path(folder)/'bad')
            self.assertFalse((Path(folder)/'escape').exists())

    def test_online_selection_uses_remote_manifest_and_verifies_release_head(self):
        def git(*args):
            if args==('rev-parse','FETCH_HEAD'):return b'a'*40+b'\n' if not fetched else RELEASE['commit'].encode()+b'\n'
            if args[0]=='show':return json.dumps(RELEASE).encode()
            return b''
        fetched=False
        def run(*args,**kwargs):
            nonlocal fetched
            if args[-1]==RELEASE['ref']:fetched=True
        with patch.object(u,'git',side_effect=git),patch.object(u,'run',side_effect=run):
            self.assertEqual(u.released_source()['version'],'2.0.0')
        with patch.object(u,'git',side_effect=lambda *a: json.dumps(RELEASE).encode() if a[0]=='show' else b'a'*40),patch.object(u,'run'):
            with self.assertRaises(RuntimeError):u.released_source()

    def test_missing_wrong_and_multiple_teensies_cannot_flash(self):
        for devices in [[],DEVICE*2,[{'serial':'other','pid':'0483'}],[{'serial':'19822090','pid':'0478'}]]:
            with patch.object(u,'usb_teensies',return_value=devices),patch.object(u,'run') as run:
                with self.assertRaises(RuntimeError):u.flash(Path('image.hex'),RELEASE,'19822090',7,'loader')
                run.assert_not_called()

    def test_loader_failure_restores_previously_running_bridge(self):
        commands=[]
        def run(*args,**kwargs):
            commands.append(args)
            if '--mcu=TEENSY40' in args:raise subprocess.CalledProcessError(1,args)
        with patch.object(u,'usb_teensies',return_value=DEVICE),patch.object(u,'run',side_effect=run),patch.object(u.subprocess,'run',return_value=SimpleNamespace(returncode=0)):
            with self.assertRaises(subprocess.CalledProcessError):u.flash(Path('image.hex'),RELEASE,'19822090',7,'loader')
        self.assertIn(('sudo','systemctl','stop','teensy-bridge.service'),commands)
        self.assertEqual(commands[-1],('sudo','systemctl','start','teensy-bridge.service'))

    def test_inactive_bridge_stays_inactive_and_identity_is_verified(self):
        with patch.object(u,'usb_teensies',return_value=DEVICE),patch.object(u,'run') as run,patch.object(u.subprocess,'run',return_value=SimpleNamespace(returncode=3)),patch.object(u,'verify_serial',return_value='ID:7,FW:2.0.0') as verify:
            self.assertEqual(u.flash(Path('image.hex'),RELEASE,'19822090',7,'loader'),'ID:7,FW:2.0.0')
            verify.assert_called_once_with('19822090',7,'2.0.0')
            self.assertFalse(any('systemctl' in c.args for c in run.call_args_list))

    def test_wrong_core_cannot_compile_a_release(self):
        cores=json.dumps({'platforms':[{'id':'teensy:avr','installed_version':'1.59.0'}]}).encode()
        with patch.object(u.subprocess,'check_output',return_value=cores),patch.object(u,'run') as run:
            with self.assertRaises(RuntimeError):u.build(RELEASE,Path('/tmp/unused'),'arduino-cli')
            run.assert_not_called()

    def test_source_cli_requires_explicit_target_identity_for_upload(self):
        result=subprocess.run(['python3',str(ROOT/'tools/update_teensy.py'),'--camera-id','7','--flash'],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('--flash requires --serial',result.stderr)


if __name__=='__main__':unittest.main()
