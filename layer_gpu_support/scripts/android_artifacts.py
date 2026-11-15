# SPDX-License-Identifier: MIT
"""GPU Support host-side artifacts; the installer only coordinates handlers."""
import csv
import json
import os
import re
import subprocess as sp
import sys
import threading
import time
from typing import Optional
from lglpy.android.adb import ADBConnect

def prepare_artifact_output(conn: ADBConnect, package: str,
                            device_artifact_dir: str, reset: bool = False) -> bool:
    '''Reset only the validated target-package dump root, and only on explicit request.'''
    try:
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+', package):
            raise ValueError('Invalid Android package name')
        expected = f'/sdcard/Android/data/{package}/files/lgl-artifacts'
        if device_artifact_dir != expected or conn.package != package or not conn.device:
            raise ValueError('Artifact path, selected package or device does not match the capture target')
        if not reset:
            return True
        shader_dump_dir = f'{device_artifact_dir}/shader-dump'
        # External app storage is accessible to adb shell on supported devices,
        # but run-as can be blocked by the storage/SELinux view despite the UID.
        # Never mkdir here: new output directories must be created by the app.
        def canonical(path):
            try:
                return conn.adb_run('readlink', '-f', path).strip()
            except sp.CalledProcessError as error:
                if error.returncode != 1:
                    raise
                return ''

        def confirm_absent(path):
            conn.adb_run('test', '!', '-e', path)
            conn.adb_run('test', '!', '-L', path)

        parent = canonical(device_artifact_dir)
        if not parent:
            confirm_absent(device_artifact_dir)
            print('No previous device artifact directory to reset.')
            return True
        allowed = rf'/storage/emulated/[0-9]+/Android/data/{re.escape(package)}/files/lgl-artifacts'
        if not re.fullmatch(allowed, parent):
            raise ValueError(f'Unexpected canonical artifact directory: {parent}')
        target = canonical(shader_dump_dir)
        if not target:
            # Some Android readlink implementations reject a nonexistent leaf.
            # Confirm both absence and absence of a dangling symlink; never
            # interpret a failed canonicalization as permission to delete.
            confirm_absent(shader_dump_dir)
            print('No previous device shader dump directory to reset.')
            return True
        if target != parent + '/shader-dump':
            raise ValueError(f'Refusing redirected shader dump directory: {target}')
        print(f'Resetting device shader dump data: {target}', flush=True)
        conn.adb_run('rm', '-rf', '--', target)
    except (sp.CalledProcessError, OSError, ValueError) as error:
        detail = getattr(error, 'stderr', None) or getattr(error, 'stdout', None) or ''
        if isinstance(detail, bytes):
            detail = detail.decode('utf-8', errors='replace')
        print(f'ERROR: Cannot prepare device artifact directory: {error}')
        if detail.strip():
            print(f'       Device response: {detail.strip()}')
        return False
    return True


class ShaderDumpProgressReporter:
    '''Render live shader dump telemetry received through the comms endpoint.'''

    def __init__(self, timed_capture: int):
        self.timed_capture = timed_capture
        self.counts = {'shaders': 0, 'pipelines': 0, 'frames': 0}
        self.started = False
        self.unavailable_reason: Optional[str] = None
        self.unavailable_reported = False
        self.start_time = 0.0
        self.last_report_time = 0.0
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.thread: Optional[threading.Thread] = None

    def start(self) -> None:
        '''Begin reporting after the application has been launched.'''
        self.start_time = time.monotonic()
        print('Shader dump capture active; waiting for live progress endpoint.', flush=True)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def mark_unavailable(self, reason: str) -> None:
        '''Record an endpoint setup failure without affecting capture itself.'''
        with self.lock:
            if not self.unavailable_reason:
                self.unavailable_reason = reason

    def handle_message(self, payload: dict) -> None:
        '''Consume one message from the GPUShaderDump comms endpoint.'''
        message_type = payload.get('type')
        if message_type not in ('capture_started', 'progress', 'capture_finished'):
            return

        became_live = False
        with self.lock:
            if not self.started:
                self.started = True
                self.unavailable_reason = None
                became_live = True
            for name in self.counts:
                value = payload.get(name)
                if isinstance(value, int) and value >= 0:
                    self.counts[name] = value

        if became_live:
            print('Shader dump live progress connected.', flush=True)
            self.print_status()

    def print_status(self) -> None:
        '''Print the latest counters, using the same format for both modes.'''
        elapsed = max(0.0, time.monotonic() - self.start_time)
        with self.lock:
            if not self.started:
                return
            counts = self.counts.copy()
            self.last_report_time = elapsed

        elapsed = int(elapsed)
        if self.timed_capture:
            time_status = f'{max(0, self.timed_capture - elapsed)}s remaining'
        else:
            time_status = f'{elapsed}s elapsed'
        print(f'Shader dump capturing | {time_status:<14} | '
              f'SPIR-V {counts["shaders"]:>4} | pipelines {counts["pipelines"]:>4} | '
              f'frames {counts["frames"]:>4}', flush=True)

    def print_unavailable(self) -> None:
        '''Print one explicit status when no endpoint connection is available.'''
        with self.lock:
            if self.started or self.unavailable_reported:
                return
            if self.unavailable_reason is None:
                self.unavailable_reason = 'endpoint did not connect'
            reason = self.unavailable_reason
            self.unavailable_reported = True
        print(f'Shader dump live progress unavailable ({reason}); capture continues.', flush=True)

    def run(self) -> None:
        '''Emit periodic output without polling the device over ADB.'''
        while not self.stop_event.wait(1.0):
            elapsed = time.monotonic() - self.start_time
            with self.lock:
                live = self.started
                unavailable = self.unavailable_reason is not None
                last_report = self.last_report_time
            if not live:
                if unavailable or elapsed >= 5.0:
                    self.print_unavailable()
                    # A startup timeout describes current availability, not the
                    # lifetime of the reporter. A late endpoint must recover.
                continue
            if elapsed - last_report >= 5.0:
                self.print_status()

    def stop(self) -> None:
        '''Stop rendering and report unavailable status for short captures too.'''
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=1.0)
        with self.lock:
            live = self.started
        if not live:
            self.print_unavailable()


def summarize_shader_dump(host_path: str) -> None:
    '''Print the final shader dump file counts after artifact pull.'''
    dump_root = host_path
    if not os.path.isdir(dump_root):
        return
    devices = os.path.join(dump_root, 'devices')
    if os.path.isdir(devices):
        for entry in sorted(os.scandir(devices), key=lambda item: item.name):
            if entry.is_dir():
                print(f'Device capture: {entry.name}')
                summarize_shader_dump(entry.path)
        return

    def count_files(directory: str, suffix: Optional[str] = None) -> int:
        path = os.path.join(dump_root, directory)
        if not os.path.isdir(path):
            return 0
        return sum(1 for entry in os.scandir(path)
                   if entry.is_file() and (suffix is None or entry.name.endswith(suffix)))

    def count_usage_frames() -> int:
        frames = count_files('frames', '.json')
        if frames:
            return frames
        usage_table = os.path.join(dump_root, 'frame_pipeline_usage.csv')
        if not os.path.isfile(usage_table):
            usage_table = os.path.join(dump_root, 'usage_by_label.csv')
        if not os.path.isfile(usage_table):
            return 0
        try:
            with open(usage_table, newline='', encoding='utf-8') as handle:
                return len({row['frame'] for row in csv.DictReader(handle) if row.get('frame')})
        except OSError:
            return 0

    print('Shader dump complete: '
          f'{count_files("shaders", ".spirv")} SPIR-V, '
          f'{count_files("pipelines", ".json")} pipelines, '
          f'{count_usage_frames()} frames')


def pull_artifacts(conn: ADBConnect, host_path: str, device_capture_path: str) -> bool:
    '''Pull the complete artifact tree to the configured host directory.'''
    try:
        # Each host capture is dedicated to shader data; omit the device wrapper.
        conn.adb('pull', f'{device_capture_path}/.', os.path.abspath(host_path))
        roots = [host_path]
        devices = os.path.join(host_path, 'devices')
        if os.path.isdir(devices):
            roots.extend(entry.path for entry in os.scandir(devices) if entry.is_dir())
        moves = []
        for root in roots:
            table_dir = os.path.join(root, 'tables')
            if not os.path.isdir(table_dir):
                continue
            entries = list(os.scandir(table_dir))
            # Check all targets before moving anything; preserve data on collision.
            for entry in entries:
                target = os.path.join(root, entry.name)
                if not entry.is_file() or os.path.exists(target):
                    raise OSError(f'Cannot flatten table without overwriting data: {entry.path}')
            moves.append((root, table_dir, entries))
        for root, table_dir, entries in moves:
            for entry in entries:
                os.rename(entry.path, os.path.join(root, entry.name))
            os.rmdir(table_dir)
    except (sp.CalledProcessError, OSError) as error:
        print(f'ERROR: Failed to pull artifacts; files remain in {device_capture_path}: {error}')
        return False
    print(f'Artifacts written to: {os.path.abspath(host_path)}')
    summarize_shader_dump(host_path)
    return True


def generate_shader_dump_report(layer, host_path: str) -> None:
    '''Run the selected layer's report generator without importing its implementation.'''
    dump_root = host_path
    script = os.path.join(layer.layer_dir, 'scripts', 'shader_dump_report.py')
    report = os.path.join(dump_root, 'shader_usage_report.html')
    if not os.path.isdir(dump_root):
        print(f'WARNING: Shader dump root is missing; report not generated: {dump_root}')
        return
    if not os.path.isfile(script):
        print(f'WARNING: Shader dump report script is missing: {script}')
        return

    try:
        result = sp.run([sys.executable, script, dump_root], check=False,
                        stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
    except OSError as error:
        print(f'WARNING: Failed to run shader dump report: {error}')
        return

    if result.returncode != 0 or not os.path.isfile(report):
        print('WARNING: Shader dump report was not generated; raw artifacts were preserved.')
        if result.stdout:
            print(result.stdout.rstrip())
        print(f'         Re-run manually: {sys.executable} {script} {dump_root}')
        return

    print(f'Shader dump report written to: {report}')



class GPUArtifactHandler:
    """Only this handler knows shader_dump settings, layout and cleanup scope."""
    def enabled(self, layer):
        config = json.loads(layer.config)
        if not isinstance(config, dict):
            raise ValueError('GPU Support configuration must be an object')
        dump = config.get('shader_dump', {})
        return isinstance(dump, dict) and bool(dump.get('enable', False))

    def configure(self, layer, device_root, session_id):
        config = json.loads(layer.config)
        config['shader_dump']['output_path'] = f'{device_root}/shader-dump/{session_id}'
        config['shader_dump'].pop('clear_output', None)
        layer.config = json.dumps(config, indent=4)
        return config['shader_dump']['output_path']

    prepare = staticmethod(prepare_artifact_output)
    pull = staticmethod(pull_artifacts)
    report = staticmethod(generate_shader_dump_report)
    create_progress = staticmethod(ShaderDumpProgressReporter)

    @staticmethod
    def register_progress(instance, progress):
        from lglpy.comms.service_gpu_shader_dump import GPUShaderDumpService
        instance.register_endpoint(GPUShaderDumpService(progress))
