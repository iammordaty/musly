#!/usr/bin/env python3
"""Clone a Musly collection file with a new method name (same track blobs).

The on-disk format is: null-terminated header (MUSLY-2-<method>) followed by
track records. Only the header string changes; feature bytes are unchanged.
Use for query-time-only method variants to skip re-analysis. The format
version must match musly/collectionfile.cpp.
"""

from __future__ import annotations

import argparse
import sys


def clone(src: str, dst: str, method: str) -> None:
    data = open(src, "rb").read()
    if not data.startswith(b"MUSLY-"):
        raise SystemExit(f"not a Musly collection: {src}")
    end = data.index(b"\0") + 1
    header = f"MUSLY-2-{method}\0".encode("ascii")
    with open(dst, "wb") as f:
        f.write(header)
        f.write(data[end:])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("method")
    args = ap.parse_args()
    clone(args.src, args.dst, args.method)
    print(f"cloned {args.src} -> {args.dst} (method={args.method})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
