#!/usr/bin/env python3
"""Check actual GLES spotlight texture sampling. --upload accepts an old backend."""
from pathlib import Path
import argparse, subprocess, tempfile, shlex
src=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--upload',type=Path,default=src/'RenderDll/XRenderGLES3/GLTexUpload.cpp')
args=parser.parse_args()
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','sdl2','glesv2'],text=True))
with tempfile.TemporaryDirectory() as tmp:
 binary=str(Path(tmp)/'test')
 subprocess.run(['c++','-std=c++11','-O2','-I'+str(src/'RenderDll/XRenderGLES3'),str(Path(__file__).with_suffix('.cpp')),str(args.upload),'-o',binary,*flags,'-pthread'],check=True)
 subprocess.run([binary],check=True,cwd=tmp)
