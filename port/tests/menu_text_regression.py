#!/usr/bin/env python3
"""Exercise production UI localization of checkpoint dates on Linux.
--source accepts StringTableMgr.cpp from an older build as a negative control.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2]/'CryGame/StringTableMgr.cpp')
args=parser.parse_args()
s=args.source.read_text()
def method(signature):
 a=s.index(signature);b=s.index('{',a)+1;depth=1
 while depth:
  depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
methods='\n'.join(method('void CStringTableMgr::'+n+'(') for n in ['Localize','AppendToUnicodeString','AppendToAsciiString'])
code=r'''
#include <string>
#include <vector>
#include <cwchar>
#include <cstdio>
#include <cassert>
#include <clocale>
using std::string;using std::wstring;
struct CStringTableMgr {
 void Localize(const string&,wstring&,bool=false);
 void AppendToUnicodeString(const string&,wstring&);
 void AppendToAsciiString(const string&,string&);
 bool LookupString(const char* k,wstring& v){assert(string(k)=="@CheckpointLast");v=L"Last checkpoint";return true;}
 bool LookupEnglishString(const char* k,string& v){assert(string(k)=="@CheckpointLast");v="Last checkpoint";return true;}
};
'''+methods+r'''
int main(){
 std::setlocale(LC_ALL,"C");CStringTableMgr mgr;
 // Match the date/time label generated in InGameSingle.lua; localize the last
 // checkpoint suffix while preserving every digit and punctuation character.
 for(bool english:{false,true}){
  wstring result;mgr.Localize("11/14/2026 [19:07:41] @CheckpointLast",result,english);
  assert(result==L"11/14/2026 [19:07:41] Last checkpoint");
 }
 wstring result;mgr.Localize("01/02/2026 [00:00:00]",result);assert(result==L"01/02/2026 [00:00:00]");
 result.clear();mgr.Localize("",result);assert(result.empty());
 string plain="prefix ";mgr.AppendToAsciiString("Checkpoint 123",plain);assert(plain=="prefix Checkpoint 123");
 std::puts("PASS: checkpoint timestamp digits/punctuation and localized suffix survive Linux menu conversion");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);(root/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-DLINUX',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True,cwd=root)
