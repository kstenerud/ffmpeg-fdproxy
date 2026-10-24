/*
 * A libFuzzer harness for FFmpeg's side of ffstreamer. Each input drives a whole session: a header
 * chooses the parent and the channel (the terms, how the parent delivers and when it stalls, what
 * becomes of the pipe of requests, when the interrupt fires, which system calls fail), and a script
 * of operations follows (reads of any size, seeks with any whence, through the protocol's functions,
 * FFmpeg's URL layer or avio, a second open sharing or refusing descriptors, the parent closing or
 * filling a pipe). The harness is single-threaded and keeps no wall clock: the protocol's poll(),
 * read(), write(), close(), fcntl() and dup() are routed through it, so it knows every descriptor
 * the protocol duplicates and closes; a poll never waits, and a slice that would
 * have passed in silence is counted instead, so the parent acts between the protocol's own steps,
 * deterministically. It checks the specification's invariants as it goes, and aborts on a breach
 * (see check_read(), check_seek(), parse_frame(), finish_session()). Built with AddressSanitizer and
 * UndefinedBehaviorSanitizer by fuzz/build.sh; `make fuzz` runs it.
 *
 * Input layout: HEADER_BYTES of header (missing bytes read as 0), a count of operations and
 * OP_BYTES for each, a count of answer actions and one byte for each, then the rest, which a raw
 * parent sends as it is. fuzz/make_corpus.py writes seeds in this layout.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* Everything the protocol includes, first, so that the macros below reach only its own calls. */
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/thread.h"
#include "libavformat/avformat.h"
#include "libavformat/avio_internal.h"
#include "libavformat/url.h"

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031       /* Linux's, which glibc names only under _GNU_SOURCE */
#endif

static int hook_poll(struct pollfd *fds, nfds_t count, int timeout);
static ssize_t hook_read(int fd, void *buffer, size_t length);
static ssize_t hook_write(int fd, const void *buffer, size_t length);
static int hook_close(int fd);
static int hook_fcntl(int fd, int command, ...);
static int hook_dup(int fd);

/* The protocol itself, its system calls routed through the harness and its URLProtocol renamed, so
 * the library's stays apart. */
#define ff_ffstreamer_protocol fuzz_ffstreamer_protocol
#define poll  hook_poll
#define read  hook_read
#define write hook_write
#define close hook_close
#define fcntl hook_fcntl
#define dup   hook_dup
#include "libavformat/ffstreamer.c"
#undef poll
#undef read
#undef write
#undef close
#undef fcntl
#undef dup

#define HEADER_BYTES   16
#define OP_BYTES       4
#define MOST_OPS       48
#define BUFFER_BYTES   (1 << 20)   /* the largest single read the script asks */
#define MOST_SERVED    (2 << 20)   /* bytes one read-through op takes at most */
#define MOST_CACHE     (4 << 20)   /* cache_blocks x block, bounded so an input stays fast */
#define AFTER_LATCH    5           /* hooked calls an op may make once the interrupt is raised */
#define STUCK_CALLS    20000       /* hooked calls in a row moving no byte: a hang */
#define INPUT_CALLS    10000       /* hooked calls past which an input's script stops: speed */
#define HISTORY        4096

/* --- The film -------------------------------------------------------------------------------- */

/** The file's byte at offset o: differs from its neighbours, so a misplaced byte shows. */
static uint8_t film_byte(int64_t o)
{
    uint64_t x = (uint64_t)o * UINT64_C(0x9E3779B97F4A7C15);
    return (uint8_t)(x >> 56 ^ (uint64_t)o);
}

/* --- State ----------------------------------------------------------------------------------- */

enum Event { EV_DELAY, EV_STALL, EV_CLOSE, EV_CHUNK };

typedef struct QueueEvent {
    size_t at;      ///< taken when this many bytes of the queue have been sent
    int    type;    ///< enum Event
    int    value;   ///< slices for EV_DELAY, bytes per pass for EV_CHUNK
} QueueEvent;

/** The main session's parent: what it sends, and its reading of what the protocol sends. */
typedef struct Parent {
    int to_w;           ///< its end of the pipe to the protocol (non-blocking), or -1
    int from_r;         ///< its end of the pipe from the protocol (non-blocking), or -1
    int junk_w;         ///< the protocol's pipe of requests opened again, to fill it, or -1
    int ffmpeg_read;    ///< the caller's descriptors handed to the main session, or -1
    int ffmpeg_write;
    int dup_read;       ///< the session's duplicates of them, while it holds them, else -1
    int dup_write;

    uint8_t *queue;     ///< everything it will send, in order
    size_t queue_length, queue_size, sent;
    QueueEvent events[512];
    int events_count, next_event;
    int chunk;          ///< bytes it writes per pass of the protocol
    int base_chunk;
    int delay;          ///< silent slices before it writes again
    int stalled;        ///< it never writes again

    int draining;       ///< it reads what the protocol sends
    int drain_resume;   ///< silent slices before it reads again, 0 for never
    size_t before_junk; ///< protocol bytes in the pipe ahead of the junk
    size_t junk_left;   ///< junk bytes still to skip

    uint8_t back[16384];
    size_t back_length;
    int64_t back_total; ///< protocol bytes taken so far

    /* The protocol's frames, as read. */
    int next_sequence;
    int hellos, capabilities, dones;
    int done_result, done_kind, done_code;
    int64_t done_run;
    int capability_sequence;
    int reads_seen;
    int64_t outstanding_offsets[64];
    int outstanding_first, outstanding_count;
    size_t header_ends[64]; ///< queue offsets at which the DATA headers not yet taken end
    int header_first, header_count;
    int data_answered;  ///< DATA headers the protocol has taken
    int64_t frame_start; ///< where in the back stream the frame being read starts
} Parent;

typedef struct Global {
    /* From the header. */
    int responsive, use_avio, resolve_close;
    int inject_period, inject_salt;
    uint32_t interrupt_at;
    int resolve_after;

    /* The terms, as sent or (raw) as the protocol took them. */
    int16_t hello_lowest, hello_highest;
    int hello_sequence;
    int16_t version; int64_t run, size; int32_t block, longest; int8_t outstanding;
    int request_sequence_wrong;
    int request_malformed, request_invalid; ///< by the specification's rules
    int expect_open;    ///< 0 success, else the AVERROR a clean open must give
    int cache_requested, cache_effective;
    const uint8_t *actions; size_t actions_count, next_action;

    /* Running. */
    Parent p;
    URLContext *main_h;
    AVIOContext *pb;
    int open_ok, closed;
    uint32_t calls;
    int latched;
    int stuck, after_latch, idle_run;
    int disturbed;      ///< something besides the protocol may have changed the open's outcome
    int lossy;          ///< DONE may fail to reach the parent
    int back_closed;
    int write_injected; ///< a write failed by injection, not retried
    int closed_fds[16], closed_count; ///< what the protocol closed in this op
    int dups[16], dups_count; ///< the protocol's duplicates, open
    int main_opening;   ///< the main session's open is running
    int caller_fl[2], caller_fd_flags[2]; ///< the main session's caller descriptors' flags

    /* The model. */
    int fault;          ///< the first error a read returned, 0 while healthy
    int64_t position;   ///< where the next read starts
    int64_t fault_back; ///< the back stream's length when the fault was seen, -1 before
    int64_t history[HISTORY];
    int history_count;

    int trace;          ///< FFSTREAMER_FUZZ_TRACE is set
    uint8_t *buffer;
} Global;

static Global G;

static void die(const char *what)
{
    fprintf(stderr, "ffstreamer_fuzzer: INVARIANT BROKEN: %s\n", what);
    abort();
}

#define CHECK(cond, what) do { if (!(cond)) die(what); } while (0)

static int kind_of_error(int error)
{
    if (error == AVERROR_EXIT)
        return FFS_KIND_INTERRUPTED;
    if (error == AVERROR_INVALIDDATA)
        return FFS_KIND_PROTOCOL;
    return FFS_KIND_CHANNEL;
}

/* --- The parent's queue ---------------------------------------------------------------------- */

static void queue_bytes(Parent *p, const uint8_t *bytes, size_t length)
{
    if (p->queue_length + length > p->queue_size) {
        size_t size = FFMAX(p->queue_size * 2, p->queue_length + length + 4096);
        uint8_t *grown = av_realloc(p->queue, size);
        CHECK(grown, "harness out of memory");
        p->queue = grown;
        p->queue_size = size;
    }
    memcpy(p->queue + p->queue_length, bytes, length);
    p->queue_length += length;
}

static void queue_event(Parent *p, int type, int value)
{
    if (p->events_count < FF_ARRAY_ELEMS(p->events))
        p->events[p->events_count++] = (QueueEvent){ p->queue_length, type, value };
}

static void queue_header(Parent *p, int type, int sequence, int32_t length)
{
    uint8_t header[6] = { (uint8_t)type, (uint8_t)sequence };
    AV_WL32(header + 2, (uint32_t)length);
    queue_bytes(p, header, 6);
}

static void close_to(Parent *p)
{
    if (p->to_w >= 0)
        close(p->to_w);
    p->to_w = -1;
}

static void close_back(Parent *p)
{
    if (p->from_r >= 0) {
        close(p->from_r);
        G.back_closed = 1;
        G.lossy = 1;
    }
    p->from_r = -1;
}

/** Writes one pass's worth of the queue, as the parent would between the protocol's waits. */
static void feed(Parent *p)
{
    size_t want, limit;
    ssize_t n;

    while (p->next_event < p->events_count && p->events[p->next_event].at <= p->sent) {
        QueueEvent *e = &p->events[p->next_event++];
        switch (e->type) {
        case EV_DELAY: p->delay = e->value; break;
        case EV_STALL: p->stalled = 1; break;
        case EV_CLOSE: close_to(p); break;
        case EV_CHUNK: p->chunk = e->value ? e->value : p->base_chunk; break;
        }
    }
    if (p->to_w < 0 || p->stalled || p->delay > 0 || p->sent >= p->queue_length)
        return;
    limit = p->next_event < p->events_count ? p->events[p->next_event].at : p->queue_length;
    want = FFMIN(limit - p->sent, (size_t)p->chunk);
    if (!want)
        return;
    n = write(p->to_w, p->queue + p->sent, want);
    if (n > 0) {
        p->sent += n;
        G.idle_run = G.stuck = 0;
    }
}

/** Bytes the protocol has taken from the pipe to it. */
static size_t consumed(Parent *p)
{
    int pending = 0;
    if (p->to_w >= 0 && ioctl(p->to_w, FIONREAD, &pending) < 0)
        pending = 0;
    return p->sent - (size_t)pending;
}

/* --- The parent's answers -------------------------------------------------------------------- */

static void queue_request(Parent *p, int sequence)
{
    uint8_t body[27];
    if (G.request_sequence_wrong)
        sequence = (sequence + 1) % 128;
    queue_header(p, FFS_REQUEST, sequence, 27);
    AV_WL16(body, (uint16_t)G.version);
    AV_WL64(body + 2, (uint64_t)G.run);
    AV_WL64(body + 10, (uint64_t)G.size);
    AV_WL32(body + 18, (uint32_t)G.block);
    AV_WL32(body + 22, (uint32_t)G.longest);
    body[26] = (uint8_t)G.outstanding;
    queue_bytes(p, body, 27);
}

/** Answers a READ: correctly, slowly, by dribs, or by one of the faults the action picks. */
static void queue_data(Parent *p, int sequence, int64_t offset, int32_t length)
{
    uint8_t action = G.next_action < G.actions_count ? G.actions[G.next_action++] : 0;
    int type = FFS_DATA, header_length = length, body_length = length;
    uint8_t chunk[4096];

    switch (action % 16) {
    case 6:  queue_event(p, EV_CHUNK, FFMAX(1, length / 2048)); break; /* by dribs */
    case 7:  queue_event(p, EV_DELAY, 1 + (action >> 4)); break; /* slow, then right */
    case 8:  sequence = (sequence + 1 + (action >> 4)) % 128; break;
    case 9:  type = (action >> 4) == FFS_DATA ? FFS_HELLO : action >> 4; break;
    case 10: header_length = (action & 0x10) ? length + 1 : length - 1; body_length = header_length; break;
    case 11: body_length = (int32_t)(length * (int64_t)(action >> 4) / 16); break; /* then closes */
    case 12: queue_event(p, EV_CLOSE, 0); return;
    case 13: queue_event(p, EV_STALL, 0); return;
    case 14: queue_header(p, type, sequence, header_length); queue_event(p, EV_STALL, 0); return;
    default: break;
    }
    queue_header(p, type, sequence, header_length);
    if (type == FFS_DATA && header_length == length && p->header_count < 64)
        p->header_ends[(p->header_first + p->header_count++) % 64] = p->queue_length;
    for (int32_t done = 0; done < body_length;) {
        int32_t n = FFMIN(body_length - done, (int32_t)sizeof(chunk));
        for (int32_t i = 0; i < n; i++)
            chunk[i] = film_byte(offset + done + i);
        queue_bytes(p, chunk, n);
        done += n;
    }
    if (action % 16 == 6)
        queue_event(p, EV_CHUNK, 0);
    if (action % 16 == 11)
        queue_event(p, EV_CLOSE, 0);
}

/* --- Reading what the protocol sends --------------------------------------------------------- */

/** The block cache's size as the specification and the terms make it. */
static int effective_cache(int requested, int64_t size, int32_t block, int outstanding)
{
    int64_t blocks = size / block + !!(size % block);
    int64_t c = requested;
    if (c > blocks)
        c = FFMAX(blocks, 1);
    if (c < outstanding + 1)
        c = outstanding + 1;
    return (int)c;
}

static void remember(int64_t block)
{
    if (G.history_count == HISTORY) {
        memmove(G.history, G.history + HISTORY / 2, sizeof(G.history) / 2);
        G.history_count = HISTORY / 2;
    }
    G.history[G.history_count++] = block;
}

/**
 * A READ for a block the cache must still hold is least-recently-used eviction broken: evicting B
 * needs every other slot pending (at most outstanding - 1) or used after B, so at least
 * cache - outstanding other blocks used since B's last use.
 */
static void check_lru(int64_t block)
{
    int64_t seen[64];
    int distinct = 0, need = G.cache_effective - G.outstanding;

    for (int i = G.history_count - 1; i >= 0 && distinct < need; i--) {
        int known = 0;
        if (G.history[i] == block)
            die("a READ for a block least-recently-used eviction keeps");
        for (int j = 0; j < distinct; j++)
            known |= seen[j] == G.history[i];
        if (!known && distinct < FF_ARRAY_ELEMS(seen))
            seen[distinct++] = G.history[i];
    }
}

static void parse_read(Parent *p, int sequence, const uint8_t *body)
{
    int64_t offset = (int64_t)AV_RL64(body);
    int32_t length = (int32_t)AV_RL32(body + 8);

    CHECK(G.open_ok || G.responsive, "a READ before the open succeeded");
    CHECK(G.fault_back < 0 || p->frame_start < G.fault_back, "a READ after the channel's fault");
    CHECK(G.block > 0, "a READ without terms");
    CHECK(offset >= 0 && offset < G.size, "a READ outside the file");
    CHECK(offset % G.block == 0, "a READ not aligned to a block");
    CHECK(length == FFMIN((int64_t)G.block, G.size - offset), "a READ of other than its block");
    CHECK(length <= G.longest, "a READ longer than the terms");
    if (G.responsive) {
        /* A READ stops being outstanding when the protocol takes its DATA header. */
        size_t taken = consumed(p);
        while (p->header_count && p->header_ends[p->header_first] <= taken) {
            p->header_first = (p->header_first + 1) % 64;
            p->header_count--;
            p->data_answered++;
        }
        while (p->outstanding_count && p->reads_seen - p->outstanding_count < p->data_answered) {
            p->outstanding_first = (p->outstanding_first + 1) % 64;
            p->outstanding_count--;
        }
        for (int i = 0; i < p->outstanding_count; i++)
            CHECK(p->outstanding_offsets[(p->outstanding_first + i) % 64] != offset,
                  "a READ for a block already outstanding");
        CHECK(p->outstanding_count + 1 <= G.outstanding, "more READs outstanding than agreed");
        p->outstanding_offsets[(p->outstanding_first + p->outstanding_count++) % 64] = offset;
    }
    if (G.cache_effective)
        check_lru(offset / G.block);
    remember(offset / G.block);
    p->reads_seen++;
    if (G.responsive)
        queue_data(p, sequence, offset, length);
}

/** Checks one whole frame the protocol sent, and answers it if the parent is responsive. */
static void parse_frame(Parent *p, int type, int sequence, int32_t length, const uint8_t *body)
{
    CHECK(sequence == p->next_sequence, "the protocol's frames are not numbered in turn");
    p->next_sequence = (p->next_sequence + 1) % 128;
    CHECK(!p->dones, "a frame after DONE");

    switch (type) {
    case FFS_HELLO:
        CHECK(length == 4 && sequence == 0 && !p->hellos, "a bad HELLO");
        CHECK(AV_RL16(body) == 1 && AV_RL16(body + 2) == 1, "HELLO offers other versions");
        p->hellos++;
        break;
    case FFS_CAPABILITY:
        CHECK(length == 9 && p->hellos == 1 && !p->capabilities, "a bad CAPABILITY");
        p->capabilities++;
        p->capability_sequence = sequence;
        if (G.responsive)
            queue_request(p, sequence);
        break;
    case FFS_READ:
        CHECK(length == 12 && p->capabilities, "a bad READ");
        parse_read(p, sequence, body);
        break;
    case FFS_DONE: {
        int16_t message;
        CHECK(length >= 9 && p->capabilities, "a bad DONE");
        p->dones++;
        p->done_run = (int64_t)AV_RL64(body);
        p->done_result = body[8];
        CHECK(p->done_result == 0 || p->done_result == 1, "DONE's result is neither OK nor ERR");
        if (p->done_result == 0) {
            CHECK(length == 9, "DONE OK has a tail");
            break;
        }
        CHECK(length >= 16, "DONE ERR is short");
        p->done_kind = body[9];
        p->done_code = (int32_t)AV_RL32(body + 10);
        message = (int16_t)AV_RL16(body + 14);
        CHECK(message >= 0 && message <= FFS_MESSAGE_MAX && length == 16 + message,
              "DONE's message length is wrong");
        break;
    }
    default:
        die("the protocol sent a frame of no known type");
    }
}

static void parse_back(Parent *p)
{
    size_t at = 0;
    while (p->back_length - at >= FFS_HEADER_BYTES) {
        int32_t length = (int32_t)AV_RL32(p->back + at + 2);
        CHECK(length >= 0 && length <= FFS_MAX_SENT_FRAME - FFS_HEADER_BYTES,
              "a frame longer than 4,096 bytes");
        if (p->back_length - at < FFS_HEADER_BYTES + (size_t)length)
            break;
        p->frame_start = p->back_total - (int64_t)p->back_length + (int64_t)at;
        parse_frame(p, p->back[at], (int8_t)p->back[at + 1], length, p->back + at + 6);
        at += FFS_HEADER_BYTES + length;
    }
    memmove(p->back, p->back + at, p->back_length - at);
    p->back_length -= at;
}

/** Reads what the protocol sent, skipping any junk the harness put in its pipe. */
static void drain(Parent *p, int force)
{
    if (p->from_r < 0 || (!p->draining && !force))
        return;
    for (;;) {
        uint8_t skip[4096];
        size_t room = sizeof(p->back) - p->back_length;
        ssize_t n;

        if (!p->before_junk && p->junk_left) {
            n = read(p->from_r, skip, FFMIN(sizeof(skip), p->junk_left));
            if (n <= 0)
                return;
            p->junk_left -= n;
            continue;
        }
        if (p->junk_left)
            room = FFMIN(room, p->before_junk);
        n = read(p->from_r, p->back + p->back_length, room);
        if (n <= 0)
            return;
        if (p->junk_left)
            p->before_junk -= n;
        p->back_length += n;
        p->back_total += n;
        G.idle_run = G.stuck = 0;
        parse_back(p);
    }
}

/** Fills the pipe of requests to the brim with junk the parent will skip, and stops reading it. */
static void fill_back(Parent *p, int resume)
{
    int pending = 0;
    uint8_t junk[4096] = { 0 };

    drain(p, 1);
    if (p->from_r < 0 || p->junk_w < 0 || p->junk_left)
        return;
    if (ioctl(p->from_r, FIONREAD, &pending) < 0)
        return;
    p->before_junk = pending;
    for (;;) {
        ssize_t n = write(p->junk_w, junk, sizeof(junk));
        if (n <= 0) {
            n = write(p->junk_w, junk, 1);   /* the last bytes, under a page */
            if (n <= 0)
                break;
        }
        p->junk_left += n;
    }
    p->draining = 0;
    p->drain_resume = resume;
}

/* --- Routing the protocol's system calls ----------------------------------------------------- */

static void latch(void)
{
    if (!G.latched) {
        G.latched = 1;
        G.after_latch = 0;
        G.disturbed = 1;
    }
}

static int interrupt_callback(void *opaque)
{
    return G.latched;
}

/** Counts a call; the interrupt fires on a count, and once it has, the op must end soon. */
static void step(int retried)
{
    G.calls++;
    if (++G.stuck > STUCK_CALLS)
        die("the protocol spins: many calls, and no byte moves");
    if (G.latched && !retried && ++G.after_latch > AFTER_LATCH)
        die("the interrupt is raised, and the protocol waits on");
    if (G.interrupt_at && G.calls == G.interrupt_at)
        latch();
}

/** Whether this call fails by injection, and with which errno (0 for none). */
static int injected(const int *errors)
{
    uint32_t x;
    if (!G.inject_period)
        return 0;
    x = (G.calls + 1) * 2654435761u ^ (uint32_t)G.inject_salt;
    if ((x >> 20) % G.inject_period)
        return 0;
    return errors[(x >> 8) & 3];
}

static int retryable(int error)
{
    return error == EINTR || error == EAGAIN;
}

static void trace_call(const char *call, int fd, long ret, int error)
{
    if (G.trace > 1)
        fprintf(stderr, "  %u %s(%d) = %ld%s%s\n", G.calls, call, fd, ret,
                error ? " injected " : "", error ? strerror(error) : "");
}

static void idle_slice(Parent *p, int write_side)
{
    if (p) {
        if (!write_side && p->delay > 0) {
            p->delay--;
            return;
        }
        if (write_side && !p->draining && p->drain_resume > 0) {
            if (!--p->drain_resume)
                p->draining = 1;
            return;
        }
    }
    if (++G.idle_run <= G.resolve_after)
        return;
    G.idle_run = 0;
    /* Nothing will change by itself: the parent ends it, or the interrupt does. */
    if (G.resolve_close && p && !write_side && p->to_w >= 0) {
        close_to(p);
        G.disturbed = 1;
    } else if (G.resolve_close && p && write_side && p->from_r >= 0) {
        close_back(p);
        G.disturbed = 1;
    } else {
        latch();
    }
}

static Parent *parent_of(int fd, int *write_side)
{
    if (fd >= 0 && fd == G.p.dup_read) {
        *write_side = 0;
        return &G.p;
    }
    if (fd >= 0 && fd == G.p.dup_write) {
        *write_side = 1;
        return &G.p;
    }
    return NULL;
}

static int hook_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    static const int errors[4] = { EINTR, EINTR, ENOMEM, EAGAIN };
    int write_side = 0, error = injected(errors), ready;
    Parent *p = parent_of(fds[0].fd, &write_side);

    step(error == EINTR);   /* a poll that fails otherwise is a fault, EAGAIN too */
    if (p) {
        drain(p, 0);
        if (!write_side)
            feed(p);
    }
    /* Once the interrupt is raised, a wait for room that is interrupted or finds none gives up,
     * as Waiting and Interrupts says, and the DONE it was for is lost. */
    if (write_side && G.latched && (error == EINTR || !poll(fds, count, 0)))
        G.lossy = 1;
    if (error) {
        if (error != EINTR)
            G.disturbed = G.lossy = 1;
        trace_call("poll", fds[0].fd, -1, error);
        errno = error;
        return -1;
    }
    ready = poll(fds, count, 0);   /* never waits: a silent slice is counted instead */
    trace_call("poll", fds[0].fd, ready, 0);
    if (!ready)
        idle_slice(p, write_side);
    return ready;
}

static ssize_t hook_read(int fd, void *buffer, size_t length)
{
    static const int errors[4] = { EINTR, EAGAIN, EAGAIN, EIO };
    int error = injected(errors), write_side;
    Parent *p = parent_of(fd, &write_side);
    ssize_t n;

    step(retryable(error));
    if (error) {
        if (!retryable(error))
            G.disturbed = 1;
        trace_call("read", fd, -1, error);
        errno = error;
        return -1;
    }
    n = read(fd, buffer, length);
    trace_call("read", fd, n, 0);
    if (n > 0)
        G.idle_run = G.stuck = 0;
    if (p)
        drain(p, 0);
    return n;
}

static ssize_t hook_write(int fd, const void *buffer, size_t length)
{
    static const int errors[4] = { EINTR, EAGAIN, EIO, EINTR };
    int error = injected(errors), write_side;
    Parent *p = parent_of(fd, &write_side);
    ssize_t n;

    step(retryable(error));
    if (error) {
        if (!retryable(error)) {
            G.disturbed = 1;
            G.write_injected = 1;
            G.lossy = 1;
        }
        trace_call("write", fd, -1, error);
        errno = error;
        return -1;
    }
    n = write(fd, buffer, length);
    trace_call("write", fd, n, n < 0 ? errno : 0);
    if (n > 0)
        G.idle_run = G.stuck = 0;
    if (p)
        drain(p, 0);
    return n;
}

/** A descriptor the protocol made: it must close it, once. */
static void note_dup(int from, int fd)
{
    for (int i = 0; i < G.dups_count; i++)
        CHECK(G.dups[i] != fd, "a duplicate the protocol holds made again");
    CHECK(G.dups_count < FF_ARRAY_ELEMS(G.dups), "the protocol holds too many duplicates");
    G.dups[G.dups_count++] = fd;
    if (G.main_opening && from == G.p.ffmpeg_read)
        G.p.dup_read = fd;
    if (G.main_opening && from == G.p.ffmpeg_write)
        G.p.dup_write = fd;
}

static int hook_fcntl(int fd, int command, ...)
{
    va_list args;
    long argument;
    int ret;

    va_start(args, command);
    argument = va_arg(args, long);
    va_end(args);
    ret = fcntl(fd, command, argument);
    if (ret >= 0 && (command == F_DUPFD || command == F_DUPFD_CLOEXEC))
        note_dup(fd, ret);
    return ret;
}

static int hook_dup(int fd)
{
    int ret = dup(fd);
    if (ret >= 0)
        note_dup(fd, ret);
    return ret;
}

/** The protocol closes only descriptors it made, each exactly once: never the caller's. */
static int hook_close(int fd)
{
    int i;
    for (i = 0; i < G.dups_count && G.dups[i] != fd; i++)
        ;
    CHECK(i < G.dups_count,
          "the protocol closed a descriptor it did not make (the caller's), or one twice");
    G.dups[i] = G.dups[--G.dups_count];
    if (fd == G.p.dup_read)
        G.p.dup_read = -1;
    if (fd == G.p.dup_write)
        G.p.dup_write = -1;
    if (G.closed_count < FF_ARRAY_ELEMS(G.closed_fds))
        G.closed_fds[G.closed_count++] = fd;
    return close(fd);
}

/* --- Checking the protocol's answers --------------------------------------------------------- */

static int is_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

/** A caller's descriptor, after any open or op: open, its flags as the caller left them. */
static void check_caller(int fd, int fl, int fd_flags, const char *what)
{
    if (fd < 0)
        return;
    if (!is_open(fd))
        die(what);
    CHECK(fcntl(fd, F_GETFL) == fl && fcntl(fd, F_GETFD) == fd_flags,
          "the protocol changed a caller's descriptor's flags");
}

static void begin_op(void)
{
    G.stuck = 0;
    G.after_latch = 0;
    G.closed_count = 0;
}

/** After every op: the main session's caller descriptors untouched. */
static void end_op(void)
{
    check_caller(G.p.ffmpeg_read, G.caller_fl[0], G.caller_fd_flags[0],
                 "the protocol closed the caller's read descriptor");
    check_caller(G.p.ffmpeg_write, G.caller_fl[1], G.caller_fd_flags[1],
                 "the protocol closed the caller's write descriptor");
}

/** The protocol's url_read, checked: what FFmpeg's I/O layer may rely on. */
static int check_read(URLContext *h, unsigned char *buffer, int size)
{
    int ret = ffstreamer_read(h, buffer, size);

    if (h != G.main_h)
        goto end;
    CHECK(ret != 0, "a read returned 0");
    CHECK(ret != AVERROR(EAGAIN), "a read returned EAGAIN");
    CHECK(ret <= size, "a read returned more than asked");
    if (G.fault) {
        CHECK(ret == G.fault, "a fault is not sticky");
        goto end;
    }
    if (ret == AVERROR_EOF) {
        CHECK(G.position >= G.size, "end of file before the end");
        goto end;
    }
    if (G.position >= G.size)
        die("a read at the end did not return AVERROR_EOF");
    if (ret < 0) {
        G.fault = ret;
        G.fault_back = G.p.back_total;
        if (G.p.from_r >= 0 && !G.p.junk_left) {
            int pending = 0;
            ioctl(G.p.from_r, FIONREAD, &pending);
            G.fault_back += pending;
        } else {
            G.fault_back = -1;
        }
        goto end;
    }
    CHECK(ret <= G.size - G.position, "a read past the end of the file");
    if (G.responsive)
        for (int i = 0; i < ret; i++)
            if (buffer[i] != film_byte(G.position + i))
                die("a read returned bytes other than the file's");
    remember(G.position / G.block);
    if ((G.position + ret - 1) / G.block != G.position / G.block)
        remember((G.position + ret - 1) / G.block);
    G.position += ret;
end:
    return ret;
}

/** What a seek must return, by the specification's Seeking and End of File. */
static int64_t expected_seek(int64_t position, int whence)
{
    int64_t base;
    switch (whence) {
    case AVSEEK_SIZE: return G.size;
    case SEEK_SET:    return position >= 0 ? position : AVERROR(EINVAL);
    case SEEK_CUR:    base = G.position; break;
    case SEEK_END:    base = G.size; break;
    default:          return AVERROR(EINVAL);
    }
    if (position > INT64_MAX - base || base + position < 0)
        return AVERROR(EINVAL);
    return base + position;
}

static int64_t check_seek(URLContext *h, int64_t position, int whence)
{
    int64_t expect = h == G.main_h ? expected_seek(position, whence) : 0;
    int64_t ret = ffstreamer_seek(h, position, whence);

    if (h != G.main_h)
        return ret;
    CHECK(ret == expect, "a seek returned other than the specification's answer");
    if (ret >= 0 && whence != AVSEEK_SIZE)
        G.position = ret;
    return ret;
}

static const URLProtocol checked_protocol = {
    .name            = "ffstreamer",
    .url_open        = ffstreamer_open,
    .url_read        = check_read,
    .url_seek        = check_seek,
    .url_close       = ffstreamer_close,
    .priv_data_size  = sizeof(FFStreamerContext),
    .priv_data_class = &ffstreamer_class,
    .default_whitelist = "ffstreamer",
};

/* The URL layer's own options, so av_opt_free() frees the whitelist ffurl_connect() copies from
 * default_whitelist, as the library's URLContext class does. */
static const AVOption url_options[] = {
    { "protocol_whitelist", "", offsetof(URLContext, protocol_whitelist), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "protocol_blacklist", "", offsetof(URLContext, protocol_blacklist), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass url_class = {
    .class_name = "URLContext",
    .item_name  = av_default_item_name,
    .option     = url_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static URLContext *make_context(const char *url, int flags, int read_fd, int write_fd, int cache)
{
    URLContext *h = av_mallocz(sizeof(*h) + strlen(url) + 1);
    FFStreamerContext *c;

    if (!h)
        return NULL;
    c = av_mallocz(sizeof(*c));
    if (!c) {
        av_free(h);
        return NULL;
    }
    h->av_class = &url_class;
    h->filename = (char *)&h[1];
    strcpy(h->filename, url);
    h->prot = &checked_protocol;
    h->flags = flags;
    h->priv_data = c;
    h->interrupt_callback = (AVIOInterruptCB){ interrupt_callback, NULL };
    c->class = &ffstreamer_class;
    av_opt_set_defaults(c);
    c->read_fd_option = read_fd;
    c->write_fd_option = write_fd;
    c->cache_blocks = cache;
    return h;
}

/** Frees a context whose open failed, as ffurl_closep() does: the protocol's close is not called. */
static void free_context(URLContext *h)
{
    if (h) {
        av_opt_free(h->priv_data);
        av_free(h->priv_data);
        av_opt_free(h);
        av_free(h);
    }
}

/* --- A second open ---------------------------------------------------------------------------- */

static int make_pipe(int fds[2])
{
    return pipe(fds) < 0 ? -1 : 0;
}

/** Writes a second parent's opening into fd, and closes it: it needs no answers. */
static void second_opening(int fd, int preset)
{
    uint8_t frames[64 + 100], *at = frames;
    at[0] = FFS_HELLO; at[1] = 0; AV_WL32(at + 2, 4); AV_WL16(at + 6, 1); AV_WL16(at + 8, 1);
    at += 10;
    if (preset >= 2) {
        at[0] = FFS_REQUEST; at[1] = 1; AV_WL32(at + 2, 27);
        AV_WL16(at + 6, 1); AV_WL64(at + 8, 9); AV_WL64(at + 16, 300);
        AV_WL32(at + 24, preset == 3 ? 0 : 100); AV_WL32(at + 28, 100); at[32] = 1;
        at += 33;
    }
    if (preset == 2) {
        at[0] = FFS_DATA; at[1] = 2; AV_WL32(at + 2, 100);
        for (int i = 0; i < 100; i++)
            at[6 + i] = film_byte(i);
        at += 106;
    }
    if (preset)
        (void)!write(fd, frames, at - frames);
    close(fd);
}

/** Counts the DONEs among a second session's frames, and returns the last one's result. */
static int second_dones(int fd, int *result)
{
    uint8_t back[8192];
    ssize_t length = 0, n;
    int dones = 0;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    while ((n = read(fd, back + length, sizeof(back) - length)) > 0)
        length += n;
    for (ssize_t at = 0; length - at >= 6;) {
        int32_t frame = (int32_t)AV_RL32(back + at + 2);
        if (frame < 0 || length - at < 6 + frame)
            break;
        if (back[at] == FFS_DONE) {
            dones++;
            *result = frame >= 9 ? back[at + 6 + 8] : -1;
        }
        at += 6 + frame;
    }
    return dones;
}

/**
 * A second open while the main session is open or not: refused for its arguments or for a pipe
 * the live session holds (EBUSY), descriptors untouched; or claimed, and then closed exactly once,
 * whether it fails or succeeds.
 */
static void second_open(int variant, int preset)
{
    int a[2] = { -1, -1 }, b[2] = { -1, -1 }, extra = -1;
    int read_fd, write_fd, flags = AVIO_FLAG_READ, expect, ret, main_open;
    const char *url = "ffstreamer:";
    URLContext *h;
    uint32_t calls_before;
    int dups_before, fl[2], fd_flags[2];

    main_open = G.open_ok && !G.closed && G.p.ffmpeg_read >= 0;
    if ((variant == 0 || variant == 1 || variant == 11) && !main_open)
        return;
    if (make_pipe(a) < 0 || make_pipe(b) < 0)
        goto end;
    read_fd = a[0];
    write_fd = b[1];
    expect = 1;   /* claimed */
    switch (variant) {
    case 0:  read_fd = extra = dup(G.p.ffmpeg_read); expect = AVERROR(EBUSY); break;
    case 1:  write_fd = extra = dup(G.p.ffmpeg_write); expect = AVERROR(EBUSY); break;
    case 2:  break;
    case 3:  read_fd = STDIN_FILENO; expect = AVERROR(EINVAL); break;
    case 4:  write_fd = STDERR_FILENO; expect = AVERROR(EINVAL); break;
    case 5:  write_fd = read_fd; expect = AVERROR(EINVAL); break;
    case 6:  read_fd = extra = open("/dev/null", O_RDONLY); expect = AVERROR(EBADF); break;
    case 7:  read_fd = a[1]; write_fd = b[0]; expect = AVERROR(EBADF); break;
    case 8:  extra = dup(a[0]); close(extra); read_fd = extra; extra = -1;
             expect = AVERROR(EBADF); break;
    case 9:  url = "ffstreamer:x"; expect = AVERROR(EINVAL); break;
    case 10: flags = AVIO_FLAG_READ_WRITE; expect = AVERROR(EINVAL); break;
    default: read_fd = G.p.ffmpeg_read; write_fd = G.p.ffmpeg_write; expect = AVERROR(EBUSY);
             break;
    }
    if (read_fd < 0 || write_fd < 0)
        goto end;
    if (expect == 1)
        second_opening(a[1], preset), a[1] = -1;

    h = make_context(url, flags, read_fd, write_fd, 2);
    if (!h)
        goto end;
    fl[0] = fcntl(read_fd, F_GETFL);
    fd_flags[0] = fcntl(read_fd, F_GETFD);
    fl[1] = fcntl(write_fd, F_GETFL);
    fd_flags[1] = fcntl(write_fd, F_GETFD);
    dups_before = G.dups_count;
    begin_op();
    calls_before = G.calls;
    ret = ffurl_connect(h, NULL);
    end_op();
    CHECK(ret != AVERROR(EAGAIN) && ret != AVERROR_EOF, "an open returned EAGAIN or end of file");
    /* Whatever the outcome, the caller's descriptors are as they were. */
    if (variant != 8)
        check_caller(read_fd, fl[0], fd_flags[0], "a second open closed its read descriptor");
    check_caller(write_fd, fl[1], fd_flags[1], "a second open closed its write descriptor");
    if (expect != 1) {
        /* Refused before the claim: nothing sent, nothing duplicated, nothing closed. */
        CHECK(ret == expect, "a refused open gave the wrong error");
        CHECK(G.calls == calls_before, "a refused open made system calls on its descriptors");
        CHECK(G.dups_count == dups_before && G.closed_count == 0,
              "a refused open duplicated or closed a descriptor");
        free_context(h);
        goto end;
    }
    CHECK(ret != AVERROR(EBUSY), "an open on fresh pipes was refused as busy");
    if (ret < 0) {
        CHECK(G.dups_count == dups_before, "a failed open left a duplicate open");
        free_context(h);
    } else {
        uint8_t got[100];
        int n;
        begin_op();
        n = check_read(h, got, sizeof(got));
        end_op();
        if (n > 0)
            for (int i = 0; i < n; i++)
                CHECK(got[i] == film_byte(i), "a second session read other bytes");
        CHECK(G.dups_count == dups_before + 2, "an open session holds other than two duplicates");
        begin_op();
        ffurl_closep(&h);
        end_op();
        CHECK(G.dups_count == dups_before && G.closed_count == 2,
              "a second session's close did not close both its duplicates");
        check_caller(read_fd, fl[0], fd_flags[0], "a second session's close closed its read descriptor");
        check_caller(write_fd, fl[1], fd_flags[1], "a second session's close closed its write descriptor");
    }
    if (!G.disturbed && !G.inject_period) {
        int result = -1, dones = second_dones(b[0], &result);
        if (preset < 2) {
            CHECK(ret < 0 && dones == 0, "a second open without terms sent DONE or opened");
        } else if (preset == 3) {
            CHECK(ret < 0 && dones == 1 && result == 1, "terms refused without one DONE ERR");
        } else {
            CHECK(ret == 0 && dones == 1 && result == 0, "a good second session without DONE OK");
        }
    }
end:
    for (int i = 0; i < 2; i++) {
        if (a[i] >= 0 && is_open(a[i]))
            close(a[i]);
        if (b[i] >= 0 && is_open(b[i]))
            close(b[i]);
    }
    if (extra >= 0 && is_open(extra))
        close(extra);
}

/* --- The script ------------------------------------------------------------------------------ */

static uint8_t byte_at(const uint8_t *data, size_t size, size_t i)
{
    return i < size ? data[i] : 0;
}

static int64_t seek_offset(uint8_t kind, uint8_t value)
{
    int64_t block = G.block > 0 ? G.block : 1;
    switch (kind % 10) {
    case 0:  return (int64_t)value - 128;
    case 1:  return value * block;
    case 2:  return value * block + block - 1;
    case 3:  return G.size - value;
    case 4:  return G.size + value;
    case 5:  return INT64_MAX - value;
    case 6:  return INT64_MIN + value;
    case 7:  return -(int64_t)value * block;
    case 8:  return G.size / 2 + (int64_t)value - 128;
    default: return G.position > INT64_MAX - value ? INT64_MAX : G.position + value;
    }
}

static int do_read(int size, int via)
{
    int ret;
    begin_op();
    if (G.use_avio) {
        /* avio_read loops until it has them all: keep that to a few hundred blocks. */
        int64_t at = avio_tell(G.pb);
        size = (int)FFMIN((int64_t)size, (int64_t)G.block * 256);
        ret = avio_read(G.pb, G.buffer, size);
        if (ret > 0 && G.responsive && at >= 0)
            for (int i = 0; i < ret; i++)
                CHECK(G.buffer[i] == film_byte(at + i), "avio_read returned other bytes");
    } else if (via) {
        ret = ffurl_read2(G.main_h, G.buffer, size);
        CHECK(ret != 0 && ret != AVERROR(EAGAIN), "ffurl_read returned 0 or EAGAIN");
    } else {
        ret = G.main_h->prot->url_read(G.main_h, G.buffer, size);
    }
    end_op();
    return ret;
}

static void do_seek(uint8_t a, uint8_t b, uint8_t c)
{
    static const int whences[5] = { SEEK_SET, SEEK_CUR, SEEK_END, AVSEEK_SIZE, 7 };
    int whence = whences[a % 5];
    int64_t offset = seek_offset(b, c), ret;

    begin_op();
    if (!G.use_avio) {
        if (a & 0x80)
            ret = ffurl_seek2(G.main_h, offset, whence);
        else
            ret = G.main_h->prot->url_seek(G.main_h, offset, whence);
        (void)ret;
    } else if (whence == AVSEEK_SIZE || whence == 7) {
        ret = avio_size(G.pb);
        CHECK(ret == G.size, "avio_size is not the file's size");
    } else if (whence == SEEK_END) {
        int64_t target = expected_seek(offset, SEEK_END);
        if (target >= 0) {
            ret = avio_seek(G.pb, target, SEEK_SET);
            CHECK(ret < 0 || ret == target, "avio_seek from the end landed elsewhere");
        }
    } else {
        int64_t from = avio_tell(G.pb), target = offset;
        ret = avio_seek(G.pb, offset, whence);
        if (ret >= 0 && (whence == SEEK_SET || !__builtin_add_overflow(from, offset, &target)))
            CHECK(ret == target, "avio_seek landed elsewhere");
    }
    end_op();
}

static void parent_op(uint8_t a, uint8_t b)
{
    Parent *p = &G.p;
    switch (a % 7) {
    case 0: close_back(p); G.disturbed = 1; break;
    case 1: fill_back(p, b % 8); G.disturbed = 1; break;
    case 2: p->draining = 0; p->drain_resume = 0; G.disturbed = 1; break;
    case 3: close_to(p); G.disturbed = 1; break;
    case 4:
        if (p->ffmpeg_read >= 0) {
            fcntl(p->ffmpeg_read, F_SETFL, fcntl(p->ffmpeg_read, F_GETFL) | O_NONBLOCK);
            G.caller_fl[0] = fcntl(p->ffmpeg_read, F_GETFL);
        }
        break;
    case 5:
        if (p->ffmpeg_write >= 0) {
            fcntl(p->ffmpeg_write, F_SETFL, fcntl(p->ffmpeg_write, F_GETFL) | O_NONBLOCK);
            G.caller_fl[1] = fcntl(p->ffmpeg_write, F_GETFL);
        }
        break;
    default:
        p->draining = 1;
        drain(p, 1);
        break;
    }
}

static void run_op(const uint8_t *op)
{
    uint8_t code = op[0], a = op[1], b = op[2], c = op[3];
    int size = 1 + ((((int)a << 8) | b) << (c & 3));
    size = FFMIN(size, BUFFER_BYTES);

    if (G.calls >= INPUT_CALLS)
        return;

    switch (code % 12) {
    case 0: case 1: case 2:
        if (G.open_ok && !G.closed)
            do_read(size, c & 4);
        break;
    case 3: {
        int64_t served = 0;
        for (int i = 0; G.open_ok && !G.closed && i < 512 && served < MOST_SERVED &&
                        G.calls < INPUT_CALLS; i++) {
            int ret = do_read(size, c & 4);
            if (ret <= 0)
                break;
            served += ret;
        }
        break;
    }
    case 4: case 5:
        if (G.open_ok && !G.closed)
            do_seek(a, b, c);
        break;
    case 6: case 7:
        second_open(a % 12, b % 4);
        break;
    case 8: case 9:
        parent_op(a, b);
        break;
    case 10:
        latch();
        break;
    default:
        if (G.open_ok && !G.closed && !G.use_avio) {
            begin_op();
            CHECK(ffurl_seek2(G.main_h, 0, AVSEEK_SIZE) == G.size, "AVSEEK_SIZE is not the size");
            end_op();
        }
        break;
    }
}

/* The old harness's script, for an input that gives none: straight through, from the middle, and
 * seeks at the edges of the arithmetic. */
static const uint8_t default_script[][OP_BYTES] = {
    { 3, 0xff, 0xff, 0 },       /* read through, 64 KiB at a time */
    { 4, 0, 8, 128 },           /* SEEK_SET to half the size */
    { 0, 0xff, 0xff, 0 },
    { 4, 1, 5, 0 },             /* SEEK_CUR by INT64_MAX */
    { 4, 1, 6, 0 },             /* SEEK_CUR by INT64_MIN */
    { 4, 2, 5, 0 },             /* SEEK_END by INT64_MAX */
    { 4, 2, 0, 127 },           /* SEEK_END by -1 */
    { 0, 0xff, 0xff, 0 },
};

/* --- A session ------------------------------------------------------------------------------- */

/** Reads the header: the terms, the parent, the channel and the interrupt. */
static void read_header_bytes(const uint8_t *h)
{
    static const int16_t hellos[8][2] = { {1,1}, {1,1}, {1,1}, {1,1}, {1,5}, {2,3}, {0,1}, {1,1} };
    int exp = (h[1] & 31) % 21, kind = h[3] >> 6, perturb = (h[4] >> 1) & 15;
    int s16 = h[2] | (h[3] & 0x3f) << 8;

    G.responsive = h[0] & 1;
    G.use_avio = !!(h[0] & 2);
    G.resolve_close = !!(h[0] & 8);
    G.inject_period = (h[14] & 0x80) ? 3 + (h[14] & 31) : 0;
    G.inject_salt = h[14];
    G.interrupt_at = (uint32_t)h[7] | (uint32_t)h[8] << 8;
    G.resolve_after = 1 + h[9] % 8;

    G.hello_lowest = hellos[h[13] % 8][0];
    G.hello_highest = hellos[h[13] % 8][1];
    G.hello_sequence = h[13] % 8 == 7 ? 1 : 0;

    G.block = 1 << exp;
    if ((h[1] & 0x40) && G.block > 2)
        G.block -= 1;
    switch (kind) {
    case 0:  G.size = s16; break;
    case 1:  G.size = (int64_t)s16 * 128; break;
    case 2:  G.size = (int64_t)G.block * (s16 & 63) + (s16 >> 6) % 3 - 1; break;
    default: G.size = FFS_LARGEST_FILE - (s16 & 0x3fff); break;
    }
    if (G.size < 0)
        G.size = 0;
    G.longest = (h[4] & 0x80) ? FFMIN(G.block * 2, FFS_LARGEST_BODY) : G.block;
    G.outstanding = 1 + (h[4] & 1);
    G.version = 1;
    G.run = 1 + h[15];
    G.cache_requested = 2 + h[5] % 63;
    while ((int64_t)G.cache_requested * G.block > MOST_CACHE && G.cache_requested > 3)
        G.cache_requested--;
    if (!G.responsive)  /* a raw parent's block may be 4 MiB */
        G.cache_requested = FFMIN(G.cache_requested, 3);

    switch (perturb) {
    case 5:  G.version = 2; break;
    case 6:  G.run = 0; break;
    case 7:  G.run = -1; break;
    case 8:  G.size = FFS_LARGEST_FILE + 1; break;
    case 9:  G.size = -1; break;
    case 10: G.block = 0; break;
    case 11: G.block = FFS_LARGEST_BODY + 1; G.longest = G.block; break;
    case 12: G.longest = G.block - 1; break;
    case 13: G.outstanding = 0; break;
    case 14: G.outstanding = 3; break;
    case 15: G.request_sequence_wrong = 1; break;
    default: break;
    }
    G.request_malformed = G.request_sequence_wrong || G.run < 0 || G.size < 0 || G.block < 0 ||
                          G.longest < 0 || G.outstanding < 0;
    G.request_invalid = !G.request_malformed &&
        (G.version != 1 || G.run < 1 || G.size > FFS_LARGEST_FILE || G.block < 1 ||
         G.block > FFS_LARGEST_BODY || G.longest < G.block || G.longest > FFS_LARGEST_BODY ||
         G.outstanding < 1 || G.outstanding > FFS_OUTSTANDING_CAP);
    if (G.hello_sequence || G.hello_lowest < 1 || G.hello_highest < 1 || G.hello_lowest > 1)
        G.expect_open = AVERROR_INVALIDDATA;
    else if (G.request_malformed)
        G.expect_open = AVERROR_INVALIDDATA;
    else if (G.request_invalid)
        G.expect_open = AVERROR(EINVAL);
    else
        G.expect_open = 0;
}

static void setup_parent(Parent *p, const uint8_t *h, int read_fd, int write_fd)
{
    p->ffmpeg_read = read_fd;
    p->ffmpeg_write = write_fd;
    p->dup_read = p->dup_write = -1;
    G.caller_fl[0] = fcntl(read_fd, F_GETFL);
    G.caller_fd_flags[0] = fcntl(read_fd, F_GETFD);
    G.caller_fl[1] = fcntl(write_fd, F_GETFL);
    G.caller_fd_flags[1] = fcntl(write_fd, F_GETFD);
    /* Bytes per pass: down to one, but no more than some 2,048 passes for a block. */
    p->base_chunk = p->chunk = FFMAX(1 << (h[6] % 17), G.block / 2048);
    p->draining = 1;
    p->capability_sequence = -1;
}

/**
 * After the main session's close (or its failed open): its descriptors closed exactly once, and
 * DONE sent exactly as When DONE Is Sent says.
 */
static void finish_session(int open_ret, int read_fd, int write_fd)
{
    Parent *p = &G.p;

    p->draining = 1;
    p->drain_resume = 0;
    drain(p, 1);
    CHECK(p->dones <= 1, "DONE sent more than once");
    if (G.open_ok) {
        if (!G.lossy)
            CHECK(p->dones == 1, "a session that opened ended without DONE");
        if (p->dones) {
            if (!G.fault) {
                CHECK(p->done_result == 0, "DONE ERR for a session without a fault");
            } else {
                CHECK(p->done_result == 1, "DONE OK for a session with a fault");
                CHECK(p->done_kind == kind_of_error(G.fault), "DONE's kind is not the fault's");
                CHECK(p->done_code == -G.fault, "DONE's code is not the fault's");
            }
            if (G.responsive)
                CHECK(p->done_run == G.run, "DONE does not echo the run");
        }
    } else if (G.responsive && !G.disturbed) {
        if (G.expect_open == AVERROR(EINVAL)) {
            CHECK(p->dones == 1 || G.lossy, "terms refused without DONE");
            if (p->dones)
                CHECK(p->done_result == 1 && p->done_kind == FFS_KIND_TERMS &&
                      p->done_run == (G.run >= 1 ? G.run : 0), "terms refused with the wrong DONE");
        } else {
            CHECK(p->dones == 0, "DONE from an open that failed before its terms");
        }
    }
    (void)open_ret;
    /* Second sessions end within their own op, so every duplicate open now is the main one's. */
    CHECK(G.dups_count == 0 && p->dup_read < 0 && p->dup_write < 0,
          "the session ended without closing both its duplicates");
    check_caller(read_fd, G.caller_fl[0], G.caller_fd_flags[0],
                 "the session's end closed the caller's read descriptor");
    check_caller(write_fd, G.caller_fl[1], G.caller_fd_flags[1],
                 "the session's end closed the caller's write descriptor");
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    av_log_set_level(AV_LOG_QUIET);
    signal(SIGPIPE, SIG_IGN);   /* as ffmpeg and ffprobe do: a parent gone is EPIPE, a fault */
    /* 1: a line for each input, for seeds; 2: and each system call the protocol makes. */
    G.trace = getenv("FFSTREAMER_FUZZ_TRACE") ? atoi(getenv("FFSTREAMER_FUZZ_TRACE")) : 0;
    G.buffer = av_malloc(BUFFER_BYTES);
    return G.buffer ? 0 : -1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t header[HEADER_BYTES];
    int to_ffmpeg[2] = { -1, -1 }, from_ffmpeg[2] = { -1, -1 };
    int read_fd, write_fd, ret, probe;
    size_t at, ops_count, actions_count;
    const uint8_t *ops;
    uint8_t *buffer = G.buffer;
    int trace = G.trace;
    struct timespec started = { 0 };   /* for the trace alone: nothing depends on the clock */

    memset(&G, 0, sizeof(G));
    G.buffer = buffer;
    G.trace = trace;
    if (trace)
        clock_gettime(CLOCK_MONOTONIC, &started);
    G.fault_back = -1;
    G.p.to_w = G.p.from_r = G.p.junk_w = G.p.ffmpeg_read = G.p.ffmpeg_write = -1;
    G.p.dup_read = G.p.dup_write = -1;
    for (int i = 0; i < HEADER_BYTES; i++)
        header[i] = byte_at(data, size, i);
    read_header_bytes(header);
    at = HEADER_BYTES;
    ops_count = FFMIN((size_t)byte_at(data, size, at), (size_t)MOST_OPS);
    at++;
    ops = data + FFMIN(at, size);
    if (at + ops_count * OP_BYTES > size)
        ops_count = size > at ? (size - at) / OP_BYTES : 0;
    at += ops_count * OP_BYTES;
    actions_count = byte_at(data, size, at);
    at++;
    if (at + actions_count > size)
        actions_count = size > at ? size - at : 0;
    G.actions = data + FFMIN(at, size);
    G.actions_count = actions_count;
    at += actions_count;

    if (pipe(to_ffmpeg) < 0 || pipe(from_ffmpeg) < 0)
        goto end;
    if (header[0] & 4)  /* the smallest pipe of requests the kernel makes: a page */
        fcntl(from_ffmpeg[1], F_SETPIPE_SZ, 4096);
    fcntl(to_ffmpeg[1], F_SETFL, O_NONBLOCK);
    fcntl(from_ffmpeg[0], F_SETFL, O_NONBLOCK);
    read_fd = to_ffmpeg[0];
    write_fd = from_ffmpeg[1];
    if (header[0] & 32) {   /* handed over non-blocking: the open must clear it */
        fcntl(read_fd, F_SETFL, O_RDONLY | O_NONBLOCK);
        fcntl(write_fd, F_SETFL, O_WRONLY | O_NONBLOCK);
    }
    G.p.to_w = to_ffmpeg[1];
    G.p.from_r = from_ffmpeg[0];
    {
        /* A description of its own, so it can write without blocking: a dup would share the
         * protocol's, and its O_NONBLOCK. */
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", write_fd);
        G.p.junk_w = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    }
    to_ffmpeg[0] = to_ffmpeg[1] = from_ffmpeg[0] = from_ffmpeg[1] = -1;
    setup_parent(&G.p, header, read_fd, write_fd);

    if (G.responsive) {
        uint8_t hello[4];
        queue_header(&G.p, FFS_HELLO, G.hello_sequence, 4);
        AV_WL16(hello, (uint16_t)G.hello_lowest);
        AV_WL16(hello + 2, (uint16_t)G.hello_highest);
        queue_bytes(&G.p, hello, 4);
    } else {
        if (at < size)
            queue_bytes(&G.p, data + at, size - at);
        queue_event(&G.p, (header[0] & 64) ? EV_STALL : EV_CLOSE, 0);
    }

    G.main_h = make_context("ffstreamer:", AVIO_FLAG_READ, read_fd, write_fd, G.cache_requested);
    if (!G.main_h)
        goto end;
    begin_op();
    G.main_opening = 1;
    ret = ffurl_connect(G.main_h, NULL);
    G.main_opening = 0;
    end_op();
    CHECK(ret != AVERROR(EAGAIN) && ret != AVERROR_EOF, "an open returned EAGAIN or end of file");
    if (G.inject_period || G.interrupt_at)
        G.disturbed = 1;
    if (ret < 0) {
        CHECK(ret != AVERROR(EBUSY) && ret != AVERROR(EBADF), "the main session's open was refused");
        if (G.responsive && !G.disturbed)
            CHECK(ret == G.expect_open, "the open's outcome is not the specification's");
        finish_session(ret, read_fd, write_fd);
        free_context(G.main_h);
        G.main_h = NULL;
        goto ops;
    }
    G.open_ok = 1;
    if (G.responsive && !G.disturbed)
        CHECK(G.expect_open == 0, "an open succeeded that the specification refuses");
    {
        FFStreamerContext *c = G.main_h->priv_data;
        CHECK(G.dups_count == 2 && c->read_fd == G.p.dup_read && c->write_fd == G.p.dup_write,
              "the session does not work on two duplicates of the caller's descriptors");
        CHECK((fcntl(c->read_fd, F_GETFD) & FD_CLOEXEC) && (fcntl(c->write_fd, F_GETFD) & FD_CLOEXEC),
              "the session's duplicates are inheritable");
        if (!G.responsive) {
            /* A raw parent's terms are whatever the protocol accepted. */
            G.size = c->size;
            G.block = c->block;
            G.longest = c->longest;
            G.outstanding = c->outstanding_allowed;
            G.run = c->run;
        }
        G.cache_effective = effective_cache(G.cache_requested, G.size, G.block, G.outstanding);
        CHECK(c->cache_blocks == G.cache_effective, "the cache is not the size the terms make");
    }
    if (G.use_avio) {
        begin_op();
        if (ffio_fdopen(&G.pb, G.main_h) < 0) {
            ffurl_closep(&G.main_h);
            G.closed = 1;
            end_op();
            finish_session(0, read_fd, write_fd);
            goto ops;
        }
        end_op();
    }

ops:
    if (ops_count) {
        for (size_t i = 0; i < ops_count; i++)
            run_op(ops + i * OP_BYTES);
    } else {
        for (size_t i = 0; i < FF_ARRAY_ELEMS(default_script); i++)
            run_op(default_script[i]);
    }

    if (G.open_ok && !G.closed) {
        G.p.draining = 1;   /* the parent reads on while the session ends */
        drain(&G.p, 1);
        begin_op();
        if (G.use_avio)
            avio_closep(&G.pb);
        else
            ffurl_closep(&G.main_h);
        end_op();
        G.closed = 1;
        finish_session(0, read_fd, write_fd);
    }

end:
    if (G.trace) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        fprintf(stderr, "trace: open %s, %d READs, fault %d, DONE %d (result %d kind %d), "
                "calls %u, latched %d, disturbed %d, lossy %d, cache %d, block %d, %ld us\n",
                G.open_ok ? "ok" : "failed", G.p.reads_seen, G.fault, G.p.dones,
                G.p.done_result, G.p.done_kind, G.calls, G.latched, G.disturbed, G.lossy,
                G.cache_effective, (int)G.block, (long)((now.tv_sec - started.tv_sec) * 1000000 +
                (now.tv_nsec - started.tv_nsec) / 1000));
    }
    CHECK(G.dups_count == 0, "the protocol leaked a duplicate");
    /* The caller's descriptors are the caller's to close. */
    if (G.p.ffmpeg_read >= 0)
        close(G.p.ffmpeg_read);
    if (G.p.ffmpeg_write >= 0)
        close(G.p.ffmpeg_write);
    close_to(&G.p);
    if (G.p.from_r >= 0)
        close(G.p.from_r);
    if (G.p.junk_w >= 0)
        close(G.p.junk_w);
    for (int i = 0; i < 2; i++) {
        if (to_ffmpeg[i] >= 0)
            close(to_ffmpeg[i]);
        if (from_ffmpeg[i] >= 0)
            close(from_ffmpeg[i]);
    }
    av_freep(&G.p.queue);
    /* No descriptor leaked: the lowest free number is the one it was before the first input. */
    probe = dup(STDIN_FILENO);
    if (probe >= 0) {
        static int baseline = -1;
        if (baseline < 0)
            baseline = probe;
        CHECK(probe == baseline, "a descriptor leaked");
        close(probe);
    }
    return 0;
}
