#!/usr/bin/env python3
"""Check the production wheel-friction pivot against v = v0 + A*(p-p0).
Uses the actual Linux update and engine vectorn operations, in float and double.
"""
from pathlib import Path
import re, subprocess, tempfile
src=Path(__file__).resolve().parents[2]
text=(src/'CryPhysics/wheeledvehicleentity.cpp').read_text()
block=re.search(r'sval = s.val\(\);\s*#if defined\(LINUX\)\s*(.*?)#else',text,re.S).group(1)
code=r'''
#include <cstdio>
#include <cmath>
#include <algorithm>
#ifdef TEST_DOUBLE
using real=double;
#else
using real=float;
#endif
constexpr int mtx_foreign_data=1;
template<class T> class matrix_tpl;
#include "vectorn.h"
int main() {
 const real A[4][4]={{4,1,.5,0},{1,3,0,.25},{.5,0,2,.5},{0,.25,.5,2}};
 const real v0[4]={3,-2,1,.5},p0[4]={.5,-.25,0,1};
 const real scales[]={0,.2,-.4,.75,.01};
 vectorn v(4),p(4),dv(4),dp(4);
 for(int i=0;i<4;++i){v[i]=v0[i];p[i]=p0[i];}
 for(int step=0;step<5;++step) {
  for(int i=0;i<4;++i) dp[i]=((i+step)%3-1)*real(.3);
  for(int i=0;i<4;++i){dv[i]=0;for(int j=0;j<4;++j)dv[i]+=A[i][j]*dp[j];}
  real sval=scales[step];
''' + block + r'''
  for(int i=0;i<4;++i) {
   real expected=v0[i];for(int j=0;j<4;++j)expected+=A[i][j]*(p[j]-p0[j]);
   if(std::fabs(v[i]-expected)>1e-5) {
    std::fprintf(stderr,"FAIL: friction pivot %d component %d velocity %.8f expected %.8f\n",step,i,double(v[i]),double(expected));return 1;
   }
  }
 }
 std::puts("PASS: wheel-friction pivots preserve accumulated contact velocity/impulse relationship");
}
'''
with tempfile.TemporaryDirectory() as d:
 root=Path(d);(root/'test.cpp').write_text(code)
 for flags in [[],['-DTEST_DOUBLE']]:
  subprocess.run(['c++','-std=c++11','-fpermissive','-O2','-I'+str(src/'CryPhysics'),str(root/'test.cpp'),'-o',str(root/'test')]+flags,check=True)
  subprocess.run([str(root/'test')],check=True)
