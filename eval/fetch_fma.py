#!/usr/bin/env python3
"""Download FMA-small audio and metadata, verify SHA-1, and unpack."""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import urllib.request
import zipfile

BASE_URL = "https://os.unil.cloud.switch.ch/fma"
ARCHIVES = {
    "fma_metadata.zip": "f0df49ffe5f2a6008d7dc83c6915b31835dfe733",
    "fma_small.zip": "ade154f733639d52e35e32f5593efe5be76c6d70",
}


def sha1_file(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def download(url: str, dest: str) -> None:
    print(f"Downloading {url} -> {dest}")
    tmp = dest + ".partial"
    urllib.request.urlretrieve(url, tmp)
    os.replace(tmp, dest)


def ensure_archive(data_dir: str, name: str, expected_sha1: str) -> str:
    path = os.path.join(data_dir, name)
    if os.path.isfile(path):
        digest = sha1_file(path)
        if digest == expected_sha1:
            print(f"OK {name} ({digest})")
            return path
        print(f"Checksum mismatch for {name}: got {digest}, expected {expected_sha1}")
        print("Re-downloading...")
    download(f"{BASE_URL}/{name}", path)
    digest = sha1_file(path)
    if digest != expected_sha1:
        raise SystemExit(f"Checksum failed for {name}: {digest} != {expected_sha1}")
    print(f"OK {name} ({digest})")
    return path


def unpack(zip_path: str, data_dir: str) -> None:
    print(f"Unpacking {zip_path}")
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(data_dir)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--data-dir",
        default=os.path.join(os.path.dirname(__file__), "data"),
        help="Directory for archives and extracted files",
    )
    ap.add_argument(
        "--skip-unpack",
        action="store_true",
        help="Only download and verify checksums",
    )
    args = ap.parse_args()
    os.makedirs(args.data_dir, exist_ok=True)

    paths = {}
    for name, sha1 in ARCHIVES.items():
        paths[name] = ensure_archive(args.data_dir, name, sha1)

    if not args.skip_unpack:
        meta_dir = os.path.join(args.data_dir, "fma_metadata")
        audio_dir = os.path.join(args.data_dir, "fma_small")
        if not os.path.isdir(meta_dir):
            unpack(paths["fma_metadata.zip"], args.data_dir)
        else:
            print(f"Already unpacked: {meta_dir}")
        if not os.path.isdir(audio_dir):
            unpack(paths["fma_small.zip"], args.data_dir)
        else:
            print(f"Already unpacked: {audio_dir}")

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
