#!/usr/bin/env bash
#
# make-songs-batch.sh [folder]
#
# Runs every audio file in <folder> (default ~/Downloads/YoutubeToMp3)
# through make-song.sh. The song name is the filename minus its extension
# and minus junk suffixes like "(Official Audio)" or "[Lyric Video]".
#
# Songs that already have all four stems under /Users/fox296/LiveRig/Songs
# are skipped, so the batch can be interrupted and re-run, and new files
# dropped into the folder are picked up by the next run.
#
# environment:
#   RIG_DEMUCS_DEVICE   cpu (default) or mps to use the GPU

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MAKE_SONG="$SCRIPT_DIR/make-song.sh"
SONGS_DIR="/Users/fox296/LiveRig/Songs"
SRC_DIR="${1:-$HOME/Downloads/YoutubeToMp3}"

say() { printf '%s\n' "$*" >&2; }
die() { printf 'make-songs-batch: %s\n' "$*" >&2; exit 1; }

[ -x "$MAKE_SONG" ] || die "cannot find make-song.sh next to this script"
[ -d "$SRC_DIR" ] || die "no such folder: $SRC_DIR"

# demucs runs with MPS fallback on so the few ops the GPU can't do go to CPU
# instead of aborting the track.
export PYTORCH_ENABLE_MPS_FALLBACK=1

# --- song name from filename ----------------------------------------------
# "Ray LaMontagne - You Are the Best Thing (Official Audio).mp3"
#   -> "Ray LaMontagne - You Are the Best Thing"
# Keeps parentheticals that mean something ("[Crossroads 2010]", "(Single
# Version)"); only chunks containing a junk keyword are dropped.
clean_name() {
    local n="${1%.*}"
    n="$(printf '%s' "$n" | sed -E \
        -e 's/ *[([][^])]*[Oo]fficial[^])]*[])]//g' \
        -e 's/ *[([][^])]*([Ll]yric|[Vv]isuali[sz]er|[Aa]udio [Oo]nly|[Mm]usic [Vv]ideo|[Rr]emaster|HD|4[Kk])[^])]*[])]//g')"
    n="${n//\//-}"
    printf '%s' "$n" | sed -E 's/^ +//; s/ +$//'
}

# --- gather the work list --------------------------------------------------
shopt -s nullglob nocaseglob
files=("$SRC_DIR"/*.mp3 "$SRC_DIR"/*.m4a "$SRC_DIR"/*.wav "$SRC_DIR"/*.flac)
shopt -u nullglob nocaseglob
[ "${#files[@]}" -gt 0 ] || die "no audio files in $SRC_DIR"

names=()
for f in "${files[@]}"; do
    name="$(clean_name "$(basename "$f")")"
    [ -n "$name" ] || name="$(basename "${f%.*}")"
    names+=("$name")
done

total=${#files[@]}

done_count=0 skip_count=0 fail_count=0
failed=()
batch_start=$SECONDS

i=0
for f in "${files[@]}"; do
    name="${names[$i]}"
    i=$((i + 1))

    # "Song (1).mp3" next to "Song.mp3" is a duplicate download, not a song.
    if [[ "$name" =~ ^(.+)\ \([0-9]+\)$ ]] \
        && printf '%s\n' "${names[@]}" | grep -qxF "${BASH_REMATCH[1]}"; then
        say "[$i/$total] skip  $name (duplicate download)"
        skip_count=$((skip_count + 1))
        continue
    fi

    dest="$SONGS_DIR/$name"
    if [ -f "$dest/drums.wav" ] && [ -f "$dest/bass.wav" ] \
        && [ -f "$dest/other.wav" ] && [ -f "$dest/vocals.wav" ]; then
        say "[$i/$total] skip  $name (already split)"
        skip_count=$((skip_count + 1))
        continue
    fi

    say "[$i/$total] split $name"
    track_start=$SECONDS
    if "$MAKE_SONG" "$f" "$name" >/dev/null; then
        say "[$i/$total] done  $name ($(( SECONDS - track_start ))s)"
        done_count=$((done_count + 1))
    else
        say "[$i/$total] FAIL  $name"
        fail_count=$((fail_count + 1))
        failed+=("$name")
    fi
done

# --- summary ---------------------------------------------------------------
elapsed=$(( SECONDS - batch_start ))
say "----------------------------------------"
say "batch finished in $(( elapsed / 60 ))m: $done_count split, $skip_count skipped, $fail_count failed"
for name in ${failed+"${failed[@]}"}; do
    say "  failed: $name"
done
[ "$fail_count" -eq 0 ]
