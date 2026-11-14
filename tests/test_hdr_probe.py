"""Validate HDR evidence interpretation, including stale raw files and geometry candidates."""
import csv
import importlib.util
import math
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('hdr_probe', Path(__file__).resolve().parents[1]/'devtools/hdr_probe.py')
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class HdrProbeTests(unittest.TestCase):
    def test_nonfinites_and_real_finite_extrema(self):
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory)/'image.f32'
            values = [2, 3, 4, math.inf, -math.inf, math.nan]
            raw.write_bytes(struct.pack('<6f', *values))
            r = probe.analyze_rgb(raw, 2, 1)
            self.assertEqual((r['finite_min'], r['finite_max'], r['finite_mean']), (2, 4, 3))
            self.assertEqual((r['infinite_channels'], r['nan_channels'], r['nonfinite_pixels']), (2, 1, 1))
            self.assertEqual(r['examples'], [{'x': 1, 'y': 0, 'rgb': ['inf', '-inf', 'nan']}])

    def test_no_finite_values_and_example_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory)/'image.f32'
            raw.write_bytes(struct.pack('<6f', *([math.inf]*6)))
            r = probe.analyze_rgb(raw, 1, 2, 1)
            self.assertIsNone(r['finite_mean'])
            self.assertIsNone(r['finite_min'])
            self.assertEqual(r['nonfinite_pixels'], 2)
            self.assertEqual(len(r['examples']), 1)

    def test_size_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory)/'image.f32'
            raw.write_bytes(b'old dump')
            with self.assertRaises(ValueError):
                probe.analyze_rgb(raw, 2, 1)

    def test_log_format_prevents_stale_raw_interpretation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root/'dump.log'
            log.write_text("pass dump 00 is the submission's pass 8 (present 123)\n"
                           "pass dump 00: 2x1 format 37 -> /files/pass_00.ppm\n"
                           "pass dump 01 is the submission's pass 30 (present 124)\n"
                           "pass dump 01: 1x1 format 122 -> /files/pass_01.ppm\n")
            (root/'pass_00.ppm.f32').write_bytes(b'stale and invalid')
            (root/'pass_01.ppm.f32').write_bytes(struct.pack('<3f', 1, 2, 3))
            rows = probe.analyze_dumps(log)['dumps']
            self.assertEqual(rows[0]['status'], 'skipped_non_hdr_format')
            self.assertEqual(rows[1]['finite_max'], 3)
            self.assertEqual((rows[1]['render_pass'], rows[1]['present']), (30, 124))

    def test_missing_hdr_payload_is_not_silently_clean(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory)/'dump.log'
            log.write_text('pass dump 00: 1x1 format 97 -> /files/pass_00.ppm\n')
            with self.assertRaises(FileNotFoundError):
                probe.analyze_dumps(log)

    def test_reused_dump_index_requires_separate_capture_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory)/'dump.log'
            log.write_text('pass dump 00: 1x1 format 37 -> /files/pass_00.ppm\n' * 2)
            with self.assertRaisesRegex(ValueError, 'reused'):
                probe.analyze_dumps(log)

    def test_history_keeps_frame_and_draw_shape_in_candidates(self):
        fields = ['event', 'frame', 'draw', 'kind', 'prepared', 'vs', 'fs', 'vb', 'ib',
                  'count', 'instances', 'base', 'hdr_word', 'depth_word', 'depth_format']
        rows = [dict(zip(fields, [str(i), '10', str(i), '4', '1', 'vs', 'fs', '100', '200',
                                 '3', '1', '0', 'hdr', 'depth', '126'])) for i in range(4)]
        rows[1]['frame'] = '11'
        rows[2]['instances'] = '2'
        rows[3]['hdr_word'] = 'newhdr'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'history.tsv'
            with path.open('w', newline='') as stream:
                stream.write('# intrusive diagnostic\n')
                writer = csv.DictWriter(stream, fieldnames=fields, delimiter='\t')
                writer.writeheader()
                writer.writerows(rows)
            result = probe.analyze_history(path)
            self.assertEqual(result['events'], 4)
            self.assertEqual([r['event'] for r in result['changes']], ['0', '3'])
            self.assertEqual(result['geometry_candidates'][0]['candidate_events'], ['0', '3'])

    def test_history_requires_expected_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'history.tsv'
            path.write_text('event\tr\tg\tb\n')
            with self.assertRaises(ValueError):
                probe.analyze_history(path)
