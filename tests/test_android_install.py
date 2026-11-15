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
from lglpy.android import artifacts as artifact_api
from layer_gpu_support.scripts import android_artifacts as gpu_artifacts


class CaptureLayoutTests(unittest.TestCase):
    def test_progress_service_registration_is_owned_by_handler(self):
        conn, progress, server = MagicMock(), MagicMock(), MagicMock()
        conn.adb.side_effect = installer.sp.CalledProcessError(1, 'adb reverse')
        handler = gpu_artifacts.GPUArtifactHandler()
        with patch.object(installer.server, 'CommsServer', return_value=server), \
                patch.object(installer.threading, 'Thread'):
            installer.configure_server(conn, None, None, [(handler, progress)])
        endpoint = server.register_endpoint.call_args.args[0]
        self.assertEqual(endpoint.get_service_name(), 'GPUShaderDump')
        endpoint.handle_message(SimpleNamespace(payload=b'{"type":"progress","frames":7}'))
        progress.handle_message.assert_called_once_with({'type': 'progress', 'frames': 7})
        progress.mark_unavailable.assert_called_once_with('ADB reverse failed')

    def test_generic_registry_selects_only_enabled_handlers_and_isolates_outputs(self):
        active, other, disabled = MagicMock(), MagicMock(), MagicMock()
        active.enabled.return_value = other.enabled.return_value = True
        disabled.enabled.return_value = False
        active.configure.side_effect = lambda layer, root, session: f'{root}/first/{session}'
        other.configure.side_effect = lambda layer, root, session: f'{root}/second/{session}'
        layers = [SimpleNamespace(name=name) for name in ('VK_FIRST', 'VK_SECOND', 'VK_OFF', 'VK_UNKNOWN')]
        registry = {'VK_FIRST': lambda: active, 'VK_SECOND': lambda: other, 'VK_OFF': lambda: disabled}
        with tempfile.TemporaryDirectory() as directory, patch.dict(artifact_api.HANDLER_FACTORIES, registry, clear=True):
            ok, plans, root = artifact_api.configure_artifact_output(layers, directory, '/device/artifacts', 'com.test')
            self.assertTrue(ok)
            self.assertEqual([plan.layer.name for plan in plans], ['VK_FIRST', 'VK_SECOND'])
            self.assertEqual({Path(plan.host_path).parent for plan in plans}, {Path(root)})
            self.assertNotEqual(plans[0].host_path, plans[1].host_path)
            self.assertNotEqual(plans[0].device_path, plans[1].device_path)
            self.assertEqual(active.configure.call_args.args[2], other.configure.call_args.args[2])
            disabled.configure.assert_not_called()

    def test_generic_metadata_is_saved_for_root_and_each_handler(self):
        save = MagicMock()
        plans = [SimpleNamespace(host_path='capture/first'), SimpleNamespace(host_path='capture/second')]
        info = {'capture': {}}
        artifact_api.save_artifact_info('capture', plans, info, save)
        self.assertEqual([call.args[0] for call in save.call_args_list],
                         ['capture', 'capture/first', 'capture/second'])

    def test_session_paths_are_unique_and_configuration_does_not_clear_output(self):
        with tempfile.TemporaryDirectory() as directory:
            layer = SimpleNamespace(name='VK_LAYER_LGL_gpu_support', config=json.dumps({
                'shader_dump': {'enable': True, 'clear_output': True}}))
            paths, device_paths = [], []
            for _ in range(2):
                ok, _, capture = artifact_api.configure_artifact_output(
                    [layer], directory, '/sdcard/Android/data/com.test/files/lgl-artifacts', 'com.test')
                self.assertTrue(ok)
                paths.append(capture)
                config = json.loads(layer.config)['shader_dump']
                self.assertNotIn('clear_output', config)
                device_paths.append(config['output_path'])
            self.assertNotEqual(paths[0], paths[1])
            self.assertNotEqual(device_paths[0], device_paths[1])
            self.assertTrue(all('/shader-dump/session-' in path for path in device_paths))

    def test_pull_preserves_device_isolation_and_flattens_each_device_table(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            conn = MagicMock()
            def pull(*args):
                self.assertEqual(args[1], '/device/shader-dump/session-new/.')
                for name in ('a', 'b'):
                    device = root / 'devices' / name
                    (device / 'tables').mkdir(parents=True)
                    (device / 'tables' / 'frame_pipeline_usage.csv').write_text('frame\n1\n')
            conn.adb.side_effect = pull
            self.assertTrue(gpu_artifacts.pull_artifacts(conn, directory, '/device/shader-dump/session-new'))
            for name in ('a', 'b'):
                self.assertTrue((root / 'devices' / name / 'frame_pipeline_usage.csv').is_file())
            self.assertFalse((root / 'frame_pipeline_usage.csv').exists())

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
            self.assertTrue(gpu_artifacts.pull_artifacts(conn, directory, '/device/shader-dump'))
            self.assertTrue((root / 'usage_by_label.csv').is_file())
            self.assertFalse((root / 'tables').exists())
            self.assertFalse((root / 'shader-dump').exists())

    def test_flatten_collision_preserves_both_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tables').mkdir()
            (root / 'tables' / 'usage_by_label.csv').write_text('original')
            (root / 'usage_by_label.csv').write_text('existing')
            self.assertFalse(gpu_artifacts.pull_artifacts(MagicMock(), directory, '/device'))
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
                self.assertEqual(data['capture_info']['vulkan_devices'], [])
                for filename in ('pipeline_summary.csv', 'shader_summary.csv'):
                    with (output / filename).open(newline='', encoding='utf-8') as handle:
                        self.assertNotIn('rank', csv.DictReader(handle).fieldnames)
                expected = Path(os.path.relpath(root / 'shaders', output)).as_posix()
                self.assertEqual(data['shader_base'], expected)
                self.assertIn('${D.shader_base}/', html)
                self.assertTrue((output / 'shader_summary.csv').is_file())
                self.assertFalse((root / 'summary').exists())

    def test_vulkan_metadata_preserves_raw_strings_and_skips_corrupt_files(self):
        spec = importlib.util.spec_from_file_location(
            'shader_dump_report', 'layer_gpu_support/scripts/shader_dump_report.py')
        report = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(report)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            metadata = root / 'metadata'
            metadata.mkdir()
            device = {'schema_version': 1, 'device_name': 'Mali test',
                      'driver_info': 'r54p1-<build>\n</script>',
                      'driver_version_raw': 4294967295, 'driver_name': None}
            for name in ('vulkan-1.json', 'vulkan-2.json'):
                (metadata / name).write_text(json.dumps(device), encoding='utf-8')
            (metadata / 'vulkan-3.json').write_text('{partial', encoding='utf-8')
            info = report.read_capture_info(root)
            self.assertEqual(info['vulkan_devices'], [device])
            self.assertEqual(len(info['warnings']), 1)
            with patch('sys.argv', ['report', str(root)]):
                report.main()
            html = (root / 'shader_usage_report.html').read_text(encoding='utf-8')
            data = json.loads(re.search(
                r'<script[^>]*id="report-data"[^>]*>(.*?)</script>', html, re.S).group(1))
            self.assertEqual(data['capture_info'], info)
            self.assertNotIn('</script>', re.search(
                r'<script[^>]*id="report-data"[^>]*>(.*?)</script>', html, re.S).group(1))


class ArtifactResetTests(unittest.TestCase):
    package = 'com.test'
    parent = '/sdcard/Android/data/com.test/files/lgl-artifacts'
    canonical = '/storage/emulated/0/Android/data/com.test/files/lgl-artifacts'

    def connection(self):
        conn = MagicMock(device='selected-device', package=self.package)
        conn.adb_run.side_effect = [self.canonical, self.canonical + '/shader-dump', '']
        return conn

    def test_default_retains_all_sessions_without_adb_cleanup(self):
        conn = self.connection()
        self.assertTrue(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent))
        conn.adb_run.assert_not_called()

    def test_reset_on_first_capture_handles_missing_root_without_deletion(self):
        conn = self.connection()
        conn.adb_run.side_effect = [self.canonical, installer.sp.CalledProcessError(1, 'readlink'), '', '']
        self.assertTrue(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent, True))
        self.assertFalse(any('rm' in call.args for call in conn.adb_run.call_args_list))

    def test_cli_reset_is_explicit_and_requires_artifact_capture(self):
        argv = ['install', '--layer', 'layer_gpu_support', '--reset-artifacts']
        with patch('sys.argv', argv):
            self.assertIsNone(installer.parse_cli())
        with patch('sys.argv', argv + ['--pull-artifact', 'output']):
            self.assertTrue(installer.parse_cli().reset_artifacts)

    def test_specific_reset_flag_is_no_longer_an_installer_option(self):
        with patch('sys.argv', ['install', '--layer', 'layer_gpu_support', '--reset-shader-dump']), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as result:
            installer.parse_cli()
        self.assertEqual(result.exception.code, 2)

    def test_explicit_reset_validates_then_deletes_exact_root_without_prompt(self):
        conn = self.connection()
        with patch('builtins.input') as prompt:
            self.assertTrue(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent, True))
        prompt.assert_not_called()
        self.assertEqual(conn.adb_run.call_args.args,
                         ('rm', '-rf', '--', self.canonical + '/shader-dump'))
        self.assertEqual(conn.adb_run.call_count, 3)
        self.assertFalse(any('run-as' in call.args or 'mkdir' in call.args
                             for call in conn.adb_run.call_args_list))

    def test_external_storage_reset_works_when_run_as_is_denied(self):
        conn = self.connection()
        def shell(*command):
            if command[0] in ('run-as', 'mkdir'):
                raise installer.sp.CalledProcessError(1, command, stderr='Permission denied')
            if command == ('readlink', '-f', self.parent):
                return self.canonical
            if command == ('readlink', '-f', self.parent + '/shader-dump'):
                return self.canonical + '/shader-dump'
            self.assertEqual(command, ('rm', '-rf', '--', self.canonical + '/shader-dump'))
            return ''
        conn.adb_run.side_effect = shell
        self.assertTrue(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent, True))

    def test_missing_parent_never_creates_directories_or_deletes_files(self):
        conn = self.connection()
        conn.adb_run.side_effect = [installer.sp.CalledProcessError(1, 'readlink'), '', '']
        self.assertTrue(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent, True))
        self.assertFalse(any('mkdir' in call.args or 'rm' in call.args for call in conn.adb_run.call_args_list))

    def test_device_error_details_are_reported_without_deleting(self):
        conn = self.connection()
        conn.adb_run.side_effect = installer.sp.CalledProcessError(1, 'readlink', stderr='Permission denied')
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertFalse(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent, True))
        self.assertIn('Device response: Permission denied', output.getvalue())
        self.assertFalse(any('rm' in call.args for call in conn.adb_run.call_args_list))

    def test_redirected_parent_or_dump_never_reaches_deletion(self):
        for responses in [ ['/storage/emulated/0/Android/data/other/files/lgl-artifacts'],
                           [self.canonical, self.canonical + '/..'] ]:
            conn = self.connection()
            conn.adb_run.side_effect = responses
            self.assertFalse(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent, True))
            self.assertFalse(any('rm' in call.args for call in conn.adb_run.call_args_list))

    def test_mismatched_target_or_missing_device_is_rejected_before_adb(self):
        for path in ('/', self.parent + '/../other', self.parent + '/shader-dump'):
            conn = self.connection()
            self.assertFalse(gpu_artifacts.prepare_artifact_output(conn, self.package, path, True))
            conn.adb_run.assert_not_called()
        conn = self.connection()
        conn.device = None
        self.assertFalse(gpu_artifacts.prepare_artifact_output(conn, self.package, self.parent, True))
        conn.adb_run.assert_not_called()


class ShaderDumpProgressTests(unittest.TestCase):
    def test_late_connection_recovers_periodic_updates_after_startup_timeout(self):
        reporter = gpu_artifacts.ShaderDumpProgressReporter(0)
        reporter.start_time = 0
        clock = [0.0]
        ticks = iter([5.1, 8.0, 13.1, 18.2])
        def wait(_):
            try:
                clock[0] = next(ticks)
            except StopIteration:
                return True
            if clock[0] == 8.0:
                reporter.handle_message({'type': 'capture_started'})
            elif clock[0] == 13.1:
                reporter.handle_message({'type': 'progress', 'shaders': 42, 'pipelines': 17, 'frames': 9})
            return False
        reporter.stop_event = MagicMock()
        reporter.stop_event.wait.side_effect = wait
        output = io.StringIO()
        with patch.object(gpu_artifacts.time, 'monotonic', side_effect=lambda: clock[0]), \
                contextlib.redirect_stdout(output):
            reporter.run()
        text = output.getvalue()
        self.assertEqual(text.count('progress unavailable'), 1)
        self.assertEqual(text.count('progress connected'), 1)
        self.assertIn('13s elapsed', text)
        self.assertIn('18s elapsed', text)
        self.assertEqual(text.count('SPIR-V   42'), 2)

    def test_missing_endpoint_warns_once_but_keeps_reporter_alive(self):
        reporter = gpu_artifacts.ShaderDumpProgressReporter(0)
        reporter.start_time = 0
        reporter.stop_event = MagicMock()
        reporter.stop_event.wait.side_effect = [False, False, False, True]
        output = io.StringIO()
        with patch.object(gpu_artifacts.time, 'monotonic', return_value=6.0), contextlib.redirect_stdout(output):
            reporter.run()
        self.assertEqual(reporter.stop_event.wait.call_count, 4)
        self.assertEqual(output.getvalue().count('progress unavailable'), 1)

    def test_connection_racing_timeout_does_not_print_unavailable(self):
        reporter = gpu_artifacts.ShaderDumpProgressReporter(20)
        reporter.start_time = 0
        with patch.object(gpu_artifacts.time, 'monotonic', return_value=11.0), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            reporter.handle_message({'type': 'progress', 'shaders': 5})
            reporter.print_unavailable()
        self.assertIn('9s remaining', output.getvalue())
        self.assertNotIn('progress unavailable', output.getvalue())


class InterruptedCaptureTests(unittest.TestCase):
    def run_capture(self, answers, timed=False, pull_ok=True, artifacts=True, handler_count=1, reset=False):
        args = SimpleNamespace(
            device='device', package='package', package_activity='activity',
            package_arguments=None, layer=['layer_gpu_support'], symbols=False,
            config=[], pull_artifact='output' if artifacts else None,
            timed_capture=10 if timed else 0, timeline_metadata=None,
            profile=None, logcat=None, timeline_perfetto=None, auto_start=True, reset_artifacts=reset)
        layer = SimpleNamespace(name='VK_LAYER_LGL_gpu_support', config=json.dumps({'shader_dump': {'output_path': '/device/session'}}))
        host_path = 'output/original_timestamp_package'
        handler = MagicMock()
        handler.prepare.return_value = True
        handler.pull.return_value = pull_ok
        plan = artifact_api.ArtifactCapture(layer, handler, host_path, '/device/session')
        plans = [plan]
        for index in range(1, handler_count):
            extra_layer = SimpleNamespace(name=f'VK_LAYER_TEST_{index}')
            extra = MagicMock()
            extra.prepare.return_value = extra.pull.return_value = True
            extra.create_progress.return_value = None
            plans.append(artifact_api.ArtifactCapture(extra_layer, extra, host_path + f'/handler-{index}', f'/device/other/{index}'))
        mocks = {
            'parse_cli': MagicMock(return_value=args),
            'ADBConnect': MagicMock(),
            'get_device_name': MagicMock(return_value='device'),
            'get_package_name': MagicMock(return_value='package'),
            'get_layer_metadata': MagicMock(return_value=[layer]),
            'get_layer_configs': MagicMock(return_value=True),
            'configure_artifact_output': MagicMock(return_value=(True, plans, host_path)),
            'install_layer_binary': MagicMock(return_value=True),
            'install_layer_config': MagicMock(return_value=True),
            'enable_layers': MagicMock(return_value=True),
            'configure_server': MagicMock(),
            'collect_capture_info': MagicMock(return_value={'capture': {}}),
            'save_capture_info': MagicMock(),
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
        mocks['pull_artifacts'] = handler.pull
        mocks['generate_shader_dump_report'] = handler.report
        mocks['ShaderDumpProgressReporter'] = handler.create_progress
        mocks['artifact_plans'] = plans
        mocks['disable_layers'].assert_called_once()
        mocks['uninstall_layer_binary'].assert_called_once()
        mocks['uninstall_layer_config'].assert_called_once()
        self.assertEqual(android.stop_package.call_count, 2 + int(reset))
        if artifacts:
            mocks['ShaderDumpProgressReporter'].return_value.stop.assert_called_once()
        return result, mocks, prompt, host_path

    def test_generic_reset_is_dispatched_to_all_selected_handlers_without_extra_prompt(self):
        result, mocks, prompt, _ = self.run_capture([''], handler_count=2, reset=True)
        self.assertEqual(result, 0)
        prompt.assert_called_once()  # normal capture-finished input only
        for plan in mocks['artifact_plans']:
            self.assertTrue(plan.handler.prepare.call_args.args[-1])
            plan.handler.pull.assert_called_once()
            plan.handler.report.assert_called_once()

    def test_one_handler_pull_failure_does_not_skip_other_handlers_or_cleanup(self):
        result, mocks, _, _ = self.run_capture([''], handler_count=2, pull_ok=False)
        self.assertEqual(result, 13)
        first, second = mocks['artifact_plans']
        first.handler.report.assert_not_called()
        second.handler.pull.assert_called_once()
        second.handler.report.assert_called_once()

    def test_interrupt_yes_pulls_into_original_directory_and_reports(self):
        result, mocks, _, host_path = self.run_capture([KeyboardInterrupt, 'y'])
        self.assertEqual(result, 0)
        self.assertEqual(mocks['pull_artifacts'].call_args.args[1], host_path)
        mocks['generate_shader_dump_report'].assert_called_once()
        snapshot = mocks['save_capture_info'].call_args.args[1]
        self.assertEqual(snapshot['capture']['status'], 'interrupted')
        self.assertEqual(snapshot['capture']['artifact_status'], 'pulled')

    def test_interrupt_skip_preserves_cleanup(self):
        for answer in ('n', '', EOFError, KeyboardInterrupt):
            with self.subTest(answer=answer):
                result, mocks, _, _ = self.run_capture([KeyboardInterrupt, answer])
                self.assertEqual(result, 0)
                mocks['pull_artifacts'].assert_not_called()
                mocks['generate_shader_dump_report'].assert_not_called()
                snapshot = mocks['save_capture_info'].call_args.args[1]
                self.assertEqual(snapshot['capture']['artifact_status'], 'pull_skipped')

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
        snapshot = mocks['save_capture_info'].call_args.args[1]
        self.assertEqual(snapshot['capture']['artifact_status'], 'pull_failed')

    def test_normal_enter_pulls_without_confirmation(self):
        result, mocks, prompt, _ = self.run_capture([''])
        self.assertEqual(result, 0)
        prompt.assert_called_once()
        mocks['pull_artifacts'].assert_called_once()
        snapshot = mocks['save_capture_info'].call_args.args[1]
        self.assertEqual(snapshot['capture']['status'], 'completed')

    def test_interrupt_without_artifacts_cleans_up_without_prompt(self):
        result, mocks, prompt, _ = self.run_capture([KeyboardInterrupt], artifacts=False)
        self.assertEqual(result, 0)
        prompt.assert_called_once()
        mocks['pull_artifacts'].assert_not_called()


if __name__ == '__main__':
    unittest.main()
