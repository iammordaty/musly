#!/bin/bash
# Decoder path smoke test: generate audio with ffmpeg, run musly analyze/playlist.
set -euo pipefail

WORKDIR="${1:-/tmp/musly-decoder-test}"
mkdir -p "$WORKDIR"
cd "$WORKDIR"

echo "=== Generating test audio ==="
ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:duration=8" -ar 44100 tone.wav
ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=duration=8:color=white" -ar 44100 noise.wav
ffmpeg -y -loglevel error -f lavfi -i "anullsrc=duration=8" -ar 44100 silence.wav
ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=220:duration=8" -af "aeval=sin(2*PI*440*t)|sin(2*PI*554*t)" -ar 44100 stereo.wav
ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=880:duration=8" -ar 8000 lowrate.wav
# Perfect-cancellation stereo (L = -R) becomes digital silence after mono downmix —
# analysis is expected to fail (validates silence rejection on the decoder path).
ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=220:duration=8" -af "aeval=val(0)|-val(0)" -ar 44100 antiphase.wav
echo "not an audio file" > broken.wav

echo "=== musly -i ==="
musly -i

echo "=== Initialize collection (default method) ==="
rm -f collection.musly
musly -N -c collection.musly

echo "=== Analyze files ==="
musly -c collection.musly -a tone.wav
musly -c collection.musly -a noise.wav
musly -c collection.musly -a stereo.wav
musly -c collection.musly -a lowrate.wav
set +e
musly -c collection.musly -a silence.wav
musly -c collection.musly -a antiphase.wav
musly -c collection.musly -a broken.wav
set -e

echo "=== List tracks ==="
musly -c collection.musly -l | tee list.txt
grep -q tone.wav list.txt
grep -q noise.wav list.txt
grep -q stereo.wav list.txt
grep -q lowrate.wav list.txt
# silence / antiphase / broken must not appear
! grep -q silence.wav list.txt
! grep -q antiphase.wav list.txt
! grep -q broken.wav list.txt

echo "=== Playlist for tone ==="
musly -c collection.musly -k 3 -p tone.wav

echo "=== Sparse similarity matrix ==="
musly -c collection.musly -k 3 -s sparse.txt
test -s sparse.txt

echo "=== Multiple -a, trailing slash, UTF-8 paths ==="
LONGDIR="setB/zażółć gęślą jaźń — zażółć gęślą jaźń — zażółć gęślą jaźń"
rm -rf setA setB multi.musly
mkdir -p setA "$LONGDIR"
cp tone.wav setA/a1.wav
cp noise.wav "$LONGDIR/b1.wav"
musly -N -c multi.musly
# Both roots in a single invocation; the first one spelled with a trailing slash.
musly -c multi.musly -a setA/ -a setB -x wav | tee analyze.txt
grep -q "\[OK\]" analyze.txt
# The truncated progress output must remain decodable.
iconv -f utf-8 -t utf-8 analyze.txt > /dev/null
musly -c multi.musly -l | tee multi.txt
grep -q "setA/a1.wav" multi.txt
grep -q "b1.wav" multi.txt
# A trailing slash on the scan root must not leak into the stored paths.
! grep "track-origin" multi.txt | grep -q "//"

echo "=== Decoder tests OK ==="
