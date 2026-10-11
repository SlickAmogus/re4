#!/usr/bin/env python3
"""Extracts the two retail GameCube discs (RVZ/ISO/GCM) with decomp-toolkit.

    python3 xbox/tools/extract_discs.py <disc1.rvz> <disc2.rvz> <out dir>

Downloads dtk v1.8.3 for the host platform into <out dir>/tools/ if it is not on PATH, then runs
`dtk vfs cp "<image>:sys" out/discN/sys` and `... "<image>:files" out/discN/files`. The output tree is
what xbox/tools/re4conv (CT-04) and the DVD shim read. Nothing here is ever committed.
"""
import os
import platform
import shutil
import subprocess
import sys
import urllib.request

DTK_TAG = 'v1.8.3'


def dtk_url():
    system = {'Windows': 'windows', 'Linux': 'linux', 'Darwin': 'macos'}[platform.system()]
    arch = {'AMD64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'arm64', 'aarch64': 'arm64'}[platform.machine()]
    suffix = '.exe' if system == 'windows' else ''
    return f'https://github.com/encounter/decomp-toolkit/releases/download/{DTK_TAG}/dtk-{system}-{arch}{suffix}', suffix


def find_dtk(out):
    exe = shutil.which('dtk')
    if exe:
        return exe
    url, suffix = dtk_url()
    os.makedirs(os.path.join(out, 'tools'), exist_ok=True)
    exe = os.path.join(out, 'tools', 'dtk' + suffix)
    if not os.path.exists(exe):
        print('downloading', url)
        urllib.request.urlretrieve(url, exe)
        os.chmod(exe, 0o755)
    return exe


def extract(dtk, image, dest):
    for part in ('sys', 'files'):
        os.makedirs(os.path.join(dest, part), exist_ok=True)
        subprocess.check_call([dtk, 'vfs', 'cp', f'{image}:{part}', os.path.join(dest, part) + os.sep])


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    disc1, disc2, out = sys.argv[1:4]
    dtk = find_dtk(out)
    extract(dtk, disc1, os.path.join(out, 'disc1'))
    extract(dtk, disc2, os.path.join(out, 'disc2'))
    print('done:', out)


if __name__ == '__main__':
    main()
