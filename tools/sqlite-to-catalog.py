#!/usr/bin/env python3
"""TRAPSTARSHOP: convert the user's catalog.sqlite into the store's
catalog-user.json plus a covers/ folder of image files.

The base carries everything embedded (covers as BLOBs, magnets in `source`).
The console client reads catalog-user.json at startup (USB /data/homebrew/
catalog-user/) and loads covers straight from the covers/ folder through
file:// URLs - no re-encoding, no internet needed for the shelf.

Usage:  python tools/sqlite-to-catalog.py <catalog.sqlite> <output-dir>
Output: <output-dir>/catalog-user.json and <output-dir>/covers/<id>.<ext>
"""
import json
import sqlite3
import sys
from pathlib import Path

EXT = {"image/jpeg": "jpg", "image/png": "png", "image/webp": "webp"}
GAMES_SQL = """
SELECT id, title, platform, source, region, size_gb, title_id, dub, version,
       release_year, genre, developer, publisher, content_type, cover_id,
       topic_url, aliases_json, provided_regions
FROM games WHERE content_type IN ('game', 'collection') ORDER BY title COLLATE NOCASE
"""
MODS_SQL = """
SELECT id, title, platform, source, region, size_gb, version, cover_id
FROM games WHERE content_type = 'mod'
"""


def cover_path(db, cover_id, covers_dir):
    if cover_id is None:
        return None
    row = db.execute("SELECT mime FROM cover_images WHERE id=?", (cover_id,)).fetchone()
    if not row:
        return None
    name = f"{cover_id}.{EXT.get(row[0], 'jpg')}"
    if not (covers_dir / name).exists():
        blob = db.execute("SELECT data FROM cover_images WHERE id=?", (cover_id,)).fetchone()[0]
        (covers_dir / name).write_bytes(blob)
    return f"covers/{name}"


def release_json(row):
    return {
        "source": row["source"],
        "region": row["region"],
        "version": row["version"],
        "title_id": row["title_id"],
        "size_gb": round(row["size_gb"], 3),
    }


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    covers = out / "covers"
    covers.mkdir(exist_ok=True)
    db = sqlite3.connect(f"file:{src.as_posix()}?mode=ro", uri=True)
    db.row_factory = sqlite3.Row

    # Alternates per game, preferred first, then the rest as mirrors. The store
    # ranks a release's sources in order and keeps the fastest alive on its own.
    alts: dict[int, list[sqlite3.Row]] = {}
    for row in db.execute("""
            SELECT gr.game_id, r.* FROM game_releases gr
            JOIN releases r ON r.id = gr.release_id
            ORDER BY gr.is_preferred DESC, r.seeds_at_scan DESC"""):
        alts.setdefault(row["game_id"], []).append(row)

    games = []
    for row in db.execute(GAMES_SQL):
        releases = alts.get(row["id"], [])
        if not releases:
            continue
        entry = {
            "title": row["title"],
            "platform": row["platform"],
            "region": row["region"],
            "dub": row["dub"],
            "version": row["version"],
            "size_gb": round(row["size_gb"], 3),
            "title_id": row["title_id"],
            "source": releases[0]["source"],
            "mirrors": [release_json(r) for r in releases[1:12]],
            "topic_url": row["topic_url"],
        }
        if row["release_year"]:
            entry["year"] = row["release_year"]
        if row["genre"]:
            entry["genres"] = [g.strip() for g in row["genre"].split(",") if g.strip()]
        if row["developer"]:
            entry["developer"] = row["developer"]
        if row["publisher"]:
            entry["publisher"] = row["publisher"]
        cover = cover_path(db, row["cover_id"], covers)
        if cover:
            entry["cover_url"] = "file:///" + cover
        games.append(entry)

    mods = []
    for row in db.execute(MODS_SQL):
        mods.append({
            "title": row["title"],
            "game_title": None,
            "platform": row["platform"],
            "source": row["source"],
            "version": row["version"],
            "size_gb": round(row["size_gb"], 3),
            "region": row["region"],
            "cover_url": ("file:///" + c) if (c := cover_path(db, row["cover_id"], covers)) else None,
        })

    meta = dict(db.execute("SELECT key, value FROM catalog_meta").fetchall())
    data = {"games": games, "mods": mods, "meta": meta}
    dest = out / "catalog-user.json"
    dest.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print(f"games={len(games)} mods={len(mods)} -> {dest} ({dest.stat().st_size >> 10} KiB)")
    print(f"covers: {len(list(covers.iterdir()))} files, {sum(f.stat().st_size for f in covers.iterdir()) >> 20} MiB")


if __name__ == "__main__":
    main()
