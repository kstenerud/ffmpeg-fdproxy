#!/usr/bin/env python3
"""Measures the bytes each run shape is served through fdproxy, against films whose tracks are
stored one after another, for the block cache's size and the budget's figures (DESIGN.md, The Block Cache).

    interleave.py --bin DIR --work DIR [--quick]

DIR holds the patched ffmpeg and ffprobe. It makes its films in --work, from FFmpeg's test sources:
five minutes of video at 2 Mb/s, N sound tracks at 128 kb/s and a subtitle track, with every track in
one region of its own (no interleaving), for N of 2, 9, 15 and 23, and the 9-track film interleaved
as a muxer normally writes it. Every run goes through test/refparent.py, which counts what it served.
It prints one JSON line per run, then a Markdown table of served bytes over the file's size."""
import argparse, json, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
REFPARENT = os.path.join(HERE, "..", "test", "refparent.py")
SECONDS = 300


def cache_blocks(streams):
    """The formula these measurements disproved: 1.5 x the streams read, + 2, at least 2."""
    return max(2, (3 * streams + 1) // 2 + 2)


def make_film(ffmpeg, path, sound_tracks, interleaved):
    if os.path.exists(path):
        return
    with tempfile.TemporaryDirectory() as scratch:
        subtitles = os.path.join(scratch, "film.srt")
        with open(subtitles, "w", encoding="utf-8") as out:
            for number, start in enumerate(range(0, SECONDS, 10), 1):
                out.write(f"{number}\n00:{start // 60:02}:{start % 60:02},000 --> "
                          f"00:{(start + 8) // 60:02}:{(start + 8) % 60:02},000\nLine {number}\n\n")
        inputs = ["-f", "lavfi", "-i", f"testsrc2=size=1280x720:rate=24:duration={SECONDS}"]
        for track in range(sound_tracks):
            inputs += ["-f", "lavfi", "-i", f"sine=frequency={300 + 40 * track}:sample_rate=48000:duration={SECONDS}"]
        inputs += ["-i", subtitles]
        maps = ["-map", "0:v"] + [part for track in range(sound_tracks) for part in ("-map", f"{track + 1}:a")]
        maps += ["-map", f"{sound_tracks + 1}:s"]
        # One region per track: chunks as long as the film, and no interleaving delta.
        layout = [] if interleaved else ["-chunk_duration", "2e9", "-chunk_size", "2e9", "-max_interleave_delta", "0"]
        subprocess.run([ffmpeg, "-v", "error", "-nostdin", "-y", *inputs, *maps,
                        "-c:v", "libx264", "-preset", "veryfast", "-g", "48", "-b:v", "2M", "-maxrate", "2M",
                        "-bufsize", "4M", "-c:a", "aac", "-b:a", "128k", "-ac", "2", "-c:s", "mov_text",
                        *layout, path], check=True)


def shapes(ffmpeg, ffprobe, sound_tracks):
    """Each run shape a media server makes, with the streams it reads."""
    last_sound = sound_tracks  # stream index of the last sound track
    subtitle = sound_tracks + 1
    every = [part for stream in range(sound_tracks + 2) for part in ("-map", f"0:{stream}")]
    return {
        "probe": (sound_tracks + 2,
                  [ffprobe, "-v", "error", "{INPUT}", "-print_format", "json", "-show_streams", "-show_chapters"]),
        "read-through": (2, [ffmpeg, "-v", "error", "-nostdin", "{INPUT}", "-map", "0:0", "-c", "copy",
                             "-f", "framecrc", "-", "-map", "0:1", "-af", "ebur128=framelog=quiet",
                             "-f", "null", "-"]),
        "prepare, every track": (sound_tracks + 2,
                                 [ffmpeg, "-v", "error", "-nostdin", "{INPUT}", *every, "-c", "copy",
                                  "-f", "null", "-"]),
        "chunk, video copy": (1, [ffmpeg, "-v", "error", "-nostdin", "-ss", "150", "-to", "156", "{INPUT}",
                                  "-map", "0:0", "-c:v", "copy", "-f", "null", "-"]),
        "chunk, last sound": (1, [ffmpeg, "-v", "error", "-nostdin", "-ss", "149", "-to", "156", "{INPUT}",
                                  "-map", f"0:{last_sound}", "-c:a", "aac", "-b:a", "128k", "-f", "null", "-"]),
        "subtitles": (1, [ffmpeg, "-v", "error", "-nostdin", "{INPUT}", "-map", f"0:{subtitle}",
                          "-c:s", "ass", "-f", "ass", "-"]),
    }


def through(film, command, block, blocks, pipe_bytes=1 << 20, timeout=180):
    argv = [sys.executable, REFPARENT, "--file", film, "--block", str(block), "--cache-blocks", str(blocks),
            "--pipe-bytes", str(pipe_bytes), "--", *command]
    started = time.monotonic()
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"exit": "timeout", "ok": False, "served": 0, "size": os.path.getsize(film), "reads": 0,
                "seconds": timeout}
    seconds = time.monotonic() - started
    result = json.loads(out.stdout)
    return {"exit": result["exit"], "ok": result["succeeded"], "served": result["served"], "size": result["size"],
            "reads": result["reads"], "seconds": round(seconds, 2)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--quick", action="store_true", help="the 9-sound-track films only")
    parser.add_argument("--configs", help="block:blocks,... to run in place of the formula's sweep")
    args = parser.parse_args()
    ffmpeg, ffprobe = os.path.join(args.bin, "ffmpeg"), os.path.join(args.bin, "ffprobe")
    os.makedirs(args.work, exist_ok=True)
    films = [(n, False) for n in ((9,) if args.quick else (2, 9, 15, 23))] + [(9, True)]
    rows = []
    for sound_tracks, interleaved in films:
        name = f"{sound_tracks}-sound-{'interleaved' if interleaved else 'one-region-each'}.mp4"
        film = os.path.join(args.work, name)
        make_film(ffmpeg, film, sound_tracks, interleaved)
        for shape, (streams, command) in shapes(ffmpeg, ffprobe, sound_tracks).items():
            formula = cache_blocks(streams)
            configs = {(1 << 20, 8), (1 << 20, formula), (1 << 20, 2 * formula), (256 << 10, formula),
                       (4 << 20, formula)}
            if args.configs:
                configs = {tuple(int(part) for part in config.split(":")) for config in args.configs.split(",")}
            for block, blocks in sorted(configs):
                result = through(film, command, block, blocks)
                row = {"film": name, "tracks": sound_tracks + 2, "shape": shape, "streams": streams,
                       "block": block, "cache_blocks": blocks, "formula": formula, **result}
                print(json.dumps(row), flush=True)
                rows.append(row)
    print()
    print("| Film | Run | Streams read | Block | Blocks kept | Served ÷ size | Served MiB | Seconds | OK |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for row in rows:
        mark = " (formula)" if row["cache_blocks"] == row["formula"] else ""
        print(f"| {row['film']} | {row['shape']} | {row['streams']} | {row['block'] >> 10} KiB "
              f"| {row['cache_blocks']}{mark} | {row['served'] / row['size']:.2f} | {row['served'] / 2**20:.1f} "
              f"| {row['seconds']} | {'yes' if row['ok'] else 'NO, exit ' + str(row['exit'])} |")


if __name__ == "__main__":
    sys.exit(main())
