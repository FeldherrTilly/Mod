# -*- coding: utf-8 -*-
"""Installs a play copy of the mod with the art packed into FPK archives (faster game start).

The development checkout (the folder above Tools) stays as it is; the play folder gets XML, Python, sounds, maps, the DLL
etc. as plain files and the art (dds, kf, kfm, nif, tga) packed into Assets/WTP<n>.fpk. Only changed files are copied; the
paks are rebuilt only when the art changed (or with --paks).

    python Tools/wtp_play.py                 sync everything, rebuild the paks if the art changed
    python Tools/wtp_play.py --dll           copy only the DLL
    python Tools/wtp_play.py --paks          force a rebuild of the paks
    python Tools/wtp_play.py --target DIR    play folder (default: ../WTP next to the checkout)

FPK format (Civ4, version 4, read from the original Assets0.fpk): int32 version 4, "FPK_", byte 1, int32 file count; per file
int32 name length, the name (lower case, backslashes, every byte + 1), padding to 4 bytes (first byte: the padding length,
then zeros), uint64 FILETIME, int32 size, int32 offset; then the file data uncompressed, each file aligned to 4 bytes.
"""
import argparse
import json
import os
import shutil
import struct
import sys

PAK_EXTENSIONS = ('.dds', '.kf', '.kfm', '.nif', '.tga')
PAK_MAX_SIZE = 256 * 1024 * 1024
PAK_PREFIX = 'WTP'
MANIFEST = 'wtp_paks.json'

# not copied into the play folder (relative to the checkout, lower case, '/')
SKIP_TOP = ('.git', '.claude', 'project files', 'tools', 'graphify-out', 'copy to colonization.exe')
SKIP_FILES = ('claude.md', '.gitignore', '.gitattributes', '.editorconfig')
SKIP_EXT = ('.bak', '.pdb', '.xcf')


def rel_key(rel):
	return rel.replace('\\', '/').lower()


def is_skipped(rel):
	key = rel_key(rel)
	top = key.split('/')[0]
	name = key.split('/')[-1]
	if top in SKIP_TOP or name in SKIP_FILES or name.startswith('profile output'):
		return True
	return any(key.endswith(ext) for ext in SKIP_EXT) or '.bak' in name


def is_packed(rel):
	key = rel_key(rel)
	return key.startswith('assets/art/') and key.endswith(PAK_EXTENSIONS)


def walk(root):
	for base, dirs, files in os.walk(root):
		rel_base = os.path.relpath(base, root)
		dirs[:] = [d for d in dirs if not is_skipped(os.path.normpath(os.path.join(rel_base, d)))]
		for name in files:
			rel = os.path.normpath(os.path.join(rel_base, name))
			if not is_skipped(rel):
				yield rel


def same_file(src, dst):
	if not os.path.exists(dst):
		return False
	a = os.stat(src)
	b = os.stat(dst)
	return a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime)


def filetime(path):
	# Windows FILETIME: 100 ns since 1601
	return int(os.stat(path).st_mtime * 10000000) + 116444736000000000


def build_paks(src_root, dst_assets, files):
	for name in os.listdir(dst_assets):
		if name.upper().startswith(PAK_PREFIX) and name.lower().endswith('.fpk'):
			os.remove(os.path.join(dst_assets, name))
	# packs of at most PAK_MAX_SIZE bytes of data
	packs = [[]]
	size = 0
	for rel in sorted(files, key=rel_key):
		fsize = os.stat(os.path.join(src_root, rel)).st_size
		if packs[-1] and size + fsize > PAK_MAX_SIZE:
			packs.append([])
			size = 0
		packs[-1].append(rel)
		size += fsize + 3
	for index, pack in enumerate(packs):
		names = []
		for rel in pack:
			# the name inside the pak: relative to Assets
			name = rel_key(rel)[len('assets/'):].replace('/', '\\')
			names.append(name.encode('latin-1'))
		header = bytearray()
		header += struct.pack('<I', 4) + b'FPK_' + b'\x01' + struct.pack('<I', len(pack))
		entry_fixups = []
		for rel, name in zip(pack, names):
			header += struct.pack('<I', len(name))
			header += bytes(bytearray((c + 1) & 0xFF for c in bytearray(name)))
			pad = (4 - len(name) % 4) % 4
			if pad:
				header += bytes(bytearray([pad] + [0] * (pad - 1)))
			header += struct.pack('<Q', filetime(os.path.join(src_root, rel)))
			entry_fixups.append(len(header))
			header += struct.pack('<II', 0, 0)
		offset = (len(header) + 3) & ~3
		path = os.path.join(dst_assets, '%s%d.fpk' % (PAK_PREFIX, index))
		with open(path, 'wb') as out:
			out.write(b'\x00' * offset)
			for i, rel in enumerate(pack):
				with open(os.path.join(src_root, rel), 'rb') as f:
					data = f.read()
				struct.pack_into('<II', header, entry_fixups[i], len(data), offset)
				out.write(data)
				offset += len(data)
				pad = (4 - offset % 4) % 4
				out.write(b'\x00' * pad)
				offset += pad
			out.seek(0)
			out.write(bytes(header))
		print('  %s: %d files, %d MB' % (os.path.basename(path), len(pack), offset // (1024 * 1024)))


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument('--target', default=None)
	parser.add_argument('--dll', action='store_true', help='copy only the DLL')
	parser.add_argument('--paks', action='store_true', help='rebuild the paks')
	args = parser.parse_args()

	src_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
	dst_root = os.path.abspath(args.target or os.path.join(os.path.dirname(src_root), 'WTP'))
	if os.path.normcase(dst_root) == os.path.normcase(src_root):
		sys.exit('the play folder must not be the checkout')
	dll = os.path.join('Assets', 'CvGameCoreDLL.dll')

	if args.dll:
		os.makedirs(os.path.join(dst_root, 'Assets'), exist_ok=True)
		shutil.copy2(os.path.join(src_root, dll), os.path.join(dst_root, dll))
		print('DLL copied to %s' % os.path.join(dst_root, dll))
		return

	print('play folder: %s' % dst_root)
	loose = []
	packed = []
	for rel in walk(src_root):
		(packed if is_packed(rel) else loose).append(rel)

	# 1. plain files: copy the changed ones, remove the ones gone from the checkout
	copied = 0
	wanted = set(rel_key(r) for r in loose)
	for rel in loose:
		src = os.path.join(src_root, rel)
		dst = os.path.join(dst_root, rel)
		if not same_file(src, dst):
			os.makedirs(os.path.dirname(dst), exist_ok=True)
			shutil.copy2(src, dst)
			copied += 1
	removed = 0
	# the ini the game writes for the play folder (named after it, e.g. WTP.ini) is not in the checkout but stays
	own_ini = rel_key(os.path.basename(dst_root) + '.ini')
	for rel in list(walk(dst_root)):
		key = rel_key(rel)
		if key == own_ini or key == 'assets/' + MANIFEST.lower() or (key.startswith('assets/' + PAK_PREFIX.lower()) and key.endswith('.fpk')):
			continue
		if key not in wanted:
			os.remove(os.path.join(dst_root, rel))
			removed += 1
	print('files: %d copied, %d removed, %d unchanged' % (copied, removed, len(loose) - copied))
	# the play folder shows up under its own name in the mod list
	ini = os.path.join(dst_root, 'Mod.ini')
	if os.path.exists(ini):
		with open(ini) as f:
			text = f.read()
		if 'Name = Mod\n' in text:
			stat = os.stat(os.path.join(src_root, 'Mod.ini'))
			with open(ini, 'w') as f:
				f.write(text.replace('Name = Mod\n', 'Name = WTP (play)\n'))
			# keeps the time of the original: no copy on the next run unless the original changes
			os.utime(ini, (stat.st_atime, stat.st_mtime))

	# 2. the paks, when the art changed
	manifest = dict((rel_key(r), [os.stat(os.path.join(src_root, r)).st_size, int(os.stat(os.path.join(src_root, r)).st_mtime)]) for r in packed)
	manifest_path = os.path.join(dst_root, 'Assets', MANIFEST)
	old = None
	if os.path.exists(manifest_path):
		with open(manifest_path) as f:
			old = json.load(f)
	if args.paks or old != manifest:
		print('building the paks (%d art files)...' % len(packed))
		build_paks(src_root, os.path.join(dst_root, 'Assets'), packed)
		with open(manifest_path, 'w') as f:
			json.dump(manifest, f)
	else:
		print('paks unchanged')


if __name__ == '__main__':
	main()
