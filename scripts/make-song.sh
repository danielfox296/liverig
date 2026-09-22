#!/usr/bin/env bash
#
# make-song.sh <input-audio> <song-name>
#
# Splits an audio file into Demucs 4-stem parts and lays them out the way
# LiveRig expects:
#
#   $HOME/LiveRig/Songs/<song-name>/{drums,bass,other,vocals}.wav
#
# Prints that folder path on stdout when it finishes; progress goes to stderr,
# so `open "$(make-song.sh track.mp3 'Sway')"` works.
#
# Input can be anything ffmpeg reads — Suno downloads as MP3 or WAV are fine.

set -euo pipefail

RIG_ROOT="$HOME/LiveRig"
SONGS_DIR="$RIG_ROOT/Songs"
VENV_DIR="$RIG_ROOT/venv"
MODEL="htdemucs"
DEVICE="${RIG_DEMUCS_DEVICE:-cpu}"

say()  { printf '%s\n' "$*" >&2; }
die()  { printf 'make-song: %s\n' "$*" >&2; exit 1; }

if [ "$#" -ne 2 ]; then
    cat >&2 <<'USAGE'
usage: make-song.sh <input-audio> <song-name>

  <input-audio>  the mixed file to split (mp3, wav, m4a, flac...)
  <song-name>    folder name to create under ~/LiveRig/Songs

example:
  ./make-song.sh ~/Downloads/suno-take-3.mp3 "Fly Me To The Moon"

environment:
  RIG_DEMUCS_DEVICE   cpu (default) or mps to use the GPU
USAGE
    exit 2
fi

INPUT="$1"
NAME="$2"

[ -f "$INPUT" ] || die "no such file: $INPUT"
[ -n "$NAME" ] || die "song name cannot be empty"

case "$NAME" in
    */*) die "song name cannot contain a slash" ;;
    .|..) die "song name cannot be . or .." ;;
esac

# --- find demucs -----------------------------------------------------------
if [ -x "$VENV_DIR/bin/demucs" ]; then
    DEMUCS="$VENV_DIR/bin/demucs"
elif command -v demucs >/dev/null 2>&1; then
    DEMUCS="$(command -v demucs)"
else
    cat >&2 <<USAGE
make-song: demucs is not installed.

Install it into a virtualenv the script will find automatically:

  python3 -m venv "$VENV_DIR"
  "$VENV_DIR/bin/pip" install --upgrade pip
  "$VENV_DIR/bin/pip" install demucs numpy

The first run downloads the htdemucs model (about 300 MB) into
~/.cache/torch. See the README for details.
USAGE
    exit 1
fi

command -v ffmpeg >/dev/null 2>&1 || say "warning: ffmpeg not found; demucs may not be able to read compressed input"

# --- separate --------------------------------------------------------------
WORK="$(mktemp -d "${TMPDIR:-/tmp}/make-song.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

say "make-song: splitting $(basename "$INPUT") with $MODEL on $DEVICE (this takes a few minutes)"

"$DEMUCS" \
    --name "$MODEL" \
    --device "$DEVICE" \
    --int24 \
    --out "$WORK" \
    --filename "{stem}.{ext}" \
    "$INPUT" >&2

SRC="$WORK/$MODEL"
[ -d "$SRC" ] || die "demucs produced no output in $SRC"

# --- lay the stems out for the rig ----------------------------------------
DEST="$SONGS_DIR/$NAME"
mkdir -p "$DEST"

for stem in drums bass other vocals; do
    [ -f "$SRC/$stem.wav" ] || die "demucs did not produce $stem.wav"
    mv -f "$SRC/$stem.wav" "$DEST/$stem.wav"
done

say "make-song: done"
printf '%s\n' "$DEST"
