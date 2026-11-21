#!/usr/bin/env python3
"""Builds a minimal static FFmpeg (libavcodec with only the VP6 decoders, libavutil) for the Vita, LGPL 2.1+ only.

Usage: build_ffmpeg_vp6.py [--vitasdk <folder>] [--jobs N] [--host] [--out <folder>]
The pinned release is downloaded and checked, the patches in tools/vita/ffmpeg-patches are applied, and the result
is installed into build-ffmpeg-vp6/install (include/ and lib/), which CMake links when it is there. Needs make,
patch and the VitaSDK's arm-vita-eabi-gcc. --host builds the same for this computer (build-ffmpeg-vp6-host), for the
host test ug2vp6test. Nothing is rebuilt when the installed copy already has the current patches.
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

VERSION = '7.1.2'
URL = 'https://ffmpeg.org/releases/ffmpeg-%s.tar.xz' % VERSION
SHA256 = '089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304'
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PATCHES = sorted(glob.glob(os.path.join(HERE, 'ffmpeg-patches', '*.patch')))

COMMON = [
    '--enable-static', '--disable-shared', '--disable-everything', '--enable-decoder=vp6', '--enable-decoder=vp6f',
    '--disable-programs', '--disable-doc', '--disable-avformat', '--disable-avdevice', '--disable-avfilter',
    '--disable-swscale', '--disable-swresample', '--disable-network', '--disable-autodetect', '--disable-pthreads',
    '--disable-debug', '--disable-iconv',
]
VITA = [
    '--enable-cross-compile', '--cross-prefix=arm-vita-eabi-', '--arch=arm', '--cpu=cortex-a9', '--target-os=none',
    '--enable-neon', '--enable-asm', '--enable-lto=no',
    '--extra-cflags=-O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums -ffunction-sections -fdata-sections',
]


def run(cmd, cwd, env):
    print('+', ' '.join(cmd))
    if subprocess.run(cmd, cwd=cwd, env=env).returncode != 0:
        sys.exit('Failed: ' + ' '.join(cmd[:2]))


def stamp(host):
    h = hashlib.sha256((VERSION + ' '.join(COMMON + ([] if host else VITA))).encode())
    for p in PATCHES:
        with open(p, 'rb') as f:
            h.update(f.read())
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vitasdk', default=os.environ.get('VITASDK', '/usr/local/vitasdk'))
    parser.add_argument('--host', action='store_true', help='build for this computer (the host test)')
    parser.add_argument('--out', help='work and install folder (default: build-ffmpeg-vp6, -host with --host)')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    out = os.path.abspath(args.out or os.path.join(REPO, 'build-ffmpeg-vp6' + ('-host' if args.host else '')))
    install = os.path.join(out, 'install')
    marker = os.path.join(install, 'nfsu-build.txt')
    wanted = stamp(args.host)
    if os.path.isfile(marker) and open(marker).read().strip() == wanted:
        print('FFmpeg %s in %s is up to date' % (VERSION, install))
        return
    os.makedirs(out, exist_ok=True)
    archive = os.path.join(out, 'ffmpeg-%s.tar.xz' % VERSION)
    if not os.path.isfile(archive):
        print('Downloading', URL)
        urllib.request.urlretrieve(URL, archive)
    with open(archive, 'rb') as f:
        if hashlib.sha256(f.read()).hexdigest() != SHA256:
            os.remove(archive)
            sys.exit('The FFmpeg download does not match its pinned SHA-256; removed it.')
    # A fresh copy of the release, then the port's patches (LGPL: they are kept in this repository).
    source = os.path.join(out, 'ffmpeg-%s' % VERSION)
    for folder in (source, os.path.join(out, 'obj'), install):
        shutil.rmtree(folder, ignore_errors=True)
    with tarfile.open(archive) as tar:
        tar.extractall(out)
    env = dict(os.environ)
    for p in PATCHES:
        run(['patch', '-p1', '-N', '-i', p], source, env)
    if not args.host:
        env['PATH'] = os.path.join(args.vitasdk, 'bin') + os.pathsep + env['PATH']
    build = os.path.join(out, 'obj')
    os.makedirs(build, exist_ok=True)
    run([os.path.join(source, 'configure'), '--prefix=' + install] + COMMON + ([] if args.host else VITA), build, env)
    run(['make', '-j%d' % args.jobs], build, env)
    run(['make', 'install'], build, env)
    with open(marker, 'w') as f:
        f.write(wanted + '\n')
    print('FFmpeg %s installed in %s' % (VERSION, install))


if __name__ == '__main__':
    main()
