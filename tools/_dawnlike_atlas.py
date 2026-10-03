#!/usr/bin/env python3
# Composite one 16x16 DawnLike tile per csim TileType into a single-row atlas
# (dawnlike.png), in the canonical order the semantic_cell SEM_CANON layout
# expects. DawnLike is CC-BY 4.0 — credit DawnBringer & DragonDePlatino.
#
#   SRC=<unzipped DawnLike dir> OUT=<path/dawnlike.png> python3 _dawnlike_atlas.py
import os
from PIL import Image

src = os.environ["SRC"]; out = os.environ["OUT"]

def crop(path, col, row):
    im = Image.open(os.path.join(src, path)).convert("RGBA")
    return im.crop((col*16, row*16, col*16+16, row*16+16))

# canonical order: home shop work bar church police park water road person grass
tiles = [
    ("Objects/Door0.png",      0, 0),  # 0  home   (wood door)
    ("Objects/Decor0.png",     0, 3),  # 1  shop   (jar of goods)
    ("Objects/Decor0.png",     0, 0),  # 2  work   (window)
    ("Objects/Decor0.png",     3, 3),  # 3  bar    (bottle)
    ("Objects/Door0.png",      4, 0),  # 4  church (ornate door)
    ("Objects/Door0.png",      2, 0),  # 5  police (metal door)
    ("Objects/Tree0.png",      0, 3),  # 6  park   (green tree)
    ("Objects/Tile.png",       1, 0),  # 7  water  (blue)
    ("Objects/Tile.png",       2, 0),  # 8  road   (gray stone)
    ("Characters/Player0.png", 0, 0),  # 9  person
    ("Objects/Floor.png",      7, 4),  # 10 grass
]
atlas = Image.new("RGBA", (16*len(tiles), 16), (0, 0, 0, 0))
for i, (p, c, r) in enumerate(tiles):
    atlas.paste(crop(p, c, r), (i*16, 0))
os.makedirs(os.path.dirname(out), exist_ok=True)
atlas.save(out)
print("wrote", out, atlas.size)
