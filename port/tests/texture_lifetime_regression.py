#!/usr/bin/env python3
"""Exercise real texture upload/allocation on GLES, including legacy ID overflow."""
from pathlib import Path
import re, subprocess, tempfile, sys
src=Path(__file__).resolve().parents[2];root=src/'RenderDll/XRenderGLES3'
core=(root/'GLRCore.cpp').read_text()
a=core.index('static std::unordered_map<unsigned, unsigned> s_textureTargets;')
b=core.index('void BindTexture(',a)
policy=re.search(r'void CGLTexMan::SetTexture\(int Id, ETexType eTT\).*?if\(([^\n]+)\)',(root/'GLTextures.cpp').read_text(),re.S).group(1)
metadata='#include <GLES3/gl3.h>\n#include <unordered_map>\nnamespace GLR {\n'+core[a:b]+'}\nbool EngineRejectsTexture(int Id) { const int TX_LASTBIND=0x4000; return '+policy+'; }\n'
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'metadata.cpp').write_text(metadata)
 flags=subprocess.check_output(['pkg-config','--cflags','--libs','sdl2','glesv2'],text=True).split()
 subprocess.run(['c++','-std=c++11','-O2',str(Path(__file__).with_suffix('.cpp')),str(root/'GLTexUpload.cpp'),str(p/'metadata.cpp'),'-o',str(p/'test')]+flags+['-pthread'],check=True)
 subprocess.run([str(p/'test')],check=True)
 if '--prove-previous-bug' in sys.argv:
  # Recreate the pre-fix allocator/policy without changing the working source.
  old=(root/'GLTexUpload.cpp').read_text().replace('static GLuint s_nextTexName = 0x10000000;', 'static GLuint s_nextTexName = 1;')
  (p/'old.cpp').write_text(old)
  (p/'metadata.cpp').write_text(metadata.replace('return '+policy+';', 'return Id < 0 || Id >= TX_LASTBIND;'))
  subprocess.run(['c++','-std=c++11','-O2','-I'+str(root),str(Path(__file__).with_suffix('.cpp')),str(p/'old.cpp'),str(p/'metadata.cpp'),'-o',str(p/'old')]+flags+['-pthread'],check=True)
  result=subprocess.run([str(p/'old')])
  if result.returncode==0: raise SystemExit('FAIL: regression did not detect the old allocation policy')
  print('PASS: previous texture allocation policy fails the same regression as expected')
