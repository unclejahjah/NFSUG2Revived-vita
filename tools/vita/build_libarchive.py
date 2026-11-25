#!/usr/bin/env python3
"""Builds the pinned libarchive's zip, 7z and rar readers, with zlib, bzip2 and liblzma, into
build-libarchive/install for the launcher. VitaSDK's own package lacks liblzma.

Usage: build_libarchive.py --packages <tools/deps.py packages> [--vitasdk D] [--cmake C] [--jobs N] [--source tar.xz]
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

VERSION = '3.8.9'
URL = 'https://github.com/libarchive/libarchive/releases/download/v%s/libarchive-%s.tar.xz' % (VERSION, VERSION)
SHA256 = '888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde'
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

OPTIONS = [
    '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=OFF', '-DENABLE_TAR=OFF', '-DENABLE_CPIO=OFF', '-DENABLE_CAT=OFF',
    '-DENABLE_UNZIP=OFF', '-DENABLE_TEST=OFF', '-DENABLE_WERROR=OFF', '-DENABLE_OPENSSL=OFF', '-DENABLE_MBEDTLS=OFF',
    '-DENABLE_NETTLE=OFF', '-DENABLE_LIBB2=OFF', '-DENABLE_LZ4=OFF', '-DENABLE_LZO=OFF', '-DENABLE_ZSTD=OFF',
    '-DENABLE_LIBXML2=OFF', '-DENABLE_EXPAT=OFF', '-DENABLE_PCREPOSIX=OFF', '-DENABLE_PCRE2POSIX=OFF', '-DENABLE_ACL=OFF',
    '-DENABLE_XATTR=OFF', '-DENABLE_ICONV=OFF', '-DENABLE_CNG=OFF', '-DENABLE_ZLIB=ON', '-DENABLE_BZip2=ON',
    '-DENABLE_LZMA=ON',
]
# newlib calls the timezone variable _timezone. The public headers have no enums, so the enum size may differ from
# the launcher's.
CFLAGS = ('-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums -Dtimezone=_timezone -ffunction-sections '
          '-fdata-sections')


def run(cmd, cwd=None, env=None):
    print('+', ' '.join(cmd))
    if subprocess.run(cmd, cwd=cwd, env=env).returncode != 0:
        sys.exit('Failed: ' + ' '.join(cmd[:2]))


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vitasdk', default=os.environ.get('VITASDK', '/usr/local/vitasdk'))
    parser.add_argument('--packages', required=True, help='folder with arm-vita-eabi/ (tools/deps.py packages)')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--source', help='a local copy of the pinned release')
    parser.add_argument('--out', help='work and install folder (default: build-libarchive)')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    out = os.path.abspath(args.out or os.path.join(REPO, 'build-libarchive'))
    install = os.path.join(out, 'install')
    marker = os.path.join(install, 'nfsu-build.txt')
    sysroot = os.path.join(os.path.abspath(args.packages), 'arm-vita-eabi')
    wanted = hashlib.sha256(' '.join([VERSION, CFLAGS] + OPTIONS).encode()).hexdigest()
    if os.path.isfile(marker) and open(marker).read().strip() == wanted:
        print('libarchive %s in %s is up to date' % (VERSION, install))
        return
    os.makedirs(out, exist_ok=True)
    archive = os.path.join(out, 'libarchive-%s.tar.xz' % VERSION)
    if args.source and not os.path.isfile(archive):
        shutil.copyfile(args.source, archive)
    if not os.path.isfile(archive):
        print('Downloading', URL)
        urllib.request.urlretrieve(URL, archive)
    if file_hash(archive) != SHA256:
        os.remove(archive)
        sys.exit('The libarchive download does not match its pinned SHA-256; removed it.')
    source = os.path.join(out, 'libarchive-%s' % VERSION)
    build = os.path.join(out, 'obj')
    for folder in (source, build, install):
        shutil.rmtree(folder, ignore_errors=True)
    with tarfile.open(archive) as tar:
        if hasattr(tarfile, 'data_filter'):
            tar.extractall(out, filter='data')
        else:
            tar.extractall(out)
    env = dict(os.environ, VITASDK=args.vitasdk)
    env['PATH'] = os.path.join(args.vitasdk, 'bin') + os.pathsep + env['PATH']
    lib = os.path.join(sysroot, 'lib')
    include = os.path.join(sysroot, 'include')
    run([args.cmake, '-S', source, '-B', build, '-G', 'Unix Makefiles',
         '-DCMAKE_TOOLCHAIN_FILE=' + os.path.join(args.vitasdk, 'share', 'vita.toolchain.cmake'),
         '-DCMAKE_INSTALL_PREFIX=' + install, '-DCMAKE_C_FLAGS=%s -I%s' % (CFLAGS, include),
         '-DZLIB_INCLUDE_DIR=' + include, '-DZLIB_LIBRARY=' + os.path.join(lib, 'libz.a'),
         '-DBZIP2_INCLUDE_DIR=' + include, '-DBZIP2_LIBRARY_RELEASE=' + os.path.join(lib, 'libbz2.a'),
         '-DLIBLZMA_INCLUDE_DIR=' + include, '-DLIBLZMA_LIBRARY=' + os.path.join(lib, 'liblzma.a')] + OPTIONS, env=env)
    run([args.cmake, '--build', build, '--parallel', str(args.jobs), '--target', 'archive_static'], env=env)
    os.makedirs(os.path.join(install, 'lib'), exist_ok=True)
    os.makedirs(os.path.join(install, 'include'), exist_ok=True)
    shutil.copyfile(os.path.join(build, 'libarchive', 'libarchive.a'), os.path.join(install, 'lib', 'libarchive.a'))
    for header in ('archive.h', 'archive_entry.h'):
        shutil.copyfile(os.path.join(source, 'libarchive', header), os.path.join(install, 'include', header))
    with open(marker, 'w') as f:
        f.write(wanted + '\n')
    print('libarchive %s installed in %s' % (VERSION, install))


if __name__ == '__main__':
    main()
