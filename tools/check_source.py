#!/usr/bin/env python3
"""Fast offline checks; deliberately does not claim to compile or test hardware."""
import hashlib,json,re,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[1]
def check(root=R):
 errors=[]
 for p in (root/'app/src').glob('*'):
  if p.suffix not in ('.hpp','.cpp'):continue
  for inc in re.findall(r'^#include\s+"([^"]+)"',p.read_text(),re.M):
   if (inc.startswith('g2h_') or inc.startswith('../')) and not (p.parent/inc).is_file():errors.append(str(p.relative_to(root))+': missing local include '+inc)
 for p in (root/'build').rglob('*'):
  if p.is_file() and p.suffix not in ('.pyc',):
   data=p.read_text(errors='replace')
   if re.search(r'/Users/[^/]+/|/home/[^/]+/',data):errors.append('Personal build path: '+str(p.relative_to(root)))
 lock=json.loads((root/'build/inputs.lock.json').read_text())
 patch=root/lock['matter']['patch']
 if hashlib.sha256(patch.read_bytes()).hexdigest()!=lock['matter']['patch_sha256']:errors.append('SDK patch hash mismatch')
 for item in lock['archives']:
  if not re.fullmatch('[0-9a-f]{64}',item['sha256']) or not item['url'].startswith('https://'):errors.append('Invalid archive lock')
 for name in ['README.md','docs/PAIRING.md']:
  if 'only from **HA 2026.11**' in (root/name).read_text():errors.append('Unverified future-version support claim: '+name)
 return errors
if __name__=='__main__':
 errors=check()
 if errors:print('\n'.join(errors),file=sys.stderr);sys.exit(1)
 print('Offline source layout and pinned-input checks passed (no hardware qualification).')
