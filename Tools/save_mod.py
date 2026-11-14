# -*- coding: utf-8 -*-
"""Switches savegames between the development mod (Mods\\Mod\\) and the play copy (Mods\\WTP\\, see wtp_play.py).

A savegame starts with int32 2, int32 length and the mod folder ("Mods\\Mod\\"); the game loads that mod for it. Both names
have 9 characters, so the name is replaced in place. A backup of each changed save is kept as <name>.ColonizationSave.bak.

    python Tools/save_mod.py                 all saves of the Mod -> WTP
    python Tools/save_mod.py --to Mod        all saves of WTP -> Mod
    python Tools/save_mod.py FILE ...        only these saves
"""
import argparse
import glob
import os
import shutil
import struct

NAMES = {'Mod': b'Mods\\Mod\\', 'WTP': b'Mods\\WTP\\'}


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument('--to', choices=sorted(NAMES), default='WTP')
	parser.add_argument('files', nargs='*')
	args = parser.parse_args()
	new = NAMES[args.to]
	old = NAMES['Mod' if args.to == 'WTP' else 'WTP']
	files = args.files
	if not files:
		saves = os.path.join(os.path.expanduser('~'), 'Documents', 'My Games', 'Civ4Colonization', 'Saves', 'single')
		files = glob.glob(os.path.join(saves, '**', '*.ColonizationSave'), recursive=True)
	changed = 0
	for path in files:
		with open(path, 'rb') as f:
			head = f.read(8 + len(old))
		if len(head) < 8 + len(old) or struct.unpack_from('<I', head, 4)[0] != len(old) or head[8:] != old:
			continue
		shutil.copy2(path, path + '.bak')
		with open(path, 'r+b') as f:
			f.seek(8)
			f.write(new)
		changed += 1
		print('%s -> %s' % (os.path.basename(path), new.decode('ascii')))
	print('%d saves changed' % changed)


if __name__ == '__main__':
	main()
