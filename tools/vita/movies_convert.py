#!/usr/bin/env python3
"""Converts the game's EA .vp6 movies to MP4 for movies=vp6preferred: H.264 (no B-frames) and AAC sound.

ffmpeg's EA demuxer stops at the first SCEl (audio end) chunk and drops the video after it, so those chunks are
removed first. Usage: movies_convert.py <MOVIES folder> [output folder]
Without an output folder the .mp4 files go beside the .vp6 files, where the port looks for them.
"""
import os
import struct
import subprocess
import sys
import tempfile


def strip_audio_end(data):
    out = bytearray()
    pos = 0
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        size = struct.unpack_from('<I', data, pos + 4)[0]
        if size < 8:
            break
        if tag != b'SCEl':
            out += data[pos:pos + size]
        pos += size
    return bytes(out)


def frame_count(path):
    text = subprocess.run(['ffprobe', '-v', 'error', '-count_frames', '-select_streams', 'v', '-show_entries',
                           'stream=nb_read_frames', '-of', 'csv=p=0', path], capture_output=True, text=True).stdout
    return int(text.strip().split()[0])


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    source, target = sys.argv[1], sys.argv[-1]
    os.makedirs(target, exist_ok=True)
    for name in sorted(os.listdir(source)):
        if not name.lower().endswith('.vp6'):
            continue
        with open(os.path.join(source, name), 'rb') as f:
            data = strip_audio_end(f.read())
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, name)
            with open(path, 'wb') as f:
                f.write(data)
            out = os.path.join(target, name[:-4] + '.mp4')
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', path, '-c:v', 'libx264', '-profile:v', 'main',
                            '-level', '3.1', '-pix_fmt', 'yuv420p', '-bf', '0', '-crf', '18', '-fps_mode', 'passthrough',
                            '-c:a', 'aac', '-b:a', '128k', '-ar', '48000', '-movflags', '+faststart', out], check=True)
            frames = [frame_count(p) for p in (path, out)]
        print(name, '->', out, 'frames', frames[0], frames[1], '' if frames[0] == frames[1] else 'MISMATCH')


if __name__ == '__main__':
    main()
