"""Capture identity, provenance, fallback and offline regeneration checks."""

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

from lglpy.android import capture


class CaptureMetadataTests(unittest.TestCase):
    def test_session_reports_keep_devices_separate_and_inherit_capture_metadata(self):
        spec = importlib.util.spec_from_file_location('shader_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'capture_info.json').write_text(json.dumps({'schema_version': 1, 'application': {'version_code': '17'}}))
            for name, value in [('device-a', 5), ('device-b', 28)]:
                device = root / 'devices' / name
                (device / 'pipelines').mkdir(parents=True)
                (device / 'pipelines' / 'same-1.json').write_text(json.dumps({
                    'pipeline_signature': 'abc', 'pipeline_instance_id': 1,
                    'executables': [{'index': 0, 'statistics': [{'name': 'Registers used', 'value': value}]}]}))
            report.main([str(root)])
            index = (root / 'shader_usage_report.html').read_text(encoding='utf-8')
            self.assertIn('devices/device-a/shader_usage_report.html', index)
            self.assertIn('devices/device-b/shader_usage_report.html', index)
            import re
            for name, value in [('device-a', 5), ('device-b', 28)]:
                page = (root / 'devices' / name / 'shader_usage_report.html').read_text(encoding='utf-8')
                data = json.loads(re.search(r'type="application/json">(.*?)</script>', page, re.S)[1])
                self.assertEqual(len(data['pipeline_instances']), 1)
                self.assertEqual(data['statistics']['abc'][0]['statistics'][0]['value'], value)
                self.assertEqual(data['capture_info']['application']['version_code'], '17')

    def test_damaged_pipeline_and_partial_usage_do_not_discard_good_records(self):
        spec = importlib.util.spec_from_file_location('shader_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'pipelines').mkdir()
            (root / 'pipelines' / 'good.json').write_text('{"pipeline_signature":"abc"}')
            (root / 'pipelines' / 'damaged.json').write_text('{"pipeline_signature":')
            (root / 'frame_pipeline_usage.csv').write_text(
                'frame,pipeline_signature,label_path,draw_count\n1,abc,Pass,3\n2,abc,Pass,\n3,abc')
            warnings = []
            self.assertEqual(list(report.read_pipelines(root, warnings)), ['abc'])
            usage = report.read_usage(root, warnings)
            self.assertEqual(usage['abc']['draw_count'], 3)
            self.assertEqual(len(warnings), 2)

    def test_indirect_commands_are_a_subset_of_cpu_draw_calls(self):
        spec = importlib.util.spec_from_file_location('shader_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'frame_pipeline_usage.csv').write_text(
                'frame,pipeline_signature,label_path,draw_count,dispatch_count,trace_rays_count,indirect_draw_command_count\n'
                '1,abc,Pass,21,0,0,16\n')
            usage = report.read_usage(root)['abc']
            self.assertEqual(usage['draw_count'], 21)
            self.assertEqual(usage['indirect_draw_command_count'], 16)
            (root / 'frame_pipeline_usage.csv').write_text('frame,pipeline_signature,draw_count\n1,abc,3\n')
            self.assertEqual(report.read_usage(root)['abc']['indirect_draw_command_count'], 0)

    def test_pipeline_instances_keep_distinct_executable_statistics(self):
        spec = importlib.util.spec_from_file_location('shader_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'pipelines').mkdir()
            for instance, registers in [(1, 5), (2, 28)]:
                data = {'pipeline_signature': 'sha1:abc', 'pipeline_instance_id': instance,
                        'kind': 'compute', 'executables': [{'index': 0, 'statistics': [
                            {'name': 'Registers used', 'value': registers}]}]}
                (root / 'pipelines' / f'abc-{instance}.json').write_text(json.dumps(data))
            group = report.read_pipelines(root)['abc']
            self.assertEqual(len(group['instances']), 2)
            self.assertEqual([e['index'] for e in group['executables']], [0, 0])
            self.assertEqual(len({e['key'] for e in group['executables']}), 2)
            with patch('sys.argv', ['report', str(root)]):
                report.main()
            import re
            html = (root / 'shader_usage_report.html').read_text(encoding='utf-8')
            data = json.loads(re.search(r'type="application/json">(.*?)</script>', html, re.S)[1])
            self.assertEqual(data['pipelines'][0]['instance_count'], 2)
            self.assertEqual(len(data['pipeline_instances']), 2)
            self.assertEqual([e['statistics'][0]['value'] for e in data['statistics']['abc']], [5, 28])

    def collect(self, root, *, hashes=False, fail=False, package_dump=None):
        binary = root / 'layer.so'
        binary.write_bytes(b'installed layer')
        layer = SimpleNamespace(name='VK_LAYER_LGL_gpu_support', host_path=str(binary),
                                layer_dir='layer_gpu_support', config='{"shader_dump":{"frame_count":300}}')
        args = SimpleNamespace(timed_capture=8, auto_start=True, package_arguments=None,
                               capture_apk_hash=hashes, capture_scene='test scene')
        conn = MagicMock(package='com.test')
        outputs = {
            ('getprop',): '[ro.product.model]: [Test Phone]\n[ro.build.version.sdk]: [36]\n',
            ('dumpsys', 'package', 'com.test'): ' versionCode=17 minSdk=29\n versionName=1.2 beta\n primaryCpuAbi=arm64-v8a\n',
            ('pm', 'path', 'com.test'): 'package:/data/app/base.apk\npackage:/data/app/split.arm64.apk\n',
        }
        if package_dump is not None:
            outputs[('dumpsys', 'package', 'com.test')] = package_dump
        conn.adb_run.side_effect = lambda *args: outputs[args]

        def pull(*args):
            if fail:
                raise OSError('APK unavailable')
            Path(args[2]).write_bytes(args[1].encode())
        conn.adb.side_effect = pull

        def run(command, **kwargs):
            self.assertEqual(command[0], 'git')
            return SimpleNamespace(stdout='abc123\n')
        with patch.object(capture.sp, 'run', side_effect=run):
            info = capture.collect_capture_info(conn, [layer], args, '.Activity', False)
        return info, conn

    def test_pm_identity_and_hashes_include_splits(self):
        with tempfile.TemporaryDirectory() as directory:
            info, conn = self.collect(Path(directory), hashes=True)
            self.assertEqual(info['application']['version_name'], '1.2 beta')
            self.assertEqual(info['application']['version_code'], '17')
            self.assertEqual(info['application']['version_source'], 'Package Manager (dumpsys package)')
            self.assertEqual(info['application']['execution_abi'], 'arm64-v8a')
            self.assertEqual(conn.adb.call_count, 2)
            for item, path in zip(info['application']['apk_files'], ['/data/app/base.apk', '/data/app/split.arm64.apk']):
                self.assertEqual(item['sha256'], hashlib.sha256(path.encode()).hexdigest())
            self.assertEqual(info['device']['model'], 'Test Phone')
            self.assertIsNone(info['device']['soc_model'])
            self.assertEqual(info['tools']['layers'][0]['binary_sha256'], hashlib.sha256(b'installed layer').hexdigest())
            self.assertEqual(info['notes']['scene'], 'test scene')

    def test_default_uses_pm_without_pulling_any_apks(self):
        with tempfile.TemporaryDirectory() as directory:
            info, conn = self.collect(Path(directory))
            conn.adb.assert_not_called()
            conn.adb_run.assert_any_call('dumpsys', 'package', 'com.test')
            self.assertEqual(info['application']['version_name'], '1.2 beta')
            self.assertEqual(info['application']['version_code'], '17')
            self.assertEqual(info['application']['version_source'], 'Package Manager (dumpsys package)')
            self.assertTrue(all(item['sha256'] is None for item in info['application']['apk_files']))

    def test_hash_failure_keeps_pm_identity_and_can_save_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info, _ = self.collect(root, fail=True, hashes=True)
            self.assertEqual(info['application']['version_name'], '1.2 beta')
            self.assertTrue(any('APK unavailable' in warning for warning in info['warnings']))
            capture.save_capture_info(root, info)
            self.assertEqual(json.loads((root / 'capture_info.json').read_text(encoding='utf-8')), info)
            self.assertFalse((root / 'capture_info.json.tmp').exists())

    def test_offline_report_combines_snapshot_vulkan_and_actual_usage_frames(self):
        spec = importlib.util.spec_from_file_location('shader_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info, _ = self.collect(root)
            capture.save_capture_info(root, info)
            original = (root / 'capture_info.json').read_bytes()
            (root / 'metadata').mkdir()
            (root / 'metadata' / 'vulkan-1.json').write_text('{"schema_version":1,"driver_info":"r54p1"}')
            (root / 'usage_by_label.csv').write_text('frame,pipeline_signature,label_path,draw_count\n7,abc,pass,1\n7,abc,pass2,2\n9,abc,pass,3\n')
            combined = report.read_capture_info(root)
            self.assertEqual(combined['recorded_usage']['frame_count'], 2)
            self.assertEqual(combined['recorded_usage']['first_frame'], 7)
            self.assertEqual(combined['recorded_usage']['last_frame'], 9)
            self.assertEqual(combined['application'], info['application'])
            self.assertEqual(combined['vulkan_devices'][0]['driver_info'], 'r54p1')
            with patch('sys.argv', ['report', str(root)]):
                report.main()
            self.assertEqual((root / 'capture_info.json').read_bytes(), original)

    def test_pm_long_version_code_and_missing_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            code = str((2 << 32) | 7)
            info, _ = self.collect(Path(directory), package_dump=f' versionCode={code} minSdk=29\n versionName=release\n')
            self.assertEqual(info['application']['version_code'], code)
            info, conn = self.collect(Path(directory), package_dump='')
            self.assertIsNone(info['application']['version_code'])
            self.assertIsNone(info['application']['version_name'])
            self.assertIsNone(info['application']['version_source'])
            conn.adb.assert_not_called()

    def test_usage_filename_prefers_frame_pipeline_usage_and_reads_legacy(self):
        spec = importlib.util.spec_from_file_location('shader_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = 'frame,pipeline_signature,label_path,draw_count\n'
            (root / 'usage_by_label.csv').write_text(header + '7,abc,pass,99\n')
            self.assertEqual(report.read_usage(root)['abc']['draw_count'], 99)
            (root / 'frame_pipeline_usage.csv').write_text(header + '9,abc,pass,3\n')
            self.assertEqual(report.read_usage(root)['abc']['draw_count'], 3)
            info = report.read_capture_info(root)
            self.assertEqual(info['recorded_usage']['first_frame'], 9)
            self.assertIn('frame_pipeline_usage.csv', info['recorded_usage']['source'])


if __name__ == '__main__':
    unittest.main()
