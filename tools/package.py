"""Package the own sources and selected distribution without user profiles or logs."""
import argparse
import hashlib
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dist',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    distribution=args.dist.resolve(); output=args.output.resolve()
    if not distribution.is_dir(): raise SystemExit('Distribution directory missing')
    files=[f for f in distribution.rglob('*') if f.is_file() and
           (f.suffix.lower() in ('.exe','.dll','.png','.txt') and f.name != 'SHA256SUMS.txt')]
    sums='\n'.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.relative_to(distribution).as_posix() for f in sorted(files))+'\n'
    (distribution/'SHA256SUMS.txt').write_text(sums,encoding='utf-8')
    files.append(distribution/'SHA256SUMS.txt')
    prefix='hvh-menu-x64/'
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for f in sorted(files):archive.write(f,prefix+'dist/'+f.relative_to(distribution).as_posix())
        for folder in ('src','include','tools','tests'):
            for f in sorted((ROOT/folder).rglob('*')):
                if f.is_file() and '__pycache__' not in f.parts and f.suffix in ('.cpp','.hpp','.h','.py','.ps1'):
                    archive.write(f,prefix+f.relative_to(ROOT).as_posix())
        for name in ('CMakeLists.txt','TESTEN.txt','THIRD_PARTY.txt','.gitignore'):
            archive.write(ROOT/name,prefix+name)
        for f in sorted((ROOT/'analysis').glob('*')):
            if f.is_file() and f.suffix in ('.txt','.json','.c'):
                archive.write(f,prefix+'analysis/'+f.name)
        for f in sorted((ROOT/'build/test-results').glob('*.json')):
            archive.write(f,prefix+'test-results/'+f.name)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:raise SystemExit('ZIP CRC validation failed')
        if any(name.endswith(('.ini','.log')) for name in archive.namelist()):raise SystemExit('Unexpected user data in ZIP')
    print('Packaged',output,output.stat().st_size,'bytes')

if __name__=='__main__':main()
