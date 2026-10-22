#!/usr/bin/env python3
"""Make the suite's two films from FFmpeg's own test sources, so the suite needs no film of anyone's.

    make_media.py --ffmpeg FFMPEG OUT_DIR

Both films are 5 minutes long with the same four streams, in the order the suite maps them:

    0  video, H.264 720p at a constant 3 Mb/s, so a whole read passes the suite's 50 MiB budget
    1  sound, 5.1
    2  sound, stereo
    3  subtitles, one line every 10 seconds

and five chapters. film.mkv is Matroska; film-moov-at-end.mp4 is MP4 with its index written after
the media, so a reader must seek to the end before it can read the start. Any FFmpeg with libx264
makes them; the patched one does."""
import argparse, os, subprocess, sys, tempfile

SECONDS = 300
CHAPTERS = 5
SUBTITLE_EVERY = 10


def timestamp(seconds, separator):
    return f"{seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02}{separator}000"


def write_subtitles(path):
    with open(path, "w", encoding="utf-8") as out:
        for number, start in enumerate(range(0, SECONDS, SUBTITLE_EVERY), 1):
            out.write(f"{number}\n{timestamp(start, ',')} --> {timestamp(start + SUBTITLE_EVERY - 2, ',')}\n"
                      f"Line {number}, at {start} seconds.\n\n")


def write_chapters(path):
    length = SECONDS // CHAPTERS
    with open(path, "w", encoding="utf-8") as out:
        out.write(";FFMETADATA1\ntitle=Test film\n")
        for number in range(CHAPTERS):
            out.write(f"[CHAPTER]\nTIMEBASE=1/1000\nSTART={number * length * 1000}\n"
                      f"END={(number + 1) * length * 1000}\ntitle=Chapter {number + 1}\n")


def make(ffmpeg, out_dir, scratch):
    subtitles = os.path.join(scratch, "film.srt")
    chapters = os.path.join(scratch, "chapters.txt")
    write_subtitles(subtitles)
    write_chapters(chapters)
    sources = [
        "-f", "lavfi", "-i", f"testsrc2=size=1280x720:rate=24:duration={SECONDS}",
        "-f", "lavfi", "-i", f"sine=frequency=440:sample_rate=48000:duration={SECONDS}",
        "-f", "lavfi", "-i", f"sine=frequency=660:sample_rate=48000:duration={SECONDS}",
        "-i", subtitles,
        "-i", chapters,
    ]
    streams = [
        "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s", "-map_metadata", "4", "-map_chapters", "4",
        "-filter:a:0", "aformat=channel_layouts=5.1", "-filter:a:1", "aformat=channel_layouts=stereo",
        "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-g", "48",
        "-b:v", "3M", "-minrate", "3M", "-maxrate", "3M", "-bufsize", "6M", "-x264-params", "nal-hrd=cbr",
        "-c:a", "aac", "-b:a:0", "384k", "-b:a:1", "128k",
    ]
    common = [ffmpeg, "-v", "error", "-nostdin", "-y", *sources, *streams]
    subprocess.run([*common, "-c:s", "srt", os.path.join(out_dir, "film.mkv")], check=True)
    # MP4 writes its index at the end unless asked to move it (-movflags +faststart), which this is not.
    subprocess.run([*common, "-c:s", "mov_text", os.path.join(out_dir, "film-moov-at-end.mp4")], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("out_dir")
    args = parser.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        make(args.ffmpeg, args.out_dir, scratch)
    for name in ("film.mkv", "film-moov-at-end.mp4"):
        path = os.path.join(args.out_dir, name)
        print(f"{path}: {os.path.getsize(path) >> 20} MiB")


if __name__ == "__main__":
    sys.exit(main())
