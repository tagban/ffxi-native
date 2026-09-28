"""The Windows game host (xi-host.exe) for the launcher to ship, built with zig from any OS.

  python3 tools/windows_dist.py [--zig <zig>] [--work <dir>]
      build/windows-x86_64/xi-host.exe and SDL3.dll beside it

host64 without the translation (XI_SPLIT, runtime/xi_game.h), as tools/build.py's host64 builds it
with MSVC, but by zig's clang against MinGW-w64: the same sources, Direct3D 12, SChannel for the
LandSandBoat sign-in. SDL3 is its released MinGW build (pinned by SHA-256); its SDL3.dll goes beside
the host. The launcher makes the game module on the player's machine with zig too, so a Windows
player needs nothing installed.
"""
import argparse
import concurrent.futures
import hashlib
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build  # noqa: E402  (the source lists)

TARGET = 'x86_64-windows-gnu'
SDL = ('https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-devel-3.4.16-mingw.tar.gz',
       'c7ef65bd72eabac6e5b535411dbd8d5824d0aab24fd62ff8812666b336f18a9c', 'SDL3-3.4.16')
CFLAGS = ['-target', TARGET, '-O2', '-g0', '-std=c11', '-DRT_GUEST_WINDOW', '-DXI_SPLIT', '-D_CRT_SECURE_NO_WARNINGS',
          '-fno-strict-aliasing', '-Wno-macro-redefined', '-I', 'runtime', '-I', 'runtime/portable', '-I', 'runtime/split', '-I', 'launcher/pol',
          '-I', 'third_party/stb']
# tools/build.py HOST_LIBS, by MinGW's names: WaitOnAddress is in the synch API set, not synchronization.lib
LIBS = ['-lws2_32', '-ladvapi32', '-lbcrypt', '-lsecur32', '-ld3d12', '-ldxgi', '-ld3dcompiler_47', '-ldxguid',
        '-lapi-ms-win-core-synch-l1-2-0', '-lole32', '-luser32', '-lgdi32', '-lwinmm']


def run(cmd, **kw):
    line = ' '.join(shlex.quote(str(c)) for c in cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...', flush=True)
    subprocess.check_call([str(c) for c in cmd], **kw)


def sdl(work):
    """SDL3's MinGW release unpacked in work (downloaded once, checked against its pinned SHA-256)."""
    url, sha, top = SDL
    tree = os.path.join(work, top, 'x86_64-w64-mingw32')
    if os.path.isdir(tree):
        return tree
    os.makedirs(work, exist_ok=True)
    archive = os.path.join(work, os.path.basename(url))
    if not os.path.exists(archive):
        print('> ' + url, flush=True)
        urllib.request.urlretrieve(url, archive + '.part')
        os.rename(archive + '.part', archive)
    with open(archive, 'rb') as f:
        got = hashlib.sha256(f.read()).hexdigest()
    if got != sha:
        os.remove(archive)
        raise SystemExit('%s: SHA-256 %s, expected %s' % (archive, got, sha))
    with tarfile.open(archive) as t:
        try:
            t.extractall(work, filter='data')
        except TypeError:  # Python without extraction filters
            t.extractall(work)
    return tree


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zig', default=shutil.which('zig') or 'zig')
    ap.add_argument('--work', default=os.path.join(ROOT, 'build', 'windows-dist'))
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    tree = sdl(work)
    zig = [a.zig, 'cc']
    objdir = os.path.join(work, 'obj')
    os.makedirs(objdir, exist_ok=True)
    srcs = [p.replace('\\', '/') for p in build.PORTABLE + build.HOST_SOURCES] + ['runtime/portable/xi_load.c']
    flags = CFLAGS + ['-I', os.path.join(tree, 'include')]
    jobs, objs = [], []
    for s in srcs:
        o = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(o)
        jobs.append(zig + ['-c'] + flags + [s, '-o', o])
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        failed = [j[-3] for j, rc in zip(jobs, ex.map(lambda c: subprocess.call(c, cwd=ROOT), jobs)) if rc]
    if failed:
        raise SystemExit('failed: ' + ', '.join(failed))
    out = os.path.join(ROOT, 'build', 'windows-x86_64')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'xi-host.exe')
    run(zig + ['-target', TARGET, '-o', exe] + objs + [os.path.join(tree, 'lib', 'libSDL3.dll.a')] + LIBS, cwd=ROOT)
    shutil.copy(os.path.join(tree, 'bin', 'SDL3.dll'), out)
    for leftover in ('xi-host.pdb', 'xi-host.lib'):
        p = os.path.join(out, leftover)
        if os.path.exists(p):
            os.remove(p)
    print('built %s and SDL3.dll beside it' % exe)


if __name__ == '__main__':
    main()
