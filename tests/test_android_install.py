'''Exercise interrupted capture recovery without an Android device.'''

import contextlib
import csv
import io
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import lgl_android_install as installer


class CaptureLayoutTests(unittest.TestCase):
    def test_pull_flattens_device_wrapper_and_tables(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            conn = MagicMock()

            def pull(*args):
                self.assertEqual(args, ('pull', '/device/shader-dump/.', directory))
                (root / 'tables').mkdir()
                (root / 'tables' / 'usage_by_label.csv').write_text('frame\n0\n')
                (root / 'pipelines').mkdir()
                (root / 'shaders').mkdir()

            conn.adb.side_effect = pull
            self.assertTrue(installer.pull_artifacts(conn, directory, '/device'))
            self.assertTrue((root / 'usage_by_label.csv').is_file())
            self.assertFalse((root / 'tables').exists())
            self.assertFalse((root / 'shader-dump').exists())

    def test_flatten_collision_preserves_both_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tables').mkdir()
            (root / 'tables' / 'usage_by_label.csv').write_text('original')
            (root / 'usage_by_label.csv').write_text('existing')
            self.assertFalse(installer.pull_artifacts(MagicMock(), directory, '/device'))
            self.assertEqual((root / 'usage_by_label.csv').read_text(), 'existing')
            self.assertEqual((root / 'tables' / 'usage_by_label.csv').read_text(), 'original')

    def test_report_reads_flat_layout_and_links_to_shaders(self):
        spec = importlib.util.spec_from_file_location(
            'shader_dump_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        for separate_output in (False, True):
            with self.subTest(separate_output=separate_output), \
                    tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'pipelines').mkdir()
                (root / 'shaders').mkdir()
                (root / 'pipelines' / 'p.json').write_text(json.dumps({
                    'pipeline_signature': 'sha1:abc', 'kind': 'graphics',
                    'shaders': [{'sha1': 'def', 'stage': 'vertex', 'entry_point': 'main'}],
                    'executables': [], 'capture_status': 'statistics_disabled'}))
                (root / 'usage_by_label.csv').write_text(
                    'frame,pipeline_signature,label_path,draw_count\n0,abc,Pass,7\n')
                output = root / 'custom report' if separate_output else root
                argv = ['report', str(root)]
                if separate_output:
                    argv += ['--output', str(output)]
                with patch('sys.argv', argv):
                    report.main()
                html = (output / 'shader_usage_report.html').read_text(encoding='utf-8')
                # The embedded data and links must use the actual report location.
                data = json.loads(re.search(
                    r'<script[^>]*id="report-data"[^>]*>(.*?)</script>', html, re.S).group(1))
                self.assertEqual(data['pipelines'][0]['command_count'], 7)
                self.assertEqual(data['shaders'][0]['label_groups'], ['Pass'])
                self.assertNotIn('rank', data['pipelines'][0])
                self.assertNotIn('rank', data['shaders'][0])
                for filename in ('pipeline_summary.csv', 'shader_summary.csv'):
                    with (output / filename).open(newline='', encoding='utf-8') as handle:
                        self.assertNotIn('rank', csv.DictReader(handle).fieldnames)
                expected = Path(os.path.relpath(root / 'shaders', output)).as_posix()
                self.assertEqual(data['shader_base'], expected)
                self.assertIn('${D.shader_base}/', html)
                self.assertTrue((output / 'shader_summary.csv').is_file())
                self.assertFalse((root / 'summary').exists())


class InterruptedCaptureTests(unittest.TestCase):
    def run_capture(self, answers, timed=False, pull_ok=True, artifacts=True):
        args = SimpleNamespace(
            device='device', package='package', package_activity='activity',
            package_arguments=None, layer=['layer_gpu_support'], symbols=False,
            config=[], pull_artifact='output' if artifacts else None,
            timed_capture=10 if timed else 0, timeline_metadata=None,
            profile=None, logcat=None, timeline_perfetto=None, auto_start=True)
        layer = SimpleNamespace(name='VK_LAYER_LGL_gpu_support')
        host_path = 'output/original_timestamp_package'
        mocks = {
            'parse_cli': MagicMock(return_value=args),
            'ADBConnect': MagicMock(),
            'get_device_name': MagicMock(return_value='device'),
            'get_package_name': MagicMock(return_value='package'),
            'get_layer_metadata': MagicMock(return_value=[layer]),
            'get_layer_configs': MagicMock(return_value=True),
            'configure_artifact_output': MagicMock(return_value=(True, layer, host_path)),
            'install_layer_binary': MagicMock(return_value=True),
            'install_layer_config': MagicMock(return_value=True),
            'enable_layers': MagicMock(return_value=True),
            'configure_server': MagicMock(),
            'ShaderDumpProgressReporter': MagicMock(),
            'pull_artifacts': MagicMock(return_value=pull_ok),
            'generate_shader_dump_report': MagicMock(),
            'disable_layers': MagicMock(return_value=True),
            'uninstall_layer_binary': MagicMock(return_value=True),
            'uninstall_layer_config': MagicMock(return_value=True),
        }
        with contextlib.ExitStack() as stack:
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(patch.multiple(installer, **mocks))
            android = stack.enter_context(patch.object(installer, 'AndroidUtils'))
            android.get_os_sdk_version.return_value = 35
            android.is_package_32bit.return_value = False
            prompt = stack.enter_context(patch('builtins.input', side_effect=answers))
            stack.enter_context(patch.object(
                installer.time, 'sleep', side_effect=KeyboardInterrupt if timed else None))
            result = installer.main()
        mocks['disable_layers'].assert_called_once()
        mocks['uninstall_layer_binary'].assert_called_once()
        mocks['uninstall_layer_config'].assert_called_once()
        self.assertEqual(android.stop_package.call_count, 2)
        if artifacts:
            mocks['ShaderDumpProgressReporter'].return_value.stop.assert_called_once()
        return result, mocks, prompt, host_path

    def test_interrupt_yes_pulls_into_original_directory_and_reports(self):
        result, mocks, _, host_path = self.run_capture([KeyboardInterrupt, 'y'])
        self.assertEqual(result, 0)
        self.assertEqual(mocks['pull_artifacts'].call_args.args[1], host_path)
        mocks['generate_shader_dump_report'].assert_called_once()

    def test_interrupt_skip_preserves_cleanup(self):
        for answer in ('n', '', EOFError, KeyboardInterrupt):
            with self.subTest(answer=answer):
                result, mocks, _, _ = self.run_capture([KeyboardInterrupt, answer])
                self.assertEqual(result, 0)
                mocks['pull_artifacts'].assert_not_called()
                mocks['generate_shader_dump_report'].assert_not_called()

    def test_timed_capture_interrupt_prompts_and_accepts_yes(self):
        result, mocks, prompt, _ = self.run_capture([' YES '], timed=True)
        self.assertEqual(result, 0)
        prompt.assert_called_once()
        mocks['pull_artifacts'].assert_called_once()

    def test_invalid_answer_reprompts(self):
        _, mocks, prompt, _ = self.run_capture([KeyboardInterrupt, 'maybe', 'y'])
        self.assertEqual(prompt.call_count, 3)
        mocks['pull_artifacts'].assert_called_once()

    def test_pull_failure_still_cleans_up(self):
        result, mocks, _, _ = self.run_capture([KeyboardInterrupt, 'y'], pull_ok=False)
        self.assertEqual(result, 13)
        mocks['generate_shader_dump_report'].assert_not_called()

    def test_normal_enter_pulls_without_confirmation(self):
        result, mocks, prompt, _ = self.run_capture([''])
        self.assertEqual(result, 0)
        prompt.assert_called_once()
        mocks['pull_artifacts'].assert_called_once()

    def test_interrupt_without_artifacts_cleans_up_without_prompt(self):
        result, mocks, prompt, _ = self.run_capture([KeyboardInterrupt], artifacts=False)
        self.assertEqual(result, 0)
        prompt.assert_called_once()
        mocks['pull_artifacts'].assert_not_called()


if __name__ == '__main__':
    unittest.main()
