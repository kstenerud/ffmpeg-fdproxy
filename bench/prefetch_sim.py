#!/usr/bin/env python3
"""Replays FFmpeg's reads of its input, captured by trace.sh, through a block protocol and a model
disk, to measure what prefetching the next block gains and what an invalidated prefetch costs.

FFmpeg's side asks for whole blocks, keeps the last few, and answers FFmpeg's reads from them. The
disk serves one request at a time, in order (a FIFO, as a spinning disk or one NAS mount does).
FFmpeg's processing time between reads is the gap the trace shows between one read's end and the
next's start."""
import re, sys, glob, os, collections

LINE = re.compile(r'^(\d+)\s+(\d+):(\d+):([\d.]+)\s+(\w+)\((.*?)\)\s+=\s+(-?\d+)(?:.*<([\d.]+)>)?')

def seconds(h, m, s): return int(h) * 3600 + int(m) * 60 + float(s)

def parse(path, media_dir):
    """The input's reads: (start, offset, bytes returned, duration), in order."""
    fds, positions, reads = set(), {}, []
    for raw in open(path, errors='replace'):
        m = LINE.match(raw)
        if not m: continue
        _, h, mi, s, call, args, result, took = m.groups()
        t, result, took = seconds(h, mi, s), int(result), float(took or 0)
        if call == 'openat' and media_dir in args and result >= 0:
            fds.add(result); positions[result] = 0
        elif call == 'close' and args.strip().isdigit() and int(args) in fds:
            fds.discard(int(args))
        elif call in ('read', 'pread64'):
            fd = int(args.split(',')[0])
            if fd not in fds or result < 0: continue
            if call == 'pread64':
                offset = int(args.rsplit(',', 1)[1])
            else:
                offset = positions[fd]; positions[fd] += result
            reads.append((t, offset, result, took))
        elif call == 'lseek':
            parts = [p.strip() for p in args.split(',')]
            fd = int(parts[0])
            if fd in fds and result >= 0: positions[fd] = result
    return reads

class Disk:
    """One request at a time, in order: a request's time is its latency, a seek when it doesn't
    follow the last, and its bytes at the disk's speed."""
    def __init__(self, latency, seek, bytes_per_s, busy=False):
        self.latency, self.seek, self.rate = latency, seek, bytes_per_s
        self.free_at, self.position, self.busy = 0.0, -1, 0.0
        # busy: another stream on the same disk, whose 1 MiB read is served before each of ours.
        self.contended = busy
    def serve(self, now, offset, size):
        start = max(now, self.free_at)
        if self.contended:
            start += self.latency + self.seek + (1 << 20) / self.rate
            self.position = -1
        cost = self.latency + (0 if offset == self.position else self.seek) + size / self.rate
        self.free_at, self.position = start + cost, offset + size
        self.busy += cost
        return self.free_at

def simulate(reads, block, cache_blocks, policy, disk, channel_cost):
    """Returns (total time, time waiting for data, blocks fetched, prefetched blocks never used)."""
    if not reads: return 0, 0, 0, 0
    cache = collections.OrderedDict()   # block -> ready time
    used, prefetched = set(), set()
    now, waited, fetched, last_needed = 0.0, 0.0, 0, None
    def fetch(k, at):
        nonlocal fetched
        fetched += 1
        ready = disk.serve(at, k * block, block) + channel_cost + block / 4e9
        cache[k] = ready; cache.move_to_end(k)
        while len(cache) > cache_blocks: cache.popitem(last=False)
        return ready
    for i, (t, offset, size, took) in enumerate(reads):
        first, last = offset // block, (offset + max(size, 1) - 1) // block
        for k in range(first, last + 1):
            if k not in cache:
                fetch(k, now)
            ready = cache[k]; cache.move_to_end(k)
            if ready > now: waited += ready - now; now = ready
            used.add(k)
            sequential = last_needed is not None and k == last_needed + 1
            if (policy == 'always' or (policy == 'sequential' and sequential)) and k + 1 not in cache:
                fetch(k + 1, now); prefetched.add(k + 1)
            last_needed = k
        if i + 1 < len(reads):
            think = max(0.0, reads[i + 1][0] - (t + took))
            now += think
    return now, waited, fetched, len(prefetched - used)

PROFILES = {
    'page cache': (50e-6, 0.0, 2e9),
    'NAS 1 Gb/s': (1e-3, 4e-3, 110e6),
    'local HDD': (0.2e-3, 9e-3, 160e6),
}

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    arguments = [argument for argument in sys.argv[1:] if argument != '--all']
    if len(arguments) != 1:
        sys.exit('usage: prefetch_sim.py MEDIA_DIR [--all]  (the directory trace.sh was given)')
    media = os.path.abspath(arguments[0])
    rows = []
    for path in sorted(glob.glob(os.path.join(here, 'traces', '*.strace'))):
        name = os.path.basename(path)[:-7]
        reads = parse(path, media)
        for disk_name, (lat, seek, rate) in PROFILES.items():
            for busy in (False, True):
                for block in (256 << 10, 1 << 20, 4 << 20):
                    for policy in ('none', 'sequential', 'always'):
                        disk = Disk(lat, seek, rate, busy)
                        total, waited, fetched, wasted = simulate(reads, block, 4, policy, disk, 30e-6)
                        rows.append((name, len(reads), sum(r[2] for r in reads) / 1e6, disk_name, busy, block >> 10,
                                     policy, total, waited, fetched, wasted))
    if '--all' in sys.argv[1:]:
        print('| Run | Reads | MB | Disk | Busy | Block KiB | Policy | Total s | Waiting s | Blocks | Wasted |')
        print('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
        for row in rows:
            print('| %s | %d | %.1f | %s | %s | %d | %s | %.2f | %.2f | %d | %d |' % row)
        return
    # The summary: 1 MiB blocks; total time under each policy, idle and busy, on the NAS and an HDD.
    print('| Run | Disk | Busy | none | sequential | always | wasted (seq / always) |')
    print('| --- | --- | --- | --- | --- | --- | --- |')
    by = {(r[0], r[3], r[4], r[5], r[6]): r for r in rows}
    for name in sorted({r[0] for r in rows}):
        for disk_name in ('NAS 1 Gb/s', 'local HDD'):
            for busy in (False, True):
                cells = [by[(name, disk_name, busy, 1024, p)] for p in ('none', 'sequential', 'always')]
                print('| %s | %s | %s | %.2f | %.2f | %.2f | %d / %d |' % (name, disk_name, 'yes' if busy else 'no',
                      cells[0][7], cells[1][7], cells[2][7], cells[1][10], cells[2][10]))

if __name__ == '__main__':
    main()
