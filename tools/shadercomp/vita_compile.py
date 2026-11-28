"""Compiles the port's Cg programs on the Vita (NFSU2VITA's shader compile mode) and fetches the .gxp files.

vita_compile.py --ip IP [--list FILE] [--out DIR] [--compare DIR] [--option KEY=VALUE ...]

Without --list, the programs are the ones cmake/shaders.cmake builds. Uploads shaders/*.cg and the manifest to
ux0:data/NeedForSpeedUnderground2/shaders/, launches the app (title ID NFSU2REVD), waits for compile.log to end, downloads out/<name>.gxp into --out.
--compare DIR reports, per program, whether DIR/<name>.gxp is identical (apart from the source-name hash, header
bytes 16-19).
"""
import argparse
import ftplib
import io
import os
import socket
import subprocess
import sys
import time

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
TITLE = 'NFSU2REVD'  # the Vita title ID (CMakeLists.txt VITA_TITLEID)
REMOTE = '/ux0:/data/NeedForSpeedUnderground2/shaders'


def command(ip, text):
    with socket.create_connection((ip, 1338), timeout=10) as s:
        s.sendall(text.encode() + b'\n')
        s.settimeout(5)
        reply = b''
        try:
            while chunk := s.recv(4096):
                reply += chunk
        except socket.timeout:
            pass
    return reply.decode(errors='replace').strip()


def programs():
    r = subprocess.run(['cmake', '-DREPO=' + REPO, '-P', os.path.join(REPO, 'tools', 'shadercomp', 'list_programs.cmake')],
                       capture_output=True, text=True, cwd=os.environ.get('TMPDIR', '/tmp'), check=True)
    return [line.strip() for line in r.stderr.splitlines() if line.strip()]


def mkdirs(ftp, path):
    parts = path.strip('/').split('/')
    for i in range(1, len(parts) + 1):
        try:
            ftp.mkd('/' + '/'.join(parts[:i]))
        except ftplib.error_perm:
            pass


def read(ftp, path):
    buf = io.BytesIO()
    try:
        ftp.retrbinary('RETR ' + path, buf.write)
    except ftplib.error_perm:
        return None
    return buf.getvalue()


def compare(new, old):
    if new == old:
        return 'identical'
    if len(new) == len(old) and new[:16] == old[:16] and new[20:] == old[20:]:
        return 'identical apart from the source hash'
    return 'differs (%d vs %d bytes)' % (len(new), len(old))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--list', help='manifest file; default: the programs of cmake/shaders.cmake')
    p.add_argument('--src', default=os.path.join(REPO, 'shaders'))
    p.add_argument('--out', default=os.path.join(REPO, 'build-shaders'))
    p.add_argument('--compare', help='folder of reference .gxp files')
    p.add_argument('--option', action='append', default=[], help='KEY=VALUE compile option (O, fx, fastmath, ...)')
    p.add_argument('--ip', required=True, help="the Vita's address on your network")
    p.add_argument('--timeout', type=int, default=900)
    a = p.parse_args()
    lines = open(a.list).read().splitlines() if a.list else programs()
    jobs = [l.split()[0] for l in lines if l.split() and not l.startswith(('#', 'option'))]
    manifest = ''.join('option %s %s\n' % tuple(o.split('=', 1)) for o in a.option) + '\n'.join(lines) + '\n'
    os.makedirs(a.out, exist_ok=True)

    ftp = ftplib.FTP()
    ftp.connect(a.ip, 1337, timeout=60)
    ftp.login()
    mkdirs(ftp, REMOTE + '/src')
    mkdirs(ftp, REMOTE + '/out')
    for name in sorted(os.listdir(a.src)):
        if name.endswith(('.cg', '.h', '.cgh')):
            with open(os.path.join(a.src, name), 'rb') as f:
                ftp.storbinary('STOR %s/src/%s' % (REMOTE, name), f)
    for stale in ('compile.log', 'compile.done'):
        try:
            ftp.delete('%s/%s' % (REMOTE, stale))
        except ftplib.error_perm:
            pass
    ftp.storbinary('STOR %s/compile.txt' % REMOTE, io.BytesIO(manifest.encode()))
    print('uploaded %d programs' % len(jobs))

    # A launch fails silently while any other app is open.
    command(a.ip, 'quit all')
    time.sleep(3)
    print('launch:', command(a.ip, 'launch %s' % TITLE) or '(no reply)')
    start = time.time()
    log = b''
    while time.time() - start < a.timeout:
        time.sleep(5)
        log = read(ftp, REMOTE + '/compile.log') or b''
        if b'\ndone ' in log or log.startswith(b'done '):
            break
    text = log.decode(errors='replace')
    with open(os.path.join(a.out, 'compile.log'), 'w') as f:
        f.write(text)
    if 'done ' not in text:
        sys.exit('no "done" line in compile.log after %d s' % a.timeout)
    for line in text.splitlines():
        if line.startswith(('compiler:', 'version:', 'done', 'failed', 'error')) or ' error ' in line:
            print(line)
    fetched = 0
    for name in jobs:
        data = read(ftp, '%s/out/%s.gxp' % (REMOTE, name))
        if data is None:
            continue
        with open(os.path.join(a.out, name + '.gxp'), 'wb') as f:
            f.write(data)
        fetched += 1
        if a.compare:
            ref = os.path.join(a.compare, name + '.gxp')
            print('%-28s %s' % (name, compare(data, open(ref, 'rb').read()) if os.path.exists(ref) else 'no reference'))
    ftp.quit()
    print('fetched %d of %d into %s' % (fetched, len(jobs), a.out))


if __name__ == '__main__':
    main()
