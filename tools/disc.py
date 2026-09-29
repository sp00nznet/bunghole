#!/usr/bin/env python3
r"""Put your Bunghole in One CD's files in game/disc/, from wherever they are.

    py -3 tools/disc.py E:\                 # the CD in a drive (or any copy of its files)
    py -3 tools/disc.py BUILD_12.cue        # a BIN/CUE dump (raw 2352-byte sectors)
    py -3 tools/disc.py disc.iso            # a 2048-byte ISO
    py -3 tools/disc.py                     # look in every CD drive

The files are read with pcrecomp's ISO9660 walker (tools/assets/iso_peek.py),
so nothing is mounted and no extractor is needed. A raw BIN is read through the
2048 user bytes of each 2352-byte Mode 1 sector.

It supplies your own copy to the local build; nothing is downloaded, and none
of it is ever committed (README, "What is not in this repo").
"""
import os
import shutil
import string
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'game', 'disc')
NEEDED = ('GOLF.EXE', '00170001.DLL')
sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'tools')),
                                'tools', 'assets'))
import iso_peek                                              # noqa: E402


class RawReader(iso_peek.Reader):
    """ISO offsets over a raw BIN: sector n's 2048 bytes sit 16 bytes into
    raw sector n (12 sync + 4 header), 2352 bytes apart."""

    def read(self, off, size):
        out = bytearray()
        while size > 0:
            sec, within = divmod(off, 2048)
            take = min(size, 2048 - within)
            self.fh.seek(sec * 2352 + 16 + within)
            out += self.fh.read(take)
            off += take
            size -= take
        return bytes(out)


def is_disc(d):
    return all(os.path.isfile(os.path.join(d, n)) for n in NEEDED)


def from_image(path):
    if path.lower().endswith('.cue'):
        # The data track is the first FILE line; this disc has only that track.
        for line in open(path, errors='replace'):
            if line.strip().upper().startswith('FILE'):
                path = os.path.join(os.path.dirname(path), line.split('"')[1])
                break
    raw = os.path.getsize(path) % 2352 == 0 and os.path.getsize(path) % 2048 != 0
    reader = (RawReader if raw or path.lower().endswith(('.bin', '.img')) else iso_peek.Reader)(path)
    files = iso_peek.walk(reader)
    names = {p.upper() for p, _, _ in files}
    if not all(n in names for n in NEEDED):
        sys.exit('%s is a disc image, but not the Bunghole in One CD (no GOLF.EXE)' % path)
    total = sum(s for _, _, s in files)
    print('reading %d files, %.0f MB, from %s' % (len(files), total / 1e6, path))
    for p, off, size in files:
        dst = os.path.join(OUT, *p.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            left = size
            while left:
                n = min(left, 1 << 20)
                f.write(reader.read(off + size - left, n))
                left -= n


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else ''
    if not src:
        drives = ['%s:\\' % c for c in string.ascii_uppercase if os.path.exists('%s:\\' % c)]
        found = [d for d in drives if is_disc(d)]
        if not found:
            sys.exit('no drive holds the Bunghole in One CD; pass a drive, folder or disc image')
        src = found[0]
    if os.path.isdir(src):
        if not is_disc(src):
            sys.exit('%s does not hold GOLF.EXE and 00170001.DLL' % src)
        print('copying %s' % src)
        shutil.copytree(src, OUT, dirs_exist_ok=True)
    elif os.path.isfile(src):
        from_image(src)
    else:
        sys.exit('no such drive, folder or file: %s' % src)
    if not is_disc(OUT):
        sys.exit('copied, but %s is still missing GOLF.EXE' % OUT)
    print('done: %s' % OUT)


if __name__ == '__main__':
    main()
