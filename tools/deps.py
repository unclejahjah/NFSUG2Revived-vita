"""Finds VitaSDK and LLVM, or downloads pinned versions once into a shared folder (~/vita-tools).

python3 tools/deps.py vitasdk|llvm|packages [--download]   prints the folder used
VitaSDK and LLVM (22.1.8 or newer) are looked for in their usual places first. packages: the launcher's VitaSDK
libraries (curl with mbedTLS, zstd, zlib, liblzma, bzip2) in a folder of their own.
"""
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

VITASDK_URL = 'https://github.com/vitasdk/autobuilds/releases/download/sdk-snapshot-20261015.850.1/'
LLVM_URL = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/'
LLVM_VERSION = '22.1.8'
LLVM_MINIMUM = (22, 1, 8)  # the version the build is tested with; older ones are refused

# (system, machine): archive name, SHA-256
VITASDK = {
    ('windows', 'x86_64'): ('vitasdk-x86_64-w64-mingw32-2026-10-15_05-17-40.tar.bz2',
                            'bc7af73fa8c873ecc9f6ab89239e449bf94c2912231f62a6151ba2a13ff45326'),
    ('darwin', 'arm64'): ('vitasdk-arm64-apple-darwin-2026-10-15_05-00-31.tar.bz2',
                          '99f2e1cfb89f0995d80f50f55c927d2c6032c3938f2d79d7b18bba4c9e4ab4ce'),
    ('darwin', 'x86_64'): ('vitasdk-x86_64-apple-darwin-2026-10-15_05-17-51.tar.bz2',
                           'b801d0db100e8be48134ce1b1d1f3ab86ac27bbcb60ff23f533b8bde069df6d5'),
    ('linux', 'x86_64'): ('vitasdk-x86_64-linux-gnu-2026-10-15_05-01-16.tar.bz2',
                          '801f483928523fe63547faf58a7dfdaca7482b26c33af0064acea96a906f7662'),
    ('linux', 'arm64'): ('vitasdk-aarch64-linux-gnu-2026-10-15_05-00-35.tar.bz2',
                         'f312261397ba4400baa6a0b15a9670d1fcb7463b55b0cd54879fbd0126c0b17d'),
}
LLVM = {
    ('windows', 'x86_64'): ('clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz',
                            'd96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234'),
    ('darwin', 'arm64'): ('LLVM-22.1.8-macOS-ARM64.tar.xz',
                          'f260f4f7c0d430828a81ae8a3826a1d63fc0963ec2459489308cc23b1f7eab4f'),
    ('linux', 'x86_64'): ('LLVM-22.1.8-Linux-X64.tar.xz',
                          'df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384'),
    ('linux', 'arm64'): ('LLVM-22.1.8-Linux-ARM64.tar.xz',
                         '805efad2bb91cb4967fa569e0881d10c0f69c04461cf671cccbae19f547acc34'),
}

PACKAGES_URL = 'https://github.com/vitasdk/vitasdk-autobuild/releases/download/packages-2026.08-snapshot-20261024.61.1/'
PACKAGES_FOLDER = 'vita-launcher-packages-20261024'
# Archive name, SHA-256
PACKAGES = [
    ('curl-mbedtls-8.22.0-1-vita.pkg.tar.xz', '249b6992000b77dc2cac59feec55bea7bb892f181c03cb9d5399504290b6d30f'),
    ('mbedtls-3.6.5-1-vita.pkg.tar.xz', 'f0340ba0eae36b2e8358ec033245e85ad442395fcffe3cf19dcc1a9551cfcf07'),
    ('zstd-1.5.7-1-vita.pkg.tar.xz', '8952615363f62179c9a9168bf7bab24e55b39db7f420ee63e580e1b141dc35f9'),
    ('zlib-1.3.2-2-vita.pkg.tar.xz', 'e5b601fe333edb3781c0821c8ea168448d0edb0195362fb391d60e2c0e5d423c'),
    ('xz-5.8.4-1-vita.pkg.tar.xz', '94f4efe01f4f7f3c9557c9f538a9c5b2d2f59a00abed8549e4e4ed0455ff36c5'),
    ('bzip2-1.0.8-1-vita.pkg.tar.xz', '81072eb7487878d1cfede1122997644892a3f750a3e7da676d80d729f4e635d0'),
]

CMAKE_VERSION = '4.5.0'
CMAKE_URL = 'https://github.com/Kitware/CMake/releases/download/v4.5.0/'
CMAKE = {
    ('windows', 'x86_64'): ('cmake-4.5.0-windows-x86_64.zip',
                            '301b1728317ccd84e984b02e366a8293735bc5ac618757d3d8f58b578f062411'),
    ('darwin', 'arm64'): ('cmake-4.5.0-macos-universal.tar.gz',
                          'a645d69fd21e521759cc0c64e7c60462d43faf0c7214ac05f75c80f7c5037dde'),
    ('darwin', 'x86_64'): ('cmake-4.5.0-macos-universal.tar.gz',
                           'a645d69fd21e521759cc0c64e7c60462d43faf0c7214ac05f75c80f7c5037dde'),
    ('linux', 'x86_64'): ('cmake-4.5.0-linux-x86_64.tar.gz',
                          '81f0d5b0960fb6adc8197030ffc827fba937640d51e73e39e0e9e40d88c66f6f'),
    ('linux', 'arm64'): ('cmake-4.5.0-linux-aarch64.tar.gz',
                         '0c8447e7bba9701467b5ac6155d157aa9e309e71af4981f78bd4b9176107f553'),
}
NINJA_VERSION = '1.13.2'
NINJA_URL = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/'
NINJA = {
    ('windows', 'x86_64'): ('ninja-win.zip', '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'),
    ('darwin', 'arm64'): ('ninja-mac.zip', 'c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b'),
    ('darwin', 'x86_64'): ('ninja-mac.zip', 'c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b'),
    ('linux', 'x86_64'): ('ninja-linux.zip', '5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6'),
    ('linux', 'arm64'): ('ninja-linux-aarch64.zip',
                         'fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc'),
}

EXE = '.exe' if os.name == 'nt' else ''
LLVM_TOOLS = ('clang', 'clang++', 'llvm-ar', 'llvm-ranlib', 'llvm-profdata')
LLVM_KEEP = re.compile(r'^(bin/(clang(\+\+|-\d+)?|lld|ld\.lld|llvm-(ar|ranlib|profdata|nm|objcopy|readelf|readobj))'
                       r'(\.exe)?|lib/clang/.*|lib/lib(LLVM|clang-cpp)[^/]*\.(dylib|so)[^/]*)$')


def host():
    """(system, machine) in the spelling the tables use."""
    machine = platform.machine().lower()
    machine = {'amd64': 'x86_64', 'aarch64': 'arm64'}.get(machine, machine)
    return platform.system().lower(), machine


def tools_dir():
    """Shared folder, not tied to this project or one copy of it."""
    return os.environ.get('NFSU_TOOLS_DIR') or os.path.join(os.path.expanduser('~'), 'vita-tools')


def note(text):
    print(text, file=sys.stderr, flush=True)


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def download(url, sha256, path):
    """Fetches url to path unless a good copy is there."""
    if os.path.exists(path) and file_hash(path) == sha256:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + '.part'
    note('Downloading ' + url)
    digest = hashlib.sha256()
    with urllib.request.urlopen(url) as response, open(part, 'wb') as out:
        total = int(response.headers.get('Content-Length') or 0)
        done, shown = 0, -1
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            out.write(block)
            digest.update(block)
            done += len(block)
            if total and done * 10 // total != shown:
                shown = done * 10 // total
                note('  %d %% of %d MB' % (shown * 10, total >> 20))
    if digest.hexdigest() != sha256:
        os.remove(part)
        raise RuntimeError('SHA-256 mismatch for ' + url)
    os.replace(part, path)


def strip_top(name):
    parts = name.replace('\\', '/').split('/', 1)
    return parts[1] if len(parts) == 2 else ''


def unpack_vitasdk(archive, target):
    with tarfile.open(archive, 'r:bz2') as tar:
        members = []
        for member in tar.getmembers():
            member.name = strip_top(member.name)
            if member.name:
                members.append(member)
        if hasattr(tarfile, 'tar_filter'):
            tar.extractall(target, members=members, filter='tar')
        else:
            tar.extractall(target, members=members)


def link(source, path):
    """A symbolic link, or a copy where links need special rights."""
    if os.name == 'nt':
        shutil.copy2(os.path.normpath(os.path.join(os.path.dirname(path), source)), path)
    else:
        os.symlink(source, path)


def unpack_llvm(archive, target):
    """Only the tools the build runs, clang's headers and shared libraries."""
    links = {}
    with tarfile.open(archive, 'r|xz') as tar:
        for member in tar:
            name = strip_top(member.name)
            if not LLVM_KEEP.match(name):
                continue
            path = os.path.join(target, *name.split('/'))
            if member.issym():
                links[path] = member.linkname
            elif member.isfile():
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with tar.extractfile(member) as src, open(path, 'wb') as out:
                    shutil.copyfileobj(src, out)
                os.chmod(path, member.mode | 0o200)
    for _ in range(4):
        for path, source in list(links.items()):
            if os.path.exists(os.path.normpath(os.path.join(os.path.dirname(path), source))):
                link(source, path)
                del links[path]
    binaries = os.path.join(target, 'bin')
    for wanted, fallback in (('clang++', 'clang'), ('ld.lld', 'lld')):
        if not os.path.exists(os.path.join(binaries, wanted + EXE)):
            link(fallback + EXE, os.path.join(binaries, wanted + EXE))


def install(name, table, base_url, folder, unpack, variable):
    """Downloads and unpacks one pinned package; returns its folder."""
    if host() not in table:
        sys.exit('No %s download is known for %s %s. Install it yourself and set %s to its folder.'
                 % (name, host()[0], host()[1], variable))
    archive_name, sha256 = table[host()]
    target = os.path.join(tools_dir(), folder)
    marker = os.path.join(target, '.installed')
    if os.path.exists(marker) and open(marker).read().strip() == sha256:
        return target
    archive = os.path.join(tools_dir(), 'downloads', archive_name)
    download(base_url + archive_name, sha256, archive)
    note('Unpacking %s (this can take several minutes)' % archive_name)
    if os.path.exists(target):
        shutil.rmtree(target)
    os.makedirs(target)
    unpack(archive, target)
    with open(marker, 'w') as f:
        f.write(sha256 + '\n')
    os.remove(archive)
    return target


def packages():
    """Downloads and unpacks the launcher's libraries once; returns their folder."""
    target = os.path.join(tools_dir(), PACKAGES_FOLDER)
    marker = os.path.join(target, '.installed')
    stamp = hashlib.sha256(' '.join(sha for _, sha in PACKAGES).encode()).hexdigest()
    if os.path.exists(marker) and open(marker).read().strip() == stamp:
        return target
    if os.path.exists(target):
        shutil.rmtree(target)
    os.makedirs(target)
    for name, sha256 in PACKAGES:
        archive = os.path.join(tools_dir(), 'downloads', name)
        download(PACKAGES_URL + name, sha256, archive)
        with tarfile.open(archive, 'r:xz') as tar:
            members = [m for m in tar.getmembers() if m.name.startswith('arm-vita-eabi/')]
            if hasattr(tarfile, 'tar_filter'):
                tar.extractall(target, members=members, filter='tar')
            else:
                tar.extractall(target, members=members)
        os.remove(archive)
    with open(marker, 'w') as f:
        f.write(stamp + '\n')
    return target


def remembered(name, folder=None):
    """Reads, or with a folder stores, a location the user chose."""
    path = os.path.join(tools_dir(), 'locations.json')
    try:
        with open(path, encoding='utf-8') as f:
            saved = json.load(f)
    except (OSError, ValueError):
        saved = {}
    if folder:
        saved[name] = folder
        os.makedirs(tools_dir(), exist_ok=True)
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(saved, f, indent=2)
    return saved.get(name)


def choose(name, target, megabytes, valid, hints, download, why='', otherwise='install it and add it to PATH'):
    """Asks whether to download or use a folder; returns a folder or None."""
    if download:
        return None
    if not sys.stdin or not sys.stdin.isatty():
        sys.exit('%s was not found. Run again with --download-tools to download it (%d MB), or %s.'
                 % (name, megabytes, otherwise))
    note('\n%s was not found in its usual places.%s' % (name, why))
    note('  1) Download it (%d MB) to %s' % (megabytes, target))
    note('  2) Use one already on this computer' + (' (found: %s)' % ', '.join(hints) if hints else ''))
    while True:
        note('Choice [1]: ')
        line = sys.stdin.readline()
        if not line:
            sys.exit('No answer given; nothing was downloaded.')
        answer = line.strip()
        if answer in ('', '1'):
            return None
        if answer == '2':
            note('Folder: ')
            folder = os.path.expanduser(sys.stdin.readline().strip().strip('"'))
            if valid(folder):
                return os.path.abspath(folder)
            note('%s is not a usable %s folder.' % (folder, name))


def is_vitasdk(folder):
    return bool(folder) and os.path.isfile(os.path.join(folder, 'share', 'vita.toolchain.cmake'))


def vitasdk_megabytes():
    return 116 if os.name == 'nt' else 105


def find_vitasdk(given=None):
    """VitaSDK folder in a usual place or named, else None; never asks or downloads."""
    if given:
        return os.path.abspath(given) if is_vitasdk(given) else None
    home = os.path.expanduser('~')
    usual = [os.environ.get('VITASDK'), remembered('vitasdk'), os.path.join(tools_dir(), 'vitasdk'),
             '/usr/local/vitasdk', os.path.join(home, 'vitasdk'), 'C:\\vitasdk' if os.name == 'nt' else None]
    for folder in usual:
        if is_vitasdk(folder):
            return os.path.abspath(folder)
    return None


def vitasdk(given=None, download=False):
    """VitaSDK: named, in a usual place, chosen, or downloaded."""
    if given and not is_vitasdk(given):
        sys.exit('%s is not a VitaSDK folder (no share/vita.toolchain.cmake).' % given)
    found = find_vitasdk(given)
    if found:
        return found
    target = os.path.join(tools_dir(), 'vitasdk')
    folder = choose('VitaSDK', target, vitasdk_megabytes(), is_vitasdk, [], download,
                    otherwise='run with --vitasdk <folder>')
    if folder:
        remembered('vitasdk', folder)
        return folder
    folder = install('VitaSDK', VITASDK, VITASDK_URL, 'vitasdk', unpack_vitasdk, 'VITASDK')
    if not is_vitasdk(folder):
        sys.exit('The VitaSDK in %s is incomplete. Delete that folder and run this again.' % folder)
    note('Other Vita projects can use it: set VITASDK to %s and add its bin folder to PATH.' % folder)
    return folder


def clang_version(folder):
    """(major, minor, patch) of the clang in folder/bin, or (0, 0, 0)."""
    none = (0, 0, 0)
    if not folder:
        return none
    binaries = os.path.join(folder, 'bin')
    if not all(os.path.isfile(os.path.join(binaries, tool + EXE)) for tool in LLVM_TOOLS):
        return none
    if not os.path.isfile(os.path.join(binaries, 'ld.lld' + EXE)) and not shutil.which('ld.lld'):
        return none
    try:
        out = subprocess.run([os.path.join(binaries, 'clang' + EXE), '--version'], capture_output=True, text=True,
                             timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return none
    found = re.search(r'clang version (\d+)\.(\d+)\.(\d+)', out)
    return tuple(int(n) for n in found.groups()) if found and 'Apple' not in out else none


def is_llvm(folder):
    return clang_version(folder) >= LLVM_MINIMUM


def installed_llvm():
    """LLVM folders on this computer that could build the project."""
    folders = ['/opt/homebrew/opt/llvm', '/usr/local/opt/llvm']
    on_path = shutil.which('clang')
    if on_path:
        folders.insert(0, os.path.dirname(os.path.dirname(os.path.realpath(on_path))))
    return [f for f in dict.fromkeys(folders) if is_llvm(f)]


def llvm_megabytes():
    return {'windows': 862, 'darwin': 1480}.get(host()[0], 1940)


def find_llvm(given=None):
    """LLVM folder that is named or already installed, else None; never asks or downloads."""
    given = given or os.environ.get('NFSU_LLVM')
    if given:
        return os.path.abspath(given) if is_llvm(given) else None
    for folder in [remembered('llvm'), os.path.join(tools_dir(), 'llvm-' + LLVM_VERSION)] + installed_llvm():
        if is_llvm(folder):
            return os.path.abspath(folder)
    return None


def llvm(given=None, download=False):
    """LLVM of the minimum version or newer: named, installed, chosen, or downloaded."""
    given = given or os.environ.get('NFSU_LLVM')
    if given and not is_llvm(given):
        sys.exit('%s is not a usable LLVM: it needs clang %s or newer, lld, llvm-ar and llvm-profdata in bin.'
                 % (given, LLVM_VERSION))
    found = find_llvm(given)
    if found:
        return found
    target = os.path.join(tools_dir(), 'llvm-' + LLVM_VERSION)
    why = ' The build needs clang %s or newer.' % LLVM_VERSION
    folder = choose('LLVM ' + LLVM_VERSION, target, llvm_megabytes(), is_llvm, [], download, why,
                    otherwise='run with --llvm <folder>')
    if folder:
        remembered('llvm', folder)
        return folder
    folder = install('LLVM', LLVM, LLVM_URL, 'llvm-' + LLVM_VERSION, unpack_llvm, 'NFSU_LLVM')
    if not is_llvm(folder):
        sys.exit('The LLVM in %s does not run. Delete that folder and run this again.' % folder)
    return folder


def unpack_plain(archive, target):
    """Unpacks a .zip or .tar.gz, dropping a single top folder."""
    if archive.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            names = z.namelist()
            top = names[0].split('/')[0] + '/' if all(n.startswith(names[0].split('/')[0] + '/') for n in names) else ''
            for item in z.infolist():
                name = item.filename[len(top):]
                if not name or name.endswith('/'):
                    continue
                path = os.path.join(target, *name.split('/'))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with z.open(item) as src, open(path, 'wb') as out:
                    shutil.copyfileobj(src, out)
                if os.name != 'nt':
                    os.chmod(path, 0o755)
        return
    with tarfile.open(archive, 'r:gz') as tar:
        members = []
        for member in tar.getmembers():
            member.name = strip_top(member.name)
            if member.name:
                members.append(member)
        if hasattr(tarfile, 'tar_filter'):
            tar.extractall(target, members=members, filter='tar')
        else:
            tar.extractall(target, members=members)


def program_in(folder, name):
    """Path of a program inside a downloaded or chosen folder, or None."""
    if not folder:
        return None
    for parts in ((name + EXE,), ('bin', name + EXE), ('CMake.app', 'Contents', 'bin', name)):
        path = os.path.join(folder, *parts)
        if os.path.isfile(path):
            return path
    return None


def build_tool_megabytes(name):
    if name == 'ninja':
        return 1
    return {'windows': 54, 'darwin': 89}.get(host()[0], 64)


def find_build_tool(name):
    """Path of cmake or ninja: on PATH, chosen or downloaded, else None; never asks."""
    version = CMAKE_VERSION if name == 'cmake' else NINJA_VERSION
    return (shutil.which(name) or program_in(remembered(name), name)
            or program_in(os.path.join(tools_dir(), '%s-%s' % (name, version)), name))


def build_tool(name, download=False):
    """Path of cmake or ninja, found, chosen or downloaded."""
    found = find_build_tool(name)
    if found:
        return found
    table, url, version = (CMAKE, CMAKE_URL, CMAKE_VERSION) if name == 'cmake' else (NINJA, NINJA_URL, NINJA_VERSION)
    title = 'CMake' if name == 'cmake' else 'Ninja'
    target = os.path.join(tools_dir(), '%s-%s' % (name, version))
    folder = choose(title, target, build_tool_megabytes(name), lambda f: bool(program_in(f, name)), [], download)
    if folder:
        remembered(name, folder)
        return program_in(folder, name)
    folder = install(title, table, url, '%s-%s' % (name, version), unpack_plain, 'PATH')
    if not program_in(folder, name):
        sys.exit('The %s in %s is incomplete. Delete that folder and run this again.' % (title, folder))
    return program_in(folder, name)


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('vitasdk', 'llvm', 'packages') or sys.argv[2:] not in ([], ['--download']):
        sys.exit(__doc__)
    if sys.argv[1] == 'packages':
        print(packages())
    else:
        print((vitasdk if sys.argv[1] == 'vitasdk' else llvm)(download=sys.argv[2:] == ['--download']))
