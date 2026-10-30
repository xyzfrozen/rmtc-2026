#!/usr/bin/env python3
"""Check the published source manifest; local binary hashes are informational."""
import hashlib,json,pathlib,sys

root=pathlib.Path(__file__).resolve().parents[1]
manifest=json.loads((root/'MANIFEST.json').read_text())
failed=[]
for relative,expected in manifest['source_sha256'].items():
    path=pathlib.Path(relative)
    if path.is_absolute() or '..' in path.parts:
        failed.append(relative+' (invalid path)');continue
    path=root/path
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=expected:
        failed.append(relative)
if failed:
    print('源码校验失败：\n'+'\n'.join(failed),file=sys.stderr);raise SystemExit(1)
print(f"源码SHA-256校验通过：{len(manifest['source_sha256'])}个文件。跨电脑不要求二进制哈希相同。")
