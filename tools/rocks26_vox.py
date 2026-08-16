"""Export the 26 rocks26 models as individual MagicaVoxel .vox files.

Reads:  game/assets/decoration/rocks26.json      (already baked from source/glb/rocks.glb)
Writes: game/assets/decoration/rocks/<name>.vox  (26 files, one per rock)

It reads the BAKED .json rather than re-running voxelize_rocks.py on the .glb, for three
reasons: that tool needs numpy and Pillow (the msys2 python on PATH has neither), rocks.glb is
a 27 MB sculpt that takes minutes to re-voxelize, and re-baking would risk producing something
subtly different from the rocks26.json the game is already shipping. The .json IS the asset;
this just re-expresses it one-model-per-file.

Every file carries the SAME shared 12-shade palette the bake produced, so the rocks stay colour-
consistent with each other and with rocks26.json. Colour indices are 1-based into RGBA, which is
the MagicaVoxel convention (index 0 means empty).

Stdlib only - run it with the plain `python` on PATH.

NOTE the game still loads rocks26.json, not these files. Editing one of them changes nothing on
its own; re-run voxelize_rocks.py to regenerate the .json, or point the loader at the folder.
"""
import json, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'game', 'assets', 'decoration', 'rocks26.json')
OUT = os.path.join(ROOT, 'game', 'assets', 'decoration', 'rocks')


def chunk(cid, content, children=b''):
    return cid + struct.pack('<II', len(content), len(children)) + content + children


def write_vox(path, m, pal):
    size = chunk(b'SIZE', struct.pack('<III', m['sx'], m['sy'], m['sz']))
    vv = b''.join(struct.pack('<BBBB', p & 255, (p >> 8) & 255, (p >> 16) & 255, ((p >> 24) & 255) + 1)
                  for p in m['vox'])
    xyzi = chunk(b'XYZI', struct.pack('<I', len(m['vox'])) + vv)
    rgba = b''
    for i in range(256):
        c = pal[i] if i < len(pal) else [0, 0, 0]
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150)
                           + chunk(b'MAIN', b'', size + xyzi + chunk(b'RGBA', rgba)))


J = json.load(open(SRC))
pal = J['pal']
os.makedirs(OUT, exist_ok=True)
print('%d rocks, %d shared shades' % (len(J['rocks']), len(pal)))
tot = 0
for m in sorted(J['rocks'], key=lambda r: (r['grp'], -len(r['vox']))):
    # the .glb's mesh names repeat the group ("Runic_1_Runic_0"); keep them as-is so a file can be
    # traced back to its source mesh, but make them safe for a filesystem.
    name = ''.join(ch if (ch.isalnum() or ch in '_-') else '_' for ch in m['name'])
    write_vox(os.path.join(OUT, name + '.vox'), m, pal)
    tot += len(m['vox'])
    print('  %-28s %-6s %3d x %3d x %3d  %6d voxels' % (name, m['grp'], m['sx'], m['sy'], m['sz'], len(m['vox'])))
kb = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT) if f.endswith('.vox')) / 1024
print('wrote %d files to %s (%.0f KB, %d voxels)' % (len(J['rocks']), OUT, kb, tot))
