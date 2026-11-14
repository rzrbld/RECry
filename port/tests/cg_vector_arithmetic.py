#!/usr/bin/env python3
"""Compile and run Cg arithmetic pixel checks on an SDL2 OpenGL ES 3 context.
Requires a desktop C++ compiler and pkg-config packages sdl2 and glesv2.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

src = Path(__file__).resolve().parents[2]
flags = shlex.split(subprocess.check_output(
    ['pkg-config', '--cflags', '--libs', 'sdl2', 'glesv2'], text=True))
with tempfile.TemporaryDirectory() as tmp:
    binary = str(Path(tmp) / 'cg-vector-arithmetic')
    subprocess.run([
        'c++', '-std=c++11', '-O2',
        str(src / 'port/tests/cg_vector_arithmetic.cpp'),
        str(src / 'RenderDll/XRenderGLES3/GLRCg.cpp'),
        '-o', binary, *flags,
    ], check=True)
    subprocess.run([binary], env=dict(os.environ, SDL_VIDEODRIVER='offscreen'), check=True)
