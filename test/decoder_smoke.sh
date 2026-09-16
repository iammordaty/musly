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

track_count() {
    musly -c "$1" -l | awk '/track-origin:/{n++} END{print n+0}'
}

echo "=== Removing tracks (-r) ==="
ROOT="$PWD/rmtest"
rm -rf "$ROOT" rm.musly rm.musly.bak rm.musly.jbox saved.jbox
mkdir -p "$ROOT/keep" "$ROOT/drop" "$ROOT/extra"
cp tone.wav "$ROOT/keep/k1.wav"
cp noise.wav "$ROOT/keep/k2.wav"
cp stereo.wav "$ROOT/drop/d1.wav"
cp lowrate.wav "$ROOT/drop/d2.wav"
cp tone.wav "$ROOT/extra/e1.wav"
cp noise.wav "$ROOT/extra/e2.wav"
musly -N -c rm.musly
musly -c rm.musly -a "$ROOT/keep" -a "$ROOT/drop" -x wav
test "$(track_count rm.musly)" = "4"

# build a jukebox state so we can check it gets invalidated
musly -c rm.musly -J -k 2 -p "$ROOT/keep/k1.wav" > /dev/null
test -f rm.musly.jbox
cp rm.musly.jbox saved.jbox

musly -c rm.musly -r "$ROOT/drop" | tee rm1.txt
grep -q "Matched 2 track(s)" rm1.txt
test "$(track_count rm.musly)" = "2"
# the previous collection is preserved
test "$(track_count rm.musly.bak)" = "4"
# stale jukebox state must not survive a removal
test ! -f rm.musly.jbox

# a target that matches nothing changes nothing
musly -c rm.musly -r "$ROOT/nope" | tee rm2.txt
grep -q "Matched 0 track(s)" rm2.txt
grep -q "Nothing to remove" rm2.txt
test "$(track_count rm.musly)" = "2"

echo "=== Jukebox fingerprint guard ==="
# Restore a jukebox built for 4 different tracks, then bring the collection
# back to 4 tracks. The counts match, the contents do not.
musly -c rm.musly -a "$ROOT/extra" -x wav > /dev/null
test "$(track_count rm.musly)" = "4"
cp saved.jbox rm.musly.jbox
musly -c rm.musly -J -k 2 -p "$ROOT/keep/k1.wav" | tee fp.txt
grep -q "does not match the contents" fp.txt

echo "=== Removing stale entries (-R) ==="
rm -f stale.musly stale.musly.bak
musly -N -c stale.musly
musly -c stale.musly -a "$ROOT" -x wav > /dev/null
test "$(track_count stale.musly)" = "6"
rm -f "$ROOT/drop/d2.wav"
# without -y nothing may change
musly -c stale.musly -R | tee stale1.txt
grep -q "Missing:.*d2.wav" stale1.txt
grep -q "Repeat with '-y'" stale1.txt
test "$(track_count stale.musly)" = "6"
musly -c stale.musly -R -y > /dev/null
test "$(track_count stale.musly)" = "5"

echo "=== Mass deletion guard ==="
GUARD="$PWD/guardtest"
rm -rf "$GUARD" guard.musly guard.musly.bak
mkdir -p "$GUARD"
for i in $(seq 1 24); do cp tone.wav "$GUARD/g$i.wav"; done
musly -N -c guard.musly
musly -c guard.musly -a "$GUARD" -x wav > /dev/null
test "$(track_count guard.musly)" = "24"
# simulate an unmounted volume: most files disappear at once
for i in $(seq 1 20); do rm -f "$GUARD/g$i.wav"; done
set +e
musly -c guard.musly -R -y > guard.txt 2>&1
GUARD_RET=$?
set -e
test "$GUARD_RET" != "0"
grep -q "Refusing to remove more than half" guard.txt
test "$(track_count guard.musly)" = "24"

echo "=== Batch -p and stdin ==="
rm -f batch.musly batch.musly.jbox
musly -N -c batch.musly
musly -c batch.musly -a tone.wav -a noise.wav -a stereo.wav
musly -c batch.musly -J -k 2 -p tone.wav -p noise.wav | tee batch1.txt
grep -c "most similar tracks to:" batch1.txt | grep -q 2
# unknown seed is skipped; the other seed still produces a playlist
set +e
musly -c batch.musly -J -k 2 -p tone.wav -p missing.wav > batch2.txt 2>&1
BATCH_RET=$?
set -e
test "$BATCH_RET" != "0"
grep -q "skipping: missing.wav" batch2.txt
grep -q "most similar tracks to: tone.wav" batch2.txt
printf '%s\n' tone.wav noise.wav | musly -c batch.musly -J -k 2 -p - | tee batch3.txt
grep -c "most similar tracks to:" batch3.txt | grep -q 2
# lean load on an exact match
grep -q "Reading jukebox file (lean)" batch3.txt

echo "=== Jukebox maintained on -a / -r with -J ==="
rm -f maint.musly maint.musly.jbox
musly -N -c maint.musly
musly -c maint.musly -J -a tone.wav -a noise.wav
test -f maint.musly.jbox
# adding more tracks with -J must keep the jukebox usable without a rebuild from -p
BEFORE=$(wc -c < maint.musly.jbox)
musly -c maint.musly -J -a stereo.wav | tee maint_add.txt
grep -q "Updating jukebox\|Rebuilding jukebox" maint_add.txt
test -f maint.musly.jbox
# removal with -J rebuilds rather than deleting
musly -c maint.musly -J -r noise.wav | tee maint_rm.txt
grep -q "Rebuilding jukebox after removal" maint_rm.txt
test -f maint.musly.jbox
musly -c maint.musly -J -k 2 -p tone.wav | tee maint_p.txt
grep -q "Reading jukebox file (lean)" maint_p.txt

echo "=== Decoder tests OK ==="
