#!/usr/bin/env python3
"""Package the requested Windows artifact without toolchains or runtime caches."""
import hashlib
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
exe = root / 'releases' / 'Edgecourt.exe'
if not exe.is_file() or exe.read_bytes()[:2] != b'MZ':
    raise SystemExit('Build releases/Edgecourt.exe first (valid Windows PE required).')
zip_path = root / 'releases' / 'Edgecourt-Windows-x64.zip'
entries = {
    exe: 'Edgecourt.exe',
    root / 'releases' / 'WINDOWS_README.txt': 'READ_ME_FIRST.txt',
    root / 'LICENSE': 'LICENSE.txt',
    root / 'THIRD_PARTY_NOTICES.md': 'THIRD_PARTY_NOTICES.md',
    root / 'vendor' / 'httplib' / 'LICENSE': 'licenses/cpp-httplib-MIT.txt',
    root / 'vendor' / 'nlohmann' / 'LICENSE.MIT': 'licenses/nlohmann-json-MIT.txt',
}
for folder in ['docs', 'samples', 'vendor/runtime-licenses']:
    for file in sorted((root / folder).glob('*')):
        if file.is_file():
            entries[file] = f'licenses/{file.name}' if folder.startswith('vendor/') else f'{folder}/{file.name}'
with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for source, target in entries.items():
        archive.write(source, target)
    archive.writestr("SHA256SUMS.txt", f"{hashlib.sha256(exe.read_bytes()).hexdigest()}  Edgecourt.exe\n")
with (root / 'releases' / 'SHA256SUMS.txt').open('w', encoding='utf-8') as output:
    for file in [exe, zip_path]:
        output.write(f'{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.name}\n')
print(f'Packaged {len(entries)+1} files in {zip_path.name} ({zip_path.stat().st_size:,} bytes)')
