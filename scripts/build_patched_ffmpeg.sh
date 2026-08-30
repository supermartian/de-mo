#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 SOURCE_DIRECTORY INSTALL_PREFIX [JOBS]" >&2
    exit 2
fi

source_directory=$1
install_prefix=$2
jobs=${3:-$(getconf _NPROCESSORS_ONLN)}
script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_directory=$(cd -- "$script_directory/.." && pwd)
patch_file="$project_directory/patches/ffmpeg/0001-avcodec-h264-export-exact-motion-metadata-without-pix.patch"
ffmpeg_commit=b32f8d1c2377079302d23f82d555d13deda68c57

if ! source_directory=$(realpath -m -- "$source_directory") ||
   ! install_prefix=$(realpath -m -- "$install_prefix"); then
    echo "error: cannot canonicalize source or install path" >&2
    exit 2
fi

if [[ -e $source_directory ]]; then
    echo "error: source directory already exists: $source_directory" >&2
    exit 1
fi
if [[ -e $install_prefix ]]; then
    echo "error: install prefix already exists: $install_prefix" >&2
    exit 1
fi
if [[ $source_directory == "$install_prefix" ]]; then
    echo "error: source directory and install prefix must differ" >&2
    exit 2
fi
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    echo "error: JOBS must be a positive integer" >&2
    exit 2
fi

git clone https://git.ffmpeg.org/ffmpeg.git "$source_directory"
git -C "$source_directory" checkout "$ffmpeg_commit"
git -C "$source_directory" am "$patch_file"

(
    cd -- "$source_directory"
    ./configure \
        --prefix="$install_prefix" \
        --disable-doc \
        --disable-debug \
        --disable-autodetect \
        --disable-network \
        --disable-everything \
        --disable-x86asm \
        --enable-shared \
        --disable-static \
        --enable-decoder=h264 \
        --enable-demuxer=mov \
        --enable-parser=h264 \
        --enable-protocol=file \
        --enable-ffmpeg \
        --enable-ffprobe \
        --enable-muxer=null \
        --enable-filter=null
    make -j"$jobs"
    make install
)

echo "Patched FFmpeg installed in $install_prefix"
