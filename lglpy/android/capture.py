# SPDX-License-Identifier: MIT
"""Best-effort, capture-time Android environment snapshots.

Metadata failures must not prevent layer cleanup or artifact recovery. The JSON
is written on the host before launch and finalized before pulling artifacts.
"""

from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess as sp
import tempfile
import uuid


def timestamp():
    """Return a host wall-clock timestamp with its UTC offset."""
    return datetime.now().astimezone().isoformat(timespec='milliseconds')


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def collect_capture_info(conn, layers, args, activity, need_32bit):
    """Collect once, before launch; unavailable fields remain null with warnings."""
    warnings = []
    hash_apks = getattr(args, 'capture_apk_hash', False)

    def attempt(label, callback, default=None):
        try:
            return callback()
        except (OSError, sp.SubprocessError, ValueError, KeyError) as error:
            warnings.append(f'{label}: {error}')
            return default

    props_text = attempt('Android properties', lambda: conn.adb_run('getprop'), '')
    props = dict(re.findall(r'^\[([^]]+)\]: \[(.*)\]$', props_text, re.M))
    device = {key: props.get(prop) or None for key, prop in {
        'manufacturer': 'ro.product.manufacturer', 'model': 'ro.product.model',
        'product': 'ro.product.name', 'device': 'ro.product.device',
        'soc_manufacturer': 'ro.soc.manufacturer', 'soc_model': 'ro.soc.model',
        'hardware': 'ro.hardware'}.items()}
    os_info = {key: props.get(prop) or None for key, prop in {
        'android_version': 'ro.build.version.release', 'sdk_level': 'ro.build.version.sdk',
        'build_fingerprint': 'ro.build.fingerprint',
        'security_patch': 'ro.build.version.security_patch'}.items()}
    package = {'package_name': conn.package, 'version_name': None, 'version_code': None,
               'version_source': None, 'activity': activity,
               'process_bitness': 32 if need_32bit else 64, 'execution_abi': None}
    package_dump = attempt('Package Manager', lambda: conn.adb_run('dumpsys', 'package', conn.package), '')
    for key, pattern in [('version_name', r'^\s*versionName=(.*)$'),
                         ('version_code', r'\bversionCode=(\d+)'),
                         ('execution_abi', r'\bprimaryCpuAbi=([^\s]+)')]:
        match = re.search(pattern, package_dump, re.M)
        if match and match[1].strip() != 'null':
            package[key] = match[1].strip()
    # secondaryCpuAbi can be selected by the installer's 32-bit detection.
    abi = package['execution_abi']
    if abi and ('64' in abi) == need_32bit:
        match = re.search(r'\bsecondaryCpuAbi=([^\s]+)', package_dump)
        package['execution_abi'] = match[1] if match and match[1] != 'null' else None
    if package['version_name'] is not None or package['version_code'] is not None:
        package['version_source'] = 'Package Manager (dumpsys package)'
    paths_text = attempt('APK paths', lambda: conn.adb_run('pm', 'path', conn.package), '')
    paths = [line[8:].strip() for line in paths_text.splitlines() if line.startswith('package:')]
    if not paths:
        warnings.append('APK paths: no installed APK paths returned')
    package['apk_files'] = [{'name': Path(path).name, 'sha256': None} for path in paths]
    def hash_installed_apks():
        with tempfile.TemporaryDirectory(prefix='lgl-apk-info-') as directory:
            for index, path in enumerate(paths):
                local = Path(directory) / f'{index}.apk'
                print(f'  Pulling {Path(path).name} for APK hashes ...', flush=True)
                conn.adb('pull', path, str(local))
                package['apk_files'][index]['sha256'] = file_sha256(local)
                local.unlink()
    if hash_apks:
        attempt('APK hashes', hash_installed_apks)

    tool_layers = []
    for layer in layers:
        config = attempt(f'{layer.name} config', lambda: json.loads(layer.config)) if layer.config else None
        manifest_path = Path(layer.layer_dir) / 'manifest.json'
        manifest = attempt(f'{layer.name} manifest', lambda: json.loads(manifest_path.read_text(encoding='utf-8')), {})
        tool_layers.append({'name': layer.name, 'binary': Path(layer.host_path).name,
                            'manifest_version': manifest.get('layer', {}).get('implementation_version'),
                            'binary_sha256': attempt(f'{layer.name} SHA-256', lambda: file_sha256(layer.host_path)),
                            'effective_config': config})
    repo = Path(__file__).resolve().parents[2]
    revision = attempt('Git revision', lambda: sp.run(
        ['git', '-C', str(repo), 'rev-parse', 'HEAD'], capture_output=True,
        text=True, check=True, timeout=5).stdout.strip())
    dirty = attempt('Git working tree', lambda: bool(sp.run(
        ['git', '-C', str(repo), 'status', '--porcelain', '--untracked-files=no'],
        capture_output=True, text=True, check=True, timeout=5).stdout.strip()))
    support = next((layer for layer in layers if layer.name == 'VK_LAYER_LGL_gpu_support'), None)
    generator = Path(support.layer_dir) / 'scripts' / 'shader_dump_report.py' if support else None
    return {
        'schema_version': 1, 'device': device, 'os': os_info, 'application': package,
        'capture': {'id': str(uuid.uuid4()), 'metadata_collected_at': timestamp(),
                    'started_at': None, 'ended_at': None, 'elapsed_seconds': None,
                    'status': 'prepared', 'artifact_status': 'pending',
                    'requested_duration_seconds': args.timed_capture or None,
                    'auto_start': args.auto_start,
                    'package_arguments': args.package_arguments},
        'tools': {'git_revision': revision, 'working_tree_modified': dirty, 'layers': tool_layers,
                  'installer_sha256': attempt('Installer SHA-256', lambda: file_sha256(repo / 'lgl_android_install.py')),
                  'metadata_collector_sha256': attempt('Metadata collector SHA-256', lambda: file_sha256(__file__)),
                  'report_generator_sha256': attempt('Report generator SHA-256', lambda: file_sha256(generator)) if generator else None},
        'notes': {key: getattr(args, 'capture_' + key, None)
                  for key in ('scene', 'quality', 'resolution', 'note')},
        'warnings': warnings,
    }


def save_capture_info(host_path, info):
    """Persist the snapshot atomically without making capture failure-prone."""
    try:
        path = Path(host_path) / 'capture_info.json'
        temporary = path.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(info, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        temporary.replace(path)
    except OSError as error:
        print(f'WARNING: Capture metadata could not be saved: {error}')
