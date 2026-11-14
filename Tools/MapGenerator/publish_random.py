"""Generate the playable Random New World maps (seeds 1 and 2), validate them and copy them to PrivateMaps.

python Tools/MapGenerator/publish_random.py

Seed 1 -> PrivateMaps/Random New World Massive.ColonizationWBSave
Seed 2 -> PrivateMaps/Random New World 2 Massive.ColonizationWBSave
Previews of each seed stay in Tools/MapGenerator/out_random/seed<N>.
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRIVATE = os.path.join(HERE, '..', '..', 'PrivateMaps')
NAME = 'Random New World Massive.ColonizationWBSave'
SEEDS = {1: NAME, 2: 'Random New World 2 Massive.ColonizationWBSave'}

for seed, target in sorted(SEEDS.items()):
    out = os.path.join(HERE, 'out_random', 'seed%d' % seed)
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'gen_random.py'), str(seed), out])
    wb = os.path.join(out, NAME)
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'validate.py'), wb])
    shutil.copyfile(wb, os.path.join(PRIVATE, target))
    print('seed', seed, '->', os.path.normpath(os.path.join(PRIVATE, target)))
