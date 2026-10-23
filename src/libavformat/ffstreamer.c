/*
 * ffstreamer: reads an input whose every byte a parent process serves over
 * two inherited pipes, by offset. See ffstreamer-specification.md in the
 * ffmpeg-ffstreamer repository, version 1.
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

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/thread.h"

#include "avformat.h"
#include "url.h"

#define FFS_VERSION_LOWEST   1
#define FFS_VERSION_HIGHEST  1

#define FFS_HEADER_BYTES     6
#define FFS_MAX_SENT_FRAME   4096
#define FFS_MAX_OUTSTANDING  16
#define FFS_WAIT_SLICE_MS    100
#define FFS_MESSAGE_MAX      1024

/* What this side offers in CAPABILITY. */
#define FFS_PREFERRED_BLOCK  (1 << 20)
#define FFS_LARGEST_BODY     (4 << 20)
#define FFS_OUTSTANDING_CAP  2

/* The largest file size a REQUEST may give (the specification's Checks Without Overflow): headroom, so the size plus a block, a
 * READ or a seek from the end stays below INT64_MAX. */
#define FFS_LARGEST_FILE     (INT64_C(1) << 62)

enum FFStreamerType {
    FFS_HELLO      = 1,
    FFS_CAPABILITY = 2,
    FFS_REQUEST    = 3,
    FFS_READ       = 4,
    FFS_DATA       = 5,
    FFS_DONE       = 6,
};

enum FFStreamerKind {
    FFS_KIND_TERMS       = 1,
    FFS_KIND_CHANNEL     = 2,
    FFS_KIND_PROTOCOL    = 3,
    FFS_KIND_INTERRUPTED = 4,
};

typedef struct FFStreamerBlock {
    int64_t index;      /* the block's number in the file, or -1 when free */
    int32_t length;
    int64_t last_used;
    int     pending;    /* its READ is outstanding */
    uint8_t *data;
} FFStreamerBlock;

typedef struct FFStreamerOutstanding {
    int8_t  sequence;
    int64_t offset;
    int32_t length;
    int     slot;
} FFStreamerOutstanding;

typedef struct FFStreamerContext {
    const AVClass *class;
    int read_fd;
    int write_fd;
    int cache_blocks;

    /* The terms, from REQUEST. */
    int     accepted;
    int64_t run;
    int64_t size;
    int32_t block;
    int32_t longest;
    int8_t  outstanding_allowed;

    AVMutex lock;       /* over numbering and writing a frame */
    int8_t  next_sequence;

    FFStreamerBlock *blocks;
    int64_t use_clock;

    FFStreamerOutstanding outstanding[FFS_MAX_OUTSTANDING];
    int outstanding_first;
    int outstanding_count;

    int64_t position;
    int64_t last_needed;

    int fault;          /* a sticky AVERROR, 0 while healthy */
    int fault_kind;
    int done_sent;
} FFStreamerContext;

/* The channels claimed in this process: each opens once, for the process's life. */
#define FFS_MAX_CLAIMS 64
static AVMutex claims_lock = AV_MUTEX_INITIALIZER;
static int claimed_read_fds[FFS_MAX_CLAIMS];
static int claimed_count;

static int claim_channel(int read_fd)
{
    int ret = 0;

    ff_mutex_lock(&claims_lock);
    for (int i = 0; i < claimed_count; i++) {
        if (claimed_read_fds[i] == read_fd) {
            ret = AVERROR(EBUSY);
            goto end;
        }
    }
    if (claimed_count == FFS_MAX_CLAIMS) {
        ret = AVERROR(EMFILE);
        goto end;
    }
    claimed_read_fds[claimed_count++] = read_fd;
end:
    ff_mutex_unlock(&claims_lock);
    return ret;
}

static int8_t next_sequence(int8_t sequence)
{
    return (int8_t)((sequence + 1) % 128);
}

/* --- Writing --------------------------------------------------------------- */

/* One write() of a whole frame, with SIGPIPE consumed rather than delivered,
 * whatever the program's disposition. */
static int write_frame_locked(FFStreamerContext *c, uint8_t type,
                              const uint8_t *body, int32_t body_length,
                              int8_t *sequence_out)
{
    uint8_t frame[FFS_MAX_SENT_FRAME];
    sigset_t pipe_set, old_set, pending;
    int was_pending, ret = 0;
    ssize_t written;

    if (body_length < 0 || body_length > FFS_MAX_SENT_FRAME - FFS_HEADER_BYTES) {
        ret = AVERROR(EINVAL);
        goto end;
    }

    frame[0] = type;
    frame[1] = (uint8_t)c->next_sequence;
    AV_WL32(frame + 2, (uint32_t)body_length);
    if (body_length)
        memcpy(frame + FFS_HEADER_BYTES, body, body_length);

    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    sigemptyset(&pending);
    sigpending(&pending);
    was_pending = sigismember(&pending, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);

    do {
        written = write(c->write_fd, frame, FFS_HEADER_BYTES + body_length);
    } while (written < 0 && errno == EINTR);

    if (written < 0) {
        ret = AVERROR(errno);
        if (errno == EPIPE && !was_pending) {
            struct timespec zero = { 0, 0 };
            sigtimedwait(&pipe_set, NULL, &zero);
        }
        goto restore_mask;
    }
    if (written != FFS_HEADER_BYTES + body_length) {
        /* A pipe takes a frame this small whole or not at all. */
        ret = AVERROR(EIO);
        goto restore_mask;
    }
    if (sequence_out)
        *sequence_out = c->next_sequence;
    c->next_sequence = next_sequence(c->next_sequence);
restore_mask:
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
end:
    return ret;
}

static int write_frame(FFStreamerContext *c, uint8_t type, const uint8_t *body,
                       int32_t body_length, int8_t *sequence_out)
{
    int ret;

    ff_mutex_lock(&c->lock);
    ret = write_frame_locked(c, type, body, body_length, sequence_out);
    ff_mutex_unlock(&c->lock);
    return ret;
}

/* --- Reading ---------------------------------------------------------------- */

/* Reads exactly length bytes, waiting as long as the parent likes, in slices,
 * checking FFmpeg's interrupt between them. End-of-file is the channel ending. */
static int read_exact(URLContext *h, uint8_t *buffer, int64_t length)
{
    FFStreamerContext *c = h->priv_data;
    int64_t got = 0;
    int ret = 0;

    while (got < length) {
        struct pollfd waiting = { .fd = c->read_fd, .events = POLLIN };
        int ready = poll(&waiting, 1, FFS_WAIT_SLICE_MS);
        size_t want;
        ssize_t n;

        if (ready < 0 && errno != EINTR) {
            ret = AVERROR(errno);
            goto end;
        }
        if (ready <= 0) {
            /* A slice passed, or a signal came: see whether FFmpeg wants to stop. */
            if (ff_check_interrupt(&h->interrupt_callback)) {
                ret = AVERROR_EXIT;
                goto end;
            }
            continue;
        }
        want = (size_t)FFMIN(length - got, INT_MAX);
        n = read(c->read_fd, buffer + got, want);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            ret = AVERROR(errno);
            goto end;
        }
        if (n == 0) {
            ret = AVERROR_EOF;
            goto end;
        }
        got += n;
    }
end:
    return ret;
}

/* Reads a header and checks its type, sequence and exact or bounded length. */
static int read_header(URLContext *h, uint8_t expected_type, int8_t expected_sequence,
                       int32_t expected_length, int32_t *length_out)
{
    uint8_t header[FFS_HEADER_BYTES];
    int32_t length;
    int ret;

    if ((ret = read_exact(h, header, FFS_HEADER_BYTES)) < 0)
        goto end;
    length = (int32_t)AV_RL32(header + 2);
    if (header[0] != expected_type || (int8_t)header[1] != expected_sequence ||
        (int8_t)header[1] < 0 || length < 0 ||
        (expected_length >= 0 && length != expected_length)) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: the parent sent type %d, number %d, length %d; "
               "expected type %d, number %d, length %d\n", header[0], (int8_t)header[1],
               length, expected_type, expected_sequence, expected_length);
        ret = AVERROR(EPROTO);
        goto end;
    }
    if (length_out)
        *length_out = length;
end:
    return ret;
}

/* --- Faults and DONE ------------------------------------------------------- */

static int kind_of(int error)
{
    if (error == AVERROR_EXIT)
        return FFS_KIND_INTERRUPTED;
    if (error == AVERROR(EPROTO))
        return FFS_KIND_PROTOCOL;
    return FFS_KIND_CHANNEL;
}

/* Records the first fault; every later read returns it. The channel ending is
 * an I/O error, never end-of-file: a parent that died must not look like the
 * end of the film. */
static int set_fault(FFStreamerContext *c, int error)
{
    if (error == AVERROR_EOF)
        error = AVERROR(EIO);
    if (!c->fault) {
        c->fault = error;
        c->fault_kind = kind_of(error);
    }
    return c->fault;
}

static void send_done(URLContext *h, int kind, int error)
{
    FFStreamerContext *c = h->priv_data;
    uint8_t body[16 + FFS_MESSAGE_MAX];
    int32_t length = 9;

    if (c->done_sent)
        return;
    c->done_sent = 1;

    AV_WL64(body, (uint64_t)(c->run >= 1 ? c->run : 0));
    if (!kind) {
        body[8] = 0;
    } else {
        char message[FFS_MESSAGE_MAX + 1] = "";
        int32_t code = (error < 0 && error != INT_MIN) ? -error : 0;
        size_t message_length;
        av_strerror(error, message, sizeof(message));
        message_length = av_strnlen(message, FFS_MESSAGE_MAX);
        body[8] = 1;
        body[9] = (uint8_t)kind;
        AV_WL32(body + 10, (uint32_t)code);
        AV_WL16(body + 14, (uint16_t)message_length);
        memcpy(body + 16, message, message_length);
        length = 16 + (int32_t)message_length;
    }
    if (write_frame(c, FFS_DONE, body, length, NULL) < 0)
        av_log(h, AV_LOG_WARNING, "ffstreamer: DONE could not be sent; the parent has gone\n");
}

/* --- Opening --------------------------------------------------------------- */

static int check_descriptor(URLContext *h, int fd, int access_mode)
{
    struct stat facts;
    int flags, ret = 0;

    if (fstat(fd, &facts) < 0 || !S_ISFIFO(facts.st_mode)) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: descriptor %d is not a pipe\n", fd);
        ret = AVERROR(EBADF);
        goto end;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != access_mode) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: descriptor %d is not open for its direction\n", fd);
        ret = AVERROR(EBADF);
        goto end;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        ret = AVERROR(errno);
end:
    return ret;
}

static int exchange_hellos(URLContext *h)
{
    FFStreamerContext *c = h->priv_data;
    uint8_t body[4];
    int16_t lowest, highest;
    int ret;

    AV_WL16(body, FFS_VERSION_LOWEST);
    AV_WL16(body + 2, FFS_VERSION_HIGHEST);
    if ((ret = write_frame(c, FFS_HELLO, body, 4, NULL)) < 0)
        goto end;
    if ((ret = read_header(h, FFS_HELLO, 0, 4, NULL)) < 0)
        goto end;
    if ((ret = read_exact(h, body, 4)) < 0)
        goto end;
    lowest = (int16_t)AV_RL16(body);
    highest = (int16_t)AV_RL16(body + 2);
    if (lowest < 1 || highest < lowest ||
        FFMIN(highest, FFS_VERSION_HIGHEST) < FFMAX(lowest, FFS_VERSION_LOWEST)) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: the parent speaks versions %d to %d, this side %d to %d\n",
               lowest, highest, FFS_VERSION_LOWEST, FFS_VERSION_HIGHEST);
        ret = AVERROR(EPROTO);
        goto end;
    }
    ret = FFMIN(highest, FFS_VERSION_HIGHEST);
end:
    return ret;
}

/* Returns 0 for good terms, a negative AVERROR for a malformed REQUEST (no DONE),
 * or 1 for well-formed terms that break the rules (DONE TERMS). */
static int receive_request(URLContext *h, int version, int8_t capability_sequence)
{
    FFStreamerContext *c = h->priv_data;
    uint8_t body[27];
    int16_t request_version;
    int ret;

    if ((ret = read_header(h, FFS_REQUEST, capability_sequence, 27, NULL)) < 0)
        goto end;
    if ((ret = read_exact(h, body, 27)) < 0)
        goto end;

    request_version         = (int16_t)AV_RL16(body);
    c->run                  = (int64_t)AV_RL64(body + 2);
    c->size                 = (int64_t)AV_RL64(body + 10);
    c->block                = (int32_t)AV_RL32(body + 18);
    c->longest              = (int32_t)AV_RL32(body + 22);
    c->outstanding_allowed  = (int8_t)body[26];

    if (request_version < 0 || c->run < 0 || c->size < 0 || c->block < 0 ||
        c->longest < 0 || c->outstanding_allowed < 0) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: REQUEST has a negative field\n");
        ret = AVERROR(EPROTO);
        goto end;
    }
    if (request_version != version || c->run < 1 || c->size > FFS_LARGEST_FILE ||
        c->block < 1 || c->block > FFS_LARGEST_BODY ||
        c->longest < c->block || c->longest > FFS_LARGEST_BODY ||
        c->outstanding_allowed < 1 || c->outstanding_allowed > FFS_OUTSTANDING_CAP) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: REQUEST's terms are outside what this side offered\n");
        ret = 1;
        goto end;
    }
    ret = 0;
end:
    return ret;
}

static void close_descriptors(FFStreamerContext *c)
{
    if (c->read_fd >= 0)
        close(c->read_fd);
    if (c->write_fd >= 0)
        close(c->write_fd);
    c->read_fd = c->write_fd = -1;
}

static void free_cache(FFStreamerContext *c)
{
    if (c->blocks) {
        for (int i = 0; i < c->cache_blocks; i++)
            av_freep(&c->blocks[i].data);
        av_freep(&c->blocks);
    }
}

static int ffstreamer_open(URLContext *h, const char *url, int flags)
{
    FFStreamerContext *c = h->priv_data;
    uint8_t body[9];
    int8_t capability_sequence;
    int version, ret;

    /* Until the channel is claimed, its descriptors are not ours to close. */
    if (strcmp(url, "ffstreamer:")) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: the URL must be exactly \"ffstreamer:\"\n");
        ret = AVERROR(EINVAL);
        goto unclaimed;
    }
    if (flags & AVIO_FLAG_WRITE) {
        ret = AVERROR(EINVAL);
        goto unclaimed;
    }
    if (c->read_fd < 0 || c->write_fd < 0) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: -ffstreamer_read_fd and -ffstreamer_write_fd are required\n");
        ret = AVERROR(EINVAL);
        goto unclaimed;
    }
    if ((ret = claim_channel(c->read_fd)) < 0) {
        av_log(h, AV_LOG_ERROR, "ffstreamer: the channel on descriptor %d was opened before\n", c->read_fd);
        goto unclaimed;
    }
    if ((ret = check_descriptor(h, c->read_fd, O_RDONLY)) < 0 ||
        (ret = check_descriptor(h, c->write_fd, O_WRONLY)) < 0)
        goto fail;

    c->next_sequence = 0;
    c->last_needed = -2;
    if ((version = exchange_hellos(h)) < 0) {
        ret = version;
        goto fail;
    }

    AV_WL32(body, FFS_PREFERRED_BLOCK);
    AV_WL32(body + 4, FFS_LARGEST_BODY);
    body[8] = FFS_OUTSTANDING_CAP;
    if ((ret = write_frame(c, FFS_CAPABILITY, body, 9, &capability_sequence)) < 0)
        goto fail;

    ret = receive_request(h, version, capability_sequence);
    if (ret < 0)
        goto fail;
    if (ret == 1) {
        send_done(h, FFS_KIND_TERMS, AVERROR(EINVAL));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (c->cache_blocks < c->outstanding_allowed + 1)
        c->cache_blocks = c->outstanding_allowed + 1;
    c->blocks = av_calloc(c->cache_blocks, sizeof(*c->blocks));
    if (!c->blocks)
        goto out_of_memory;
    for (int i = 0; i < c->cache_blocks; i++) {
        c->blocks[i].index = -1;
        c->blocks[i].data = av_malloc(c->block);
        if (!c->blocks[i].data)
            goto out_of_memory;
    }

    c->accepted = 1;
    h->is_streamed = 0;
    ret = 0;
    goto end;

out_of_memory:
    send_done(h, FFS_KIND_TERMS, AVERROR(ENOMEM));
    ret = AVERROR(ENOMEM);
fail:
    if (ret == AVERROR_EOF)
        ret = AVERROR(EIO);
    /* FFmpeg never calls the close of an open that failed. */
    free_cache(c);
    close_descriptors(c);
    goto end;
unclaimed:
    c->read_fd = c->write_fd = -1;
end:
    return ret;
}

/* --- The block cache ----------------------------------------------------- */

static int find_block(FFStreamerContext *c, int64_t index)
{
    for (int i = 0; i < c->cache_blocks; i++)
        if (c->blocks[i].index == index)
            return i;
    return -1;
}

/* The least recently used block that no outstanding READ will fill. */
static int free_slot(FFStreamerContext *c)
{
    int chosen = -1;
    for (int i = 0; i < c->cache_blocks; i++) {
        if (c->blocks[i].pending)
            continue;
        if (c->blocks[i].index < 0)
            return i;
        if (chosen < 0 || c->blocks[i].last_used < c->blocks[chosen].last_used)
            chosen = i;
    }
    return chosen;
}

static int32_t block_length(FFStreamerContext *c, int64_t index)
{
    int64_t start = index * c->block;
    return (int32_t)FFMIN((int64_t)c->block, c->size - start);
}

static int send_read(URLContext *h, int64_t index)
{
    FFStreamerContext *c = h->priv_data;
    FFStreamerOutstanding *entry;
    uint8_t body[12];
    int slot = free_slot(c), ret;
    int32_t length = block_length(c, index);
    int8_t sequence;

    if (slot < 0) {
        ret = AVERROR_BUG;      /* the cache is larger than what may be outstanding */
        goto end;
    }
    AV_WL64(body, (uint64_t)(index * c->block));
    AV_WL32(body + 8, (uint32_t)length);
    if ((ret = write_frame(c, FFS_READ, body, 12, &sequence)) < 0) {
        ret = set_fault(c, ret);
        goto end;
    }

    c->blocks[slot].index = index;
    c->blocks[slot].length = length;
    c->blocks[slot].pending = 1;
    entry = &c->outstanding[(c->outstanding_first + c->outstanding_count) % FFS_MAX_OUTSTANDING];
    entry->sequence = sequence;
    entry->offset = index * c->block;
    entry->length = length;
    entry->slot = slot;
    c->outstanding_count++;
end:
    return ret;
}

/* Takes the answer to the oldest outstanding READ into its block. */
static int receive_data(URLContext *h)
{
    FFStreamerContext *c = h->priv_data;
    FFStreamerOutstanding *oldest = &c->outstanding[c->outstanding_first];
    int ret;

    if ((ret = read_header(h, FFS_DATA, oldest->sequence, oldest->length, NULL)) < 0)
        goto fault;
    /* The slot is freed no earlier than reading the DATA header (the specification's READ). */
    c->outstanding_first = (c->outstanding_first + 1) % FFS_MAX_OUTSTANDING;
    c->outstanding_count--;
    if ((ret = read_exact(h, c->blocks[oldest->slot].data, oldest->length)) < 0)
        goto fault;
    c->blocks[oldest->slot].pending = 0;
    c->blocks[oldest->slot].last_used = ++c->use_clock;
    ret = 0;
    goto end;
fault:
    ret = set_fault(c, ret);
end:
    return ret;
}

static int is_outstanding(FFStreamerContext *c, int64_t index)
{
    for (int i = 0; i < c->outstanding_count; i++)
        if (c->outstanding[(c->outstanding_first + i) % FFS_MAX_OUTSTANDING].offset == index * c->block)
            return 1;
    return 0;
}

static int ffstreamer_read(URLContext *h, unsigned char *buf, int size)
{
    FFStreamerContext *c = h->priv_data;
    int64_t index, blocks_in_file, within;
    int slot, ret;

    if (c->fault) {
        ret = c->fault;
        goto end;
    }
    if (c->done_sent) {
        ret = AVERROR(EIO);
        goto end;
    }
    if (c->position >= c->size) {
        ret = AVERROR_EOF;
        goto end;
    }

    index = c->position / c->block;
    /* Rounded up without adding: a size near INT64_MAX must not overflow. */
    blocks_in_file = c->size / c->block + (c->size % c->block != 0);

    if ((slot = find_block(c, index)) < 0) {
        while (c->outstanding_count >= c->outstanding_allowed)
            if ((ret = receive_data(h)) < 0)
                goto end;
        if ((slot = find_block(c, index)) < 0 && (ret = send_read(h, index)) < 0)
            goto end;
    }

    /* The block about to be read is the most recently used, so a prefetch never evicts it. */
    if (slot >= 0)
        c->blocks[slot].last_used = ++c->use_clock;

    /* Prefetch when reading straight through. */
    if (index == c->last_needed + 1 && index + 1 < blocks_in_file &&
        find_block(c, index + 1) < 0 && !is_outstanding(c, index + 1) &&
        c->outstanding_count < c->outstanding_allowed && free_slot(c) >= 0) {
        if ((ret = send_read(h, index + 1)) < 0)
            goto end;
    }

    while ((slot = find_block(c, index)) < 0 || c->blocks[slot].pending)
        if ((ret = receive_data(h)) < 0)
            goto end;

    c->last_needed = index;
    c->blocks[slot].last_used = ++c->use_clock;
    within = c->position - index * c->block;
    ret = (int)FFMIN((int64_t)size, c->blocks[slot].length - within);
    memcpy(buf, c->blocks[slot].data + within, ret);
    c->position += ret;
end:
    return ret;
}

static int64_t ffstreamer_seek(URLContext *h, int64_t position, int whence)
{
    FFStreamerContext *c = h->priv_data;
    int64_t target, base;

    switch (whence) {
    case AVSEEK_SIZE: target = c->size; goto end;
    case SEEK_SET:    target = position; break;
    case SEEK_CUR:    base = c->position; break;
    case SEEK_END:    base = c->size; break;
    default:          target = AVERROR(EINVAL); goto end;
    }
    if (whence != SEEK_SET) {
        /* base is never negative, so only a sum past INT64_MAX can overflow. */
        if (position > INT64_MAX - base) {
            target = AVERROR(EINVAL);
            goto end;
        }
        target = base + position;
    }
    if (target < 0) {
        target = AVERROR(EINVAL);
        goto end;
    }
    c->position = target;
end:
    return target;
}

static int ffstreamer_close(URLContext *h)
{
    FFStreamerContext *c = h->priv_data;

    if (c->accepted)
        send_done(h, c->fault ? c->fault_kind : 0, c->fault);
    free_cache(c);
    close_descriptors(c);
    return 0;
}

#define OFFSET(x) offsetof(FFStreamerContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
static const AVOption ffstreamer_options[] = {
    { "ffstreamer_read_fd",      "the descriptor the parent's messages arrive on", OFFSET(read_fd),      AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, D },
    { "ffstreamer_write_fd",     "the descriptor this side's messages go out on",  OFFSET(write_fd),     AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, D },
    { "ffstreamer_cache_blocks", "how many blocks to keep",                         OFFSET(cache_blocks), AV_OPT_TYPE_INT, { .i64 = 8 },   2,  1024,    D },
    { NULL }
};

static const AVClass ffstreamer_class = {
    .class_name = "ffstreamer",
    .item_name  = av_default_item_name,
    .option     = ffstreamer_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int ffstreamer_init(URLContext *h, const char *url, int flags)
{
    FFStreamerContext *c = h->priv_data;
    int ret;

    if ((ret = ff_mutex_init(&c->lock, NULL))) {
        ret = AVERROR(ret);
        goto end;
    }
    if ((ret = ffstreamer_open(h, url, flags)) < 0)
        ff_mutex_destroy(&c->lock);
end:
    return ret;
}

static int ffstreamer_close_all(URLContext *h)
{
    FFStreamerContext *c = h->priv_data;
    int ret = ffstreamer_close(h);
    ff_mutex_destroy(&c->lock);
    return ret;
}

const URLProtocol ff_ffstreamer_protocol = {
    .name                = "ffstreamer",
    .url_open            = ffstreamer_init,
    .url_read            = ffstreamer_read,
    .url_seek            = ffstreamer_seek,
    .url_close           = ffstreamer_close_all,
    .priv_data_size      = sizeof(FFStreamerContext),
    .priv_data_class     = &ffstreamer_class,
};
