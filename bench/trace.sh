#!/bin/sh
# Captures FFmpeg's reads of its input for each run shape a media server uses, with strace, into
# traces/ beside this script, for prefetch_sim.py.
#
#     trace.sh FFMPEG_DIR MEDIA_DIR
#
# FFMPEG_DIR holds ffmpeg and ffprobe. MEDIA_DIR holds film.mkv (video, a 5.1 track at 0:1, a stereo
# track at 0:2, subtitles at 0:3) and film-moov-at-end.mp4, the same film with its index at the end.
set -e
if [ $# -ne 2 ]; then
    echo "usage: $0 FFMPEG_DIR MEDIA_DIR" >&2
    exit 2
fi
F=$1
M=$2
OUT=$(dirname "$0")/traces
mkdir -p "$OUT"
run() { name=$1; shift; strace -f -tt -T -e trace=openat,read,pread64,lseek,close -o "$OUT/$name.strace" "$@" >/dev/null 2>&1 || true; }
run probe-mkv $F/ffprobe -v error -print_format json -show_format -show_streams -show_chapters -i file:$M/film.mkv
run probe-mp4-moov-end $F/ffprobe -v error -print_format json -show_format -show_streams -show_chapters -i file:$M/film-moov-at-end.mp4
run readthrough-mkv $F/ffmpeg -nostdin -nostats -i file:$M/film.mkv -filter_complex "[0:1]asplit=2[m][s];[m]ebur128=framelog=quiet[mo];[s]pan=stereo|FL<FL+0.707*FC+0.707*BL|FR<FR+0.707*FC+0.707*BR,ebur128=framelog=quiet[so]" -map "[mo]" -map "[so]" -f null - -map 0:0 -c copy -f framecrc -
run subtitles-mkv $F/ffmpeg -nostdin -v error -i file:$M/film.mkv -map 0:3 -c:s ass -f ass -
run chunk-copy-mkv $F/ffmpeg -nostdin -v error -ss 150 -to 156 -i file:$M/film.mkv -map 0:0 -c:v copy -f framemd5 -
run chunk-encode-mkv $F/ffmpeg -nostdin -v error -ss 150 -to 156 -i file:$M/film.mkv -map 0:0 -c:v libx264 -preset veryfast -b:v 3M -threads 2 -f null -
run chunk-sound-mkv $F/ffmpeg -nostdin -v error -ss 149 -to 156 -i file:$M/film.mkv -map 0:2 -c:a aac -b:a 128k -f null -
run chunk-copy-mp4 $F/ffmpeg -nostdin -v error -ss 150 -to 156 -i file:$M/film-moov-at-end.mp4 -map 0:0 -c:v copy -f framemd5 -
run prepare-remux-mkv $F/ffmpeg -nostdin -v error -i file:$M/film.mkv -map 0:0 -map 0:2 -c copy -f null -
