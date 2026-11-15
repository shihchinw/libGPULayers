# SPDX-License-Identifier: MIT
"""General artifact coordination; handlers own layer-specific config and scope."""
from dataclasses import dataclass
import os
import re
import time
import uuid
from typing import Any, Optional, Protocol


class ArtifactHandler(Protocol):
    """Host contract for registered layers.

    prepare must preserve data unless reset is true, validate its layer-owned
    scope before deletion, and never reset the shared device_root wholesale.
    prepare/pull report operational failure with False; report is best-effort.
    """
    def enabled(self, layer: Any) -> bool: ...
    def configure(self, layer: Any, device_root: str, session_id: str) -> str: ...
    def prepare(self, conn: Any, package: str, device_root: str, reset: bool) -> bool: ...
    def pull(self, conn: Any, host_path: str, device_path: str) -> bool: ...
    def report(self, layer: Any, host_path: str) -> None: ...
    def create_progress(self, timed_capture: int) -> Any: ...
    def register_progress(self, server: Any, progress: Any) -> None: ...


def _gpu_support_handler():
    from layer_gpu_support.scripts.android_artifacts import GPUArtifactHandler
    return GPUArtifactHandler()


# Explicit, trusted registry; no executable plugin path comes from layer config.
HANDLER_FACTORIES = {'VK_LAYER_LGL_gpu_support': _gpu_support_handler}


@dataclass
class ArtifactCapture:
    layer: Any
    handler: ArtifactHandler
    host_path: str
    device_path: str


def configure_artifact_output(layers: list, host_root: str, device_root: str,
                              package: str) -> tuple[bool, list[ArtifactCapture], Optional[str]]:
    selected = []
    try:
        for layer in layers:
            factory = HANDLER_FACTORIES.get(layer.name)
            if factory is None:
                print(f'Artifact capture not supported by {layer.name}; skipping.')
                continue
            if not re.fullmatch(r'[A-Za-z0-9_]+', layer.name):
                raise ValueError('Invalid artifact layer directory name')
            handler = factory()
            if handler.enabled(layer):
                selected.append((layer, handler))
            else:
                print(f'Artifact capture disabled for {layer.name}; skipping.')
        if not selected:
            return True, [], None
        os.makedirs(host_root, exist_ok=True)
        base = f'{time.strftime("%Y%m%d_%H%M%S")}_{package}'
        if not re.fullmatch(r'[A-Za-z0-9_.]+', package):
            raise ValueError('Invalid package name for artifact directory')
        for suffix in range(10000):
            name = base if suffix == 0 else f'{base}_{suffix:02d}'
            capture_root = os.path.abspath(os.path.join(host_root, name))
            try:
                os.mkdir(capture_root)
                break
            except FileExistsError:
                continue
        else:
            raise OSError('Cannot allocate a unique artifact capture directory')
        session_id = f'session-{uuid.uuid4().hex}'
        plans = []
        for layer, handler in selected:
            host_path = capture_root if len(selected) == 1 else os.path.join(capture_root, layer.name)
            os.makedirs(host_path, exist_ok=True)
            device_path = handler.configure(layer, device_root, session_id)
            plans.append(ArtifactCapture(layer, handler, host_path, device_path))
        return True, plans, capture_root
    except (OSError, ValueError, TypeError) as error:
        print(f'ERROR: Cannot configure artifact capture: {error}')
        return False, [], None


def save_artifact_info(host_root, plans, info, save):
    """Each handler report can discover the common capture snapshot locally."""
    for path in dict.fromkeys([host_root, *(plan.host_path for plan in plans)]):
        save(path, info)
