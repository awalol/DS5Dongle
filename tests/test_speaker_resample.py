"""Host regression: python tests/test_speaker_resample.py (requires g++ on PATH).

Builds the speaker path's resampler and WDL_Resampler, as the firmware builds
it, and checks that they give the same audio. See speaker_resample_test.cpp.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SpeakerResampleTests(unittest.TestCase):
    def test_matches_wdl(self):
        wdl = ROOT / 'lib/WDL/WDL'
        self.assertTrue((wdl / 'resample.cpp').exists(),
                        'lib/WDL is not checked out: git submodule update --init lib/WDL')
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'speaker_resample_test'
            subprocess.run(
                ['g++', '-std=c++17', '-O2', '-Wall', '-fsigned-char',
                 '-DWDL_RESAMPLE_TYPE=float', '-DPORT_FAST_FUNC(fn)=fn',
                 '-I', str(wdl), '-I', str(ROOT / 'src'),
                 str(ROOT / 'tests/speaker_resample_test.cpp'),
                 str(ROOT / 'src/speaker_resample.cpp'),
                 str(ROOT / 'src/wdl_resample.cpp'),
                 '-o', str(exe)],
                check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            print(result.stdout, end='')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
