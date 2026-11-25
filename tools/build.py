#!/usr/bin/env python3
"""Builds NFSU2VITA.vpk from your game folder in one step, on Windows, macOS or Linux.

python3 tools/build.py "<game folder>" [--download-tools] [--vitasdk DIR] [--llvm DIR] [--clean] [--regenerate] [--no-game-art]
Finds the toolchain or offers to download it, translates the game code when it changed, then builds.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import deps  # noqa: E402

EXE = 'SPEED2.EXE'
REMEMBERED = os.path.join(REPO, 'build-game-dir.txt')
GENERATED = os.path.join(REPO, 'src', 'ug2', 'generated')
FX_TABLES = os.path.join(REPO, 'include', 'winapi', 'd3d9_fxtables.h')
GAME_ART = os.path.join(REPO, 'sce_sys', 'game', 'icon0.png')
CODEGEN_OPTIONS = ['--float-align', 'all', '--inline-ftol2', '--pure-natives', '--native-writes', '--promote-stack-slots',
                   '--fold-rdata']


def step(text):
    print('\n== ' + text, flush=True)


def run(command, **options):
    """Runs a command; returns its exit code."""
    if os.name == 'nt' and os.environ.get('NFSU_PYTHON'):
        options['creationflags'] = subprocess.CREATE_NO_WINDOW
    return subprocess.call(command, **options)


# Game DLLs translated with the executable when they are next to it (tools/recompiler/games/srv.py).
DLLS = ('server.dll',)


def find_exe(folder):
    """The executable's path in a folder, whatever its letter case."""
    if folder and os.path.isdir(folder):
        for name in os.listdir(folder):
            if name.upper() == EXE:
                return os.path.join(folder, name)
    return None


def game_folder(given):
    """The game folder: argument, NFSU_GAME_DIR, or the last one used."""
    candidates = [given, os.environ.get('NFSU_GAME_DIR')]
    if os.path.isfile(REMEMBERED):
        with open(REMEMBERED, encoding='utf-8-sig') as f:
            candidates.append(f.read().strip())
    for folder in candidates:
        if not folder:
            continue
        if find_exe(folder):
            return os.path.abspath(folder)
        if folder == given:
            sys.exit('%s not found in %s. Give the game folder, the one with %s.' % (EXE, given, EXE))
    return None


def generator_stamp(folder):
    """Fingerprint of everything the translated code depends on."""
    files = [find_exe(folder)]
    files += [os.path.join(folder, n) for n in os.listdir(folder) if n.lower() in DLLS]
    for root, _, names in os.walk(folder):
        if os.path.basename(root).upper() == 'MOVIES':
            continue
        files += [os.path.join(root, n) for n in names if n.lower().endswith('.abk')]
    for root, _, names in os.walk(os.path.join(HERE, 'recompiler')):
        if '__pycache__' not in root:
            files += [os.path.join(root, n) for n in names if n.endswith(('.py', '.txt'))]
    digest = hashlib.sha256(' '.join(CODEGEN_OPTIONS).encode())
    for path in sorted(files):
        with open(path, 'rb') as f:
            digest.update(hashlib.sha256(f.read()).digest())
    return digest.hexdigest()


def python():
    """Command that runs Python: this interpreter, or what NFSU_PYTHON names."""
    named = os.environ.get('NFSU_PYTHON')
    return json.loads(named) if named else [sys.executable]


def translate(folder, force):
    """Game code and effect tables, only when their inputs changed."""
    stamp_file = os.path.join(GENERATED, '.stamp')
    stamp = generator_stamp(folder)
    current = open(stamp_file).read().strip() if os.path.isfile(stamp_file) else ''
    changed = force or stamp != current or not os.path.isfile(os.path.join(GENERATED, 'ug2.cpp')) \
        or not os.path.isfile(os.path.join(GENERATED, 'srv.cpp'))
    if changed:
        step('Translating the game code (about 5 minutes)')
        os.makedirs(GENERATED, exist_ok=True)
        command = python() + [os.path.join(HERE, 'recompiler', 'generate.py'), '--game', 'ug2',
                   '--binaries', folder, '--out', GENERATED] + CODEGEN_OPTIONS
        if run(command) != 0:
            sys.exit('Translating the game code failed.')
        # The game DLLs (games/srv.py: server.dll, for hosting LAN games), when the folder has them.
        command = python() + [os.path.join(HERE, 'recompiler', 'generate.py'), '--game', 'srv', '--missing-ok',
                   '--binaries', folder, '--out', GENERATED] + CODEGEN_OPTIONS
        if run(command) != 0:
            sys.exit('Translating server.dll failed.')
        with open(stamp_file, 'w') as f:
            f.write(stamp + '\n')
    if changed or not os.path.isfile(FX_TABLES):
        step('Making the effect tables')
        command = python() + ['-I', os.path.join(HERE, 'fx', 'fxtable.py'), '--exe', find_exe(folder),
                   '--out', FX_TABLES]
        if run(command) != 0:
            sys.exit('Making the effect tables failed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', nargs='?', help='the game folder, the one with SPEED2.EXE (remembered afterwards)')
    parser.add_argument('--clean', action='store_true', help='delete the build folder first')
    parser.add_argument('--regenerate', action='store_true', help='translate the game code again')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4, help='parallel compile jobs')
    parser.add_argument('--build-dir', default=os.path.join(REPO, 'build-vita'), help='build folder')
    parser.add_argument('--vitasdk', help='VitaSDK folder to use, if not in a usual place')
    parser.add_argument('--llvm', help='LLVM folder to use (clang 22.1.8 or newer)')
    parser.add_argument('--no-game-art', action='store_true', help='use the plain default icon and LiveArea images')
    parser.add_argument('--download-tools', action='store_true', help='download missing tools without asking')
    args = parser.parse_args()

    folder = game_folder(args.game)
    have_code = os.path.isfile(os.path.join(GENERATED, 'ug2.cpp')) and os.path.isfile(FX_TABLES)
    if not folder and not have_code:
        sys.exit('Give the game folder: python3 tools/build.py "<folder with %s>"' % EXE)

    step('Toolchain')
    vitasdk = deps.vitasdk(args.vitasdk, args.download_tools)
    llvm = deps.llvm(args.llvm, args.download_tools)
    cmake = deps.build_tool('cmake', args.download_tools)
    ninja = deps.build_tool('ninja', args.download_tools)
    try:
        packages = deps.packages()
    except Exception as e:
        print('The launcher\'s libraries could not be installed (%s): building without mod downloads.' % e)
        packages = ''
    clang = '.'.join(str(n) for n in deps.clang_version(llvm))
    print('VitaSDK: %s\nLLVM:    %s (clang %s)\nCMake:   %s\nNinja:   %s\nLibraries: %s' % (vitasdk, llvm, clang, cmake,
                                                                                    ninja, packages or '(none)'))

    if folder:
        with open(REMEMBERED, 'w', encoding='utf-8') as f:
            f.write(folder + '\n')
        translate(folder, args.regenerate)
        if not args.no_game_art and not os.path.isfile(GAME_ART):
            step('Icon and LiveArea images from the game files')
            if run(python() + ['-I', os.path.join(HERE, 'vita', 'livearea_art.py'), folder]) != 0:
                print('Using the plain default images.')
    else:
        print('Game folder not known: building the code translated earlier.')

    # The script rebuilds only when the installed copy lacks the current patches.
    step('FFmpeg for native VP6 movies (optional)')
    if run(python() + ['-I', os.path.join(HERE, 'vita', 'build_ffmpeg_vp6.py'), '--vitasdk', vitasdk,
                       '--jobs', str(args.jobs)], env=dict(os.environ)) != 0:
        print('FFmpeg did not build: the port is built without native VP6 movies (movies=vp6preferred or vp6only).')
    if packages:
        step('libarchive for the launcher\'s mod downloads (optional)')
        if run(python() + ['-I', os.path.join(HERE, 'vita', 'build_libarchive.py'), '--vitasdk', vitasdk, '--packages',
                           packages, '--cmake', cmake, '--jobs', str(args.jobs)], env=dict(os.environ)) != 0:
            print('libarchive did not build: the launcher is built without mod downloads.')

    use_art = os.path.isfile(GAME_ART) and not args.no_game_art
    build = os.path.abspath(args.build_dir)
    if args.clean and os.path.isdir(build):
        shutil.rmtree(build)
    env = dict(os.environ, VITASDK=vitasdk, NFSU_LLVM=llvm)
    env['PATH'] = os.pathsep.join([os.path.join(vitasdk, 'bin'), os.path.dirname(ninja), env.get('PATH', '')])
    configure = [cmake, '-S', REPO, '-B', build, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja,
                 '-DCMAKE_TOOLCHAIN_FILE=' + os.path.join(REPO, 'cmake', 'vita-clang.cmake'),
                 '-DCMAKE_BUILD_TYPE=Release', '-DNFSU_GAME_OPT=-O2',
                 '-DVITASDK=' + vitasdk.replace('\\', '/'), '-DNFSU_LLVM=' + llvm.replace('\\', '/'),
                 '-DNFSU_VITA_PACKAGES=' + packages.replace('\\', '/')]
    configure.append('-DNFSU_PLAIN_ART=' + ('OFF' if use_art else 'ON'))
    options = '\n'.join(configure + ['game art: %s' % use_art]) + '\n'
    options_file = os.path.join(build, 'build-options.txt')
    current = open(options_file, encoding='utf-8').read() if os.path.isfile(options_file) else ''
    if options != current or not os.path.isfile(os.path.join(build, 'build.ninja')):
        if current and [l for l in current.split('\n') if 'VITASDK=' in l or 'NFSU_LLVM=' in l] != \
                [l for l in options.split('\n') if 'VITASDK=' in l or 'NFSU_LLVM=' in l]:
            print('The toolchain changed: starting the build folder again.')
            shutil.rmtree(build)
        step('Configuring')
        if run(configure, env=env) != 0:
            sys.exit('Configuring failed.')
        with open(options_file, 'w', encoding='utf-8') as f:
            f.write(options)
    step('Building (the first build takes a while)')
    if run([cmake, '--build', build, '--parallel', str(args.jobs)], env=env) != 0:
        sys.exit('Building failed.')
    print('\nDone: ' + os.path.join(build, 'NFSU2VITA.vpk'))


if __name__ == '__main__':
    main()
