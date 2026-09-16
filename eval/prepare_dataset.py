#!/usr/bin/env python3
"""Build a genre/artist/track tree from FMA-small and write a manifest.

Layout: <out>/<genre>/<artist>/<track_id>.mp3
Relative paths genre/artist/id.mp3 make musly -e 0 -f 1 work when the
collection is built by adding each genre directory from the tree root.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict

import pandas as pd


def sanitize(name: str, fallback: str = "unknown") -> str:
    name = (name or "").strip()
    if not name:
        name = fallback
    # Keep path components portable across filesystems.
    name = re.sub(r"[/\\:\0]", "_", name)
    name = re.sub(r"\s+", " ", name).strip(" .")
    return name[:120] or fallback


def load_small_tracks(tracks_csv: str) -> pd.DataFrame:
    tracks = pd.read_csv(tracks_csv, index_col=0, header=[0, 1])
    mask = tracks[("set", "subset")] == "small"
    # Pull scalar columns explicitly — indexing a MultiIndex row with
    # row["artist"] would return every ('artist', *) field as a Series.
    out = pd.DataFrame(
        {
            "genre": tracks.loc[mask, ("track", "genre_top")].astype(str),
            "artist": tracks.loc[mask, ("artist", "name")].astype(str),
        }
    )
    return out


def fma_audio_path(audio_root: str, track_id: int) -> str:
    tid = f"{int(track_id):06d}"
    return os.path.join(audio_root, tid[:3], tid + ".mp3")


def link_or_copy(src: str, dst: str, mode: str, tree_root: str) -> None:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    if mode == "symlink":
        # Relative target so Docker can mount data-dir once and resolve links.
        rel_target = os.path.relpath(os.path.abspath(src), start=os.path.dirname(dst))
        os.symlink(rel_target, dst)
    elif mode == "hardlink":
        os.link(src, dst)
    else:
        shutil.copy2(src, dst)


def ffmpeg_decodable(path: str, timeout: int = 30) -> bool:
    try:
        r = subprocess.run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-i",
                path,
                "-f",
                "null",
                "-",
            ],
            capture_output=True,
            timeout=timeout,
        )
        return r.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def sha256_file(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--data-dir",
        default=os.path.join(os.path.dirname(__file__), "data"),
        help="Directory with fma_small/ and fma_metadata/",
    )
    ap.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "data", "tree"),
        help="Output tree root (genre/artist/track.mp3)",
    )
    ap.add_argument(
        "--mode",
        choices=("symlink", "hardlink", "copy"),
        default="symlink",
        help="How to place audio files in the tree",
    )
    ap.add_argument(
        "--validate",
        action="store_true",
        help="Decode each file with ffmpeg before accepting it",
    )
    ap.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Optional cap on number of tracks (0 = all)",
    )
    args = ap.parse_args()

    tracks_csv = os.path.join(args.data_dir, "fma_metadata", "tracks.csv")
    audio_root = os.path.join(args.data_dir, "fma_small")
    if not os.path.isfile(tracks_csv):
        raise SystemExit(f"Missing {tracks_csv}; run fetch_fma.py first")
    if not os.path.isdir(audio_root):
        raise SystemExit(f"Missing {audio_root}; run fetch_fma.py first")

    small = load_small_tracks(tracks_csv)
    if args.limit > 0:
        small = small.iloc[: args.limit]

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)

    rows = []
    rejected = []
    genre_counts = defaultdict(int)

    for track_id, row in small.iterrows():
        src = fma_audio_path(audio_root, int(track_id))
        genre = sanitize(str(row["genre"]), "Unknown")
        artist = sanitize(str(row["artist"]), f"artist_{track_id}")
        rel = f"{genre}/{artist}/{int(track_id):06d}.mp3"
        dst = os.path.join(args.out, rel)

        if not os.path.isfile(src):
            rejected.append({"track_id": int(track_id), "reason": "missing", "src": src})
            continue
        if args.validate and not ffmpeg_decodable(src):
            rejected.append({"track_id": int(track_id), "reason": "decode", "src": src})
            continue

        link_or_copy(src, dst, args.mode, args.out)
        digest = sha256_file(src)
        rows.append(
            {
                "track_id": int(track_id),
                "path": rel,
                "genre": genre,
                "artist": artist,
                "sha256": digest,
            }
        )
        genre_counts[genre] += 1

    rows.sort(key=lambda r: r["path"])
    manifest_path = os.path.join(args.out, "manifest.csv")
    with open(manifest_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["track_id", "path", "genre", "artist", "sha256"])
        w.writeheader()
        w.writerows(rows)

    # Stable fingerprint over sorted relative paths and content hashes.
    h = hashlib.sha256()
    for r in rows:
        h.update(f"{r['path']}:{r['sha256']}\n".encode())
    fingerprint = h.hexdigest()

    meta = {
        "n_tracks": len(rows),
        "n_rejected": len(rejected),
        "genres": dict(sorted(genre_counts.items())),
        "manifest_sha256": fingerprint,
        "tree": os.path.abspath(args.out),
        "mode": args.mode,
        "validated": bool(args.validate),
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    if rejected:
        with open(os.path.join(args.out, "rejected.json"), "w") as f:
            json.dump(rejected, f, indent=2)
            f.write("\n")

    print(json.dumps(meta, indent=2))
    print(f"Wrote {manifest_path}")
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
