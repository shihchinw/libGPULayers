"""Run with python -m unittest discover -s layer_gpu_support/scripts -p 'test_*.py'."""
import struct
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import shader_dump_report as report


def module(*entries):
    words = [0x07230203, 0x10000, 0, 10, 0]
    for model, name in entries:
        encoded = name.encode() + b'\0'
        encoded += b'\0' * (-len(encoded) % 4)
        operands = [model, 1, *struct.unpack(f'<{len(encoded)//4}I', encoded)]
        words += [((len(operands) + 1) << 16) | 15, *operands]
    return struct.pack(f'<{len(words)}I', *words)


class ShaderConversionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.shader_id = 'a' * 40
        (self.root / 'shaders').mkdir()
        self.source = self.root / 'shaders' / f'{self.shader_id}.spirv'
        self.source.write_bytes(module((4, 'main')))

    @patch.object(report.shutil, 'which', side_effect=lambda value: value)
    @patch.object(report.subprocess, 'run')
    def test_conversion_cache_force_and_formats(self, run, which):
        run.return_value = subprocess.CompletedProcess([], 0, b'code\n', b'')
        report.convert_shader(self.root, self.shader_id, ['glsl', 'hlsl', 'spvasm'])
        self.assertEqual(run.call_count, 3)
        calls = [call.args[0] for call in run.call_args_list]
        self.assertIn('--vulkan-semantics', calls[0])
        self.assertIn('--hlsl', calls[1])
        self.assertEqual(calls[2][0], 'spirv-dis')
        report.convert_shader(self.root, self.shader_id, ['glsl'])
        self.assertEqual(run.call_count, 3)
        report.convert_shader(self.root, self.shader_id, ['glsl'], force=True)
        self.assertEqual(run.call_count, 4)

    @patch.object(report.shutil, 'which', side_effect=lambda value: value)
    @patch.object(report.subprocess, 'run')
    def test_failure_preserves_assembly_and_exits_with_error(self, run, which):
        run.side_effect = [subprocess.CompletedProcess([], 1, b'', b'unsupported extension'),
                           subprocess.CompletedProcess([], 0, b'; assembly', b'')]
        with self.assertRaisesRegex(ValueError, 'unsupported extension'):
            report.convert_shader(self.root, self.shader_id, ['glsl', 'spvasm'])
        output = self.root / 'shader_sources'
        self.assertFalse((output / f'{self.shader_id}.glsl').exists())
        self.assertEqual((output / f'{self.shader_id}.spvasm').read_text(), '; assembly')

    @patch.object(report.shutil, 'which', return_value='tool')
    @patch.object(report.subprocess, 'run')
    def test_multiple_entries_and_spv_input(self, run, which):
        self.source.unlink()
        self.source.with_suffix('.spv').write_bytes(module((0, 'main'), (4, 'main')))
        run.return_value = subprocess.CompletedProcess([], 0, b'code', b'')
        report.convert_shader(self.root, self.shader_id, ['glsl', 'spvasm'])
        names = [p.name for p in (self.root / 'shader_sources').iterdir()]
        self.assertEqual(len(names), 3)
        self.assertTrue(any('.vert.main-' in name for name in names))
        self.assertTrue(any('.frag.main-' in name for name in names))

    def test_invalid_id_binary_and_missing_tools(self):
        with self.assertRaisesRegex(ValueError, '40 hexadecimal'):
            report.convert_shader(self.root, '../bad', ['glsl'])
        with patch.object(report.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'not found'):
                report.convert_shader(self.root, self.shader_id, ['spvasm'])
        self.source.write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError, 'length'):
            report.convert_shader(self.root, self.shader_id, ['spvasm'])

    @patch.object(report, 'convert_shader')
    def test_batch_continues_after_failure_and_deduplicates(self, convert):
        (self.root / 'shaders' / 'named.spv').write_bytes(module((0, 'vertex')))
        (self.root / 'shaders' / 'copy.spv').write_bytes(self.source.read_bytes())
        convert.side_effect = [ValueError('unsupported'), None]
        failed = report.convert_all_shaders(self.root, ['glsl', 'spvasm'], force=True, jobs=1)
        self.assertEqual(failed, 1)
        self.assertEqual(convert.call_count, 2)
        for call in convert.call_args_list:
            source = call.kwargs['source_path']
            self.assertEqual(call.args[1], hashlib.sha1(source.read_bytes()).hexdigest())
            self.assertTrue(call.args[3])

    @patch.object(report, 'convert_shader')
    def test_batch_flat_folder_and_empty_folder(self, convert):
        flat = self.root / 'flat'
        flat.mkdir()
        with self.assertRaisesRegex(ValueError, 'No .spv'):
            report.convert_all_shaders(flat, ['spvasm'])
        (flat / 'example.SPV').write_bytes(module((5, 'compute')))
        self.assertEqual(report.convert_all_shaders(flat, ['spvasm']), 0)
        self.assertEqual(convert.call_args.kwargs['source_path'], flat / 'example.SPV')

    @patch.object(report, 'convert_shader')
    @patch.object(report, 'convert_all_shaders', return_value=0)
    def test_cli_star_selects_batch_and_sha1_selects_single(self, batch, single):
        with patch('sys.argv', ['shader_dump_report.py', str(self.root), '--convert-shader', '*', '--jobs', '3']):
            report.main()
        batch.assert_called_once_with(self.root, ['glsl', 'spvasm'], False, 'spirv-cross', 'spirv-dis', 3)
        single.assert_not_called()
        with patch('sys.argv', ['shader_dump_report.py', str(self.root), '--convert-shader', self.shader_id]):
            report.main()
        single.assert_called_once_with(self.root, self.shader_id, ['glsl', 'spvasm'], False, 'spirv-cross', 'spirv-dis')

    def test_batch_rejects_invalid_worker_count(self):
        for jobs in (0, -1):
            with self.subTest(jobs=jobs), self.assertRaisesRegex(ValueError, '--jobs'):
                report.convert_all_shaders(self.root, ['spvasm'], jobs=jobs)

    @patch.object(report.os, 'cpu_count', return_value=12)
    @patch.object(report, 'ProcessPoolExecutor')
    @patch.object(report, 'as_completed', return_value=[])
    def test_batch_defaults_to_cpu_count(self, completed, pool, cpu_count):
        for index in range(12):
            (self.root / 'shaders' / f'{index}.spv').write_bytes(module((4, f'entry{index}')))
        report.convert_all_shaders(self.root, ['spvasm'])
        pool.assert_called_once_with(max_workers=12)


if __name__ == '__main__':
    unittest.main()
