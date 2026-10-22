# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Compile the actual bounded MP3 worker and run it on owned synthetic audio."""
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from common import ROOT, LOCK, dependency


def main():
    header = ROOT / 'native/shared/third_party/dr_mp3.h'
    assert hashlib.sha256(header.read_bytes()).hexdigest() == LOCK['drMp3']['sha256']
    compiler = os.environ.get('CXX', 'clang++')
    if not shutil.which(compiler) or not shutil.which('ffmpeg'):
        raise RuntimeError('Music tests require a native C++20 compiler and FFmpeg')
    hui = dependency('platform')
    with tempfile.TemporaryDirectory(prefix='ps5-music-') as temporary:
        work = Path(temporary)
        mono, stereo, invalid = work/'mono.mp3', work/'stereo.mp3', work/'invalid.mp3'
        for file, rate, channels in ((mono, 22050, 1), (stereo, 44100, 2)):
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                            f'sine=frequency=440:duration=0.4:sample_rate={rate}',
                            '-ac', str(channels), '-c:a', 'libmp3lame', '-y', str(file)], check=True)
        invalid.write_bytes(b'ID3\x04\x00\x00\x00\x00\x00\x00not an MP3 frame')
        binary = work / 'music-client'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(ROOT/'native/shared'), '-I', str(hui/'src'),
                        str(ROOT/'native/shared/music_player.cpp'),
                        str(ROOT/'native/shared/tests/music_client.cpp'), '-o', str(binary)], check=True)
        output, reference = work/'decoded.f32', work/'reference.f32'
        env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
        subprocess.run([str(binary), str(mono), str(stereo), str(invalid), str(output)], env=env, check=True, timeout=45)
        subprocess.run(['ffmpeg', '-v', 'error', '-i', str(stereo), '-ar', '48000', '-ac', '2',
                        '-f', 'f32le', '-y', str(reference)], check=True)
        actual, expected = array('f'), array('f')
        actual.frombytes(output.read_bytes()); expected.frombytes(reference.read_bytes())
        assert abs(len(actual)-len(expected)) <= 4, (len(actual), len(expected))
        count = min(len(actual), len(expected))
        rmse = math.sqrt(sum((actual[i]-expected[i])**2 for i in range(count)) / count)
        assert rmse < 0.002, rmse
        print(json.dumps({'passed_groups': 5, 'reference_sample_count': count, 'reference_rmse': rmse,
                          'sanitizers': ['address', 'undefined', 'leaks'], 'decoder_sha256': LOCK['drMp3']['sha256']}))


if __name__ == '__main__':
    main()
