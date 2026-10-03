#!/usr/bin/env bash
# Fetch the CC0 tilesets that csim supports into csim/tilesets/.
#
# Assets are NOT committed to the repo (see .gitignore) — run this once to
# populate them. Fetches the CC0 sets (Camashu CP437 + Kenney sprites) and
# DawnLike (CC-BY 4.0 — credit DawnBringer & DragonDePlatino; composited into a
# per-type atlas). The DF-wiki sets (curses/phoebus/anikki) have varied
# licenses; fetch those yourself after checking each set's terms.
#
#   tools/fetch-tilesets.sh
#
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dst="$root/csim/tilesets"
mkdir -p "$dst"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# best_png DIR PAT1 [PAT2 ...] : print the largest .png whose path matches the
# first pattern that yields any match (falls back to the largest png overall).
best_png() {
  local dir="$1"; shift
  local pat best bestsz sz f
  for pat in "$@" '.*'; do
    best=""; bestsz=0
    while IFS= read -r f; do
      sz=$(stat -c%s "$f" 2>/dev/null || echo 0)
      if printf '%s' "$f" | grep -qiE "$pat" && (( sz > bestsz )); then best="$f"; bestsz=$sz; fi
    done < <(find "$dir" -iname '*.png')
    [[ -n "$best" ]] && { printf '%s' "$best"; return 0; }
  done
  return 1
}

fetch_direct() { # url out
  echo ">> $2"
  curl -fsSL "$1" -o "$dst/$2" || { echo "   !! download failed"; return 1; }
}

fetch_zip() { # url out  pat...
  local url="$1" out="$2"; shift 2
  echo ">> $out"
  curl -fsSL "$url" -o "$tmp/a.zip" || { echo "   !! download failed"; return 1; }
  rm -rf "$tmp/x"; mkdir -p "$tmp/x"
  unzip -oq "$tmp/a.zip" -d "$tmp/x" || { echo "   !! unzip failed"; return 1; }
  local png; png="$(best_png "$tmp/x" "$@")" || { echo "   !! no matching png"; return 1; }
  cp "$png" "$dst/$out"
}

# Camashu — CC0 CP437 grid (non-square cells; the CP437 path auto-derives size).
fetch_direct "https://raw.githubusercontent.com/Camashu/DorfFortressTileSet/main/AzaraDorfs.png" camashu.png

# Kenney — CC0 semantic sprite sheets (16px tiles, 1px spacing; transparent variants).
fetch_zip "https://opengameart.org/sites/default/files/Roguelike%20pack.zip"          kenney_roguelike.png "sheet.*transparent|transparent.*sheet" "transparent"
fetch_zip "https://opengameart.org/sites/default/files/Roguelike%20Indoor%20pack.zip" kenney_indoor.png    "sheet.*transparent|transparent.*sheet" "transparent"
fetch_zip "https://opengameart.org/sites/default/files/Roguelike%20Cave%20pack.zip"   kenney_caves.png     "sheet.*transparent|transparent.*sheet" "transparent"
fetch_zip "https://opengameart.org/sites/default/files/1bitpack_kenney_1.1.zip"        kenney_1bit.png      "colou?red.*transparent|transparent.*colou?red" "transparent"

# DawnLike — CC-BY 4.0 (credit DawnBringer & DragonDePlatino). Many small files,
# so composite one tile per type into a canonical atlas (dawnlike.png) with PIL.
echo ">> dawnlike.png  (CC-BY 4.0 — credit DawnBringer & DragonDePlatino)"
if python3 -c "import PIL" 2>/dev/null; then
  if curl -fsSL "https://opengameart.org/sites/default/files/DawnLike_5.zip" -o "$tmp/dl.zip"; then
    rm -rf "$tmp/dl"; mkdir -p "$tmp/dl"; unzip -oq "$tmp/dl.zip" -d "$tmp/dl"
    SRC="$tmp/dl" OUT="$dst/dawnlike.png" python3 "$root/tools/_dawnlike_atlas.py" || echo "   !! composite failed"
  else echo "   !! download failed"; fi
else
  echo "   !! python3 + Pillow required for dawnlike; skipped"
fi

echo
echo "Fetched into $dst:"
for f in "$dst"/*.png; do [[ -e "$f" ]] && printf '  %-22s %s\n' "$(basename "$f")" "$(file -b "$f" | cut -d, -f1-2)"; done
