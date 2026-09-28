"""Portable Linux builds of the game host (xi-host) for the launcher to ship.

  python3 tools/linux_dist.py [--arch x86_64] [--arch aarch64] [--zig <zig>] [--work <dir>]
      build/linux-<arch>/xi-host

One binary per architecture that runs on any Linux with glibc 2.28 or later (2018 on: Debian 10,
Ubuntu 20.04, Fedora 29, SteamOS 3), whatever else it has installed: zig compiles everything
against glibc 2.28, and SDL3 and mbedtls are built from their released sources and linked in
statically. SDL loads X11, Wayland, PulseAudio, PipeWire and ALSA itself at run time, so none is
needed to start, only whichever the desktop has.

Runs on Linux, either architecture (it cross-compiles the other): SDL's back ends are built against
the build machine's headers, which it needs installed (the -dev packages of X11, Wayland, xkbcommon,
libdecor, PulseAudio, PipeWire, ALSA, dbus, udev, drm, gbm, EGL, GL), plus cmake and ninja. The
headers go after zig's own (-idirafter), so glibc's always come from zig.
"""
import argparse
import hashlib
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build_posix  # noqa: E402  (the source lists and flags)

GLIBC = '2.28'
SOURCES = {
    'SDL3': ('https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz',
             '7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68', 'SDL3-3.4.16'),
    'mbedtls': ('https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.5/mbedtls-3.6.5.tar.bz2',
                '4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8', 'mbedtls-3.6.5'),
}


def run(cmd, **kw):
    line = ' '.join(shlex.quote(str(c)) for c in cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...', flush=True)
    subprocess.check_call([str(c) for c in cmd], **kw)


def source(work, name):
    """The release unpacked in work/src (downloaded once, checked against its pinned SHA-256)."""
    url, sha, top = SOURCES[name]
    src = os.path.join(work, 'src')
    os.makedirs(src, exist_ok=True)
    tree = os.path.join(src, top)
    if os.path.isdir(tree):
        return tree
    archive = os.path.join(src, os.path.basename(url))
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
            t.extractall(src, filter='data')
        except TypeError:  # Python without extraction filters (before 3.10.12)
            t.extractall(src)
    return tree


def toolchain(work, zig, arch):
    """zig as cc, ar and ranlib for <arch>-linux-gnu.<GLIBC>, as scripts cmake can be given."""
    d = os.path.join(work, arch, 'bin')
    os.makedirs(d, exist_ok=True)
    target = '%s-linux-gnu.%s' % (arch, GLIBC)
    tools = {'cc': 'cc -target %s' % target, 'ar': 'ar', 'ranlib': 'ranlib'}
    for name, sub in tools.items():
        p = os.path.join(d, name)
        with open(p, 'w') as f:
            f.write('#!/bin/sh\nexec %s %s "$@"\n' % (shlex.quote(zig), sub))
        os.chmod(p, 0o755)
    return {k: os.path.join(d, k) for k in tools}


# the build machine's headers for SDL's back ends, after zig's (never its glibc)
SYS_HEADERS = ['-idirafter', '/usr/include']
# ... and its libraries, which cmake's finders only read the names of (SDL dlopens each back end's
# library by its soname, the same on every architecture): with zig as the compiler cmake does not
# know the multiarch folder they are in
_MULTIARCH = '/usr/lib/%s-linux-gnu' % platform.machine()  # (platform: imported on Windows too, windows_dist.py)
LIB_HINTS = ['-DCMAKE_LIBRARY_PATH=' + _MULTIARCH] if os.path.isdir(_MULTIARCH) else []


def cmake(work, arch, tc, name, srcdir, options):
    build = os.path.join(work, arch, name + '-build')
    prefix = os.path.join(work, arch, 'prefix')
    flags = ' '.join(['-O2', '-fPIC'] + SYS_HEADERS)
    run(['cmake', '-G', 'Ninja', '-S', srcdir, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_SYSTEM_NAME=Linux', '-DCMAKE_SYSTEM_PROCESSOR=' + arch,
         '-DCMAKE_C_COMPILER=' + tc['cc'], '-DCMAKE_AR=' + tc['ar'], '-DCMAKE_RANLIB=' + tc['ranlib'],
         '-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_INSTALL_PREFIX=' + prefix, '-DCMAKE_INSTALL_LIBDIR=lib',
         '-DCMAKE_POSITION_INDEPENDENT_CODE=ON'] + LIB_HINTS + options)
    run(['cmake', '--build', build])
    run(['cmake', '--install', build])
    return prefix


def sdl(work, arch, tc):
    # SDL_DEPS_SHARED (the default): each back end's library is dlopen'd, not linked
    return cmake(work, arch, tc, 'SDL3', source(work, 'SDL3'), [
        '-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF',
        '-DSDL_DEPS_SHARED=ON', '-DSDL_X11=ON', '-DSDL_WAYLAND=ON', '-DSDL_PULSEAUDIO=ON', '-DSDL_PIPEWIRE=ON',
        '-DSDL_ALSA=ON', '-DSDL_JACK=OFF', '-DSDL_SNDIO=OFF', '-DSDL_IBUS=OFF', '-DSDL_HIDAPI_LIBUSB=OFF',
        '-DSDL_VULKAN=ON', '-DSDL_OPENGL=ON', '-DSDL_OPENGLES=ON', '-DSDL_KMSDRM=OFF', '-DSDL_RPATH=OFF'])


def mbedtls(work, arch, tc):
    return cmake(work, arch, tc, 'mbedtls', source(work, 'mbedtls'), [
        '-DENABLE_TESTING=OFF', '-DENABLE_PROGRAMS=OFF', '-DUSE_SHARED_MBEDTLS_LIBRARY=OFF',
        '-DUSE_STATIC_MBEDTLS_LIBRARY=ON', '-DMBEDTLS_FATAL_WARNINGS=OFF'])


def xihost(work, arch, tc, prefix):
    """build/linux-<arch>/xi-host: host64 without the translation (XI_SPLIT), everything static
    but glibc."""
    out = os.path.join(ROOT, 'build', 'linux-' + arch)
    objdir = os.path.join(work, arch, 'obj')
    os.makedirs(out, exist_ok=True)
    os.makedirs(objdir, exist_ok=True)
    i = build_posix.CFLAGS.index('generated')
    flags = (build_posix.CFLAGS[:i - 1] + build_posix.CFLAGS[i + 1:] +
             ['-DXI_SPLIT', '-I', 'runtime/split', '-I', os.path.join(prefix, 'include'), '-g0'])
    if '-D_GNU_SOURCE' not in flags:
        flags.append('-D_GNU_SOURCE')
    srcs = build_posix.PORTABLE + build_posix.HOST_SOURCES + ['runtime/portable/xi_load.c']
    objs, jobs = [], []
    for s in srcs:
        o = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(o)
        jobs.append([tc['cc'], '-c'] + flags + [s, '-o', o])
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        failed = [j[-3] for j, rc in zip(jobs, ex.map(lambda c: subprocess.call(c, cwd=ROOT), jobs)) if rc]
    if failed:
        raise SystemExit('failed: ' + ', '.join(failed))
    lib = os.path.join(prefix, 'lib')
    exe = os.path.join(out, 'xi-host')
    run([tc['cc'], '-o', exe] + objs + [os.path.join(lib, n) for n in
         ('libSDL3.a', 'libmbedtls.a', 'libmbedx509.a', 'libmbedcrypto.a')] +
        ['-lm', '-lpthread', '-ldl', '-lrt', '-s'], cwd=ROOT)
    print('built %s (glibc %s and later, SDL3 and mbedtls inside)' % (exe, GLIBC))
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arch', action='append', choices=['x86_64', 'aarch64'])
    ap.add_argument('--zig', default=shutil.which('zig') or 'zig')
    ap.add_argument('--work', default=os.path.join(ROOT, 'build', 'linux-dist'))
    a = ap.parse_args()
    if not sys.platform.startswith('linux'):
        raise SystemExit('run this on Linux (it builds SDL against the machine\'s X11/Wayland/audio headers)')
    work = os.path.abspath(a.work)
    for arch in a.arch or ['x86_64', 'aarch64']:
        tc = toolchain(work, os.path.abspath(a.zig) if os.path.exists(a.zig) else a.zig, arch)
        sdl(work, arch, tc)
        prefix = mbedtls(work, arch, tc)
        xihost(work, arch, tc, prefix)


if __name__ == '__main__':
    main()
