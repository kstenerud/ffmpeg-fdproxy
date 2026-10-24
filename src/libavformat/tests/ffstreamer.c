/*
 * Tests the ffstreamer protocol through the public avio API, against a parent
 * served by a thread over two pipes: a file read through, by seeks and at
 * random through a cache of two blocks; a parent that leaves, stalls, numbers
 * a DATA wrongly or stops reading; terms the protocol refuses; descriptors
 * that are no pipe; and a pipe that serves one session at a time.
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

#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavformat/avio.h"
#include "libavformat/url.h"

enum {
    HELLO = 1, CAPABILITY = 2, REQUEST = 3, READ = 4, DATA = 5, DONE = 6,
};

#define FILE_SIZE  100003
#define BLOCK      4096

/** How the parent behaves once it has answered its first READs. */
enum Behaviour {
    ANSWERS,        ///< answers every READ
    LEAVES,         ///< closes its pipe to the protocol
    MISNUMBERS,     ///< numbers its next DATA wrongly
    STALLS,         ///< answers nothing more, and waits
    STOPS_READING,  ///< closes both its pipes, without reading DONE
    TRICKLES,       ///< sends its next DATA a byte at a time
};

/** The parent's side of one session, which serve() runs in a thread. */
typedef struct Parent {
    int from_child;     ///< where the protocol's frames arrive
    int to_child;       ///< where the parent's frames go
    const uint8_t *file;
    int64_t size;
    int32_t block;
    enum Behaviour behaviour;
    int answers;        ///< READs answered before behaving so
    int done_result;    ///< DONE's result: -1 when none came
    int done_kind;      ///< DONE's error kind, for an ERR
    int violation;      ///< a frame broke the specification
    volatile int *stalled; ///< set once it stalls: the interrupt's flag
} Parent;

static int read_all(int fd, uint8_t *buffer, int length)
{
    int done = 0;

    while (done < length) {
        ssize_t got = read(fd, buffer + done, length - done);
        if (got <= 0)
            return -1;
        done += got;
    }
    return 0;
}

static int write_frame(int fd, int type, int sequence, const uint8_t *body,
                       int length)
{
    uint8_t header[6];

    header[0] = type;
    header[1] = sequence;
    AV_WL32(header + 2, length);
    if (write(fd, header, 6) != 6)
        return -1;
    if (length && write(fd, body, length) != length)
        return -1;
    return 0;
}

/**
 * Reads the protocol's next frame into body, checking its sequence number.
 * Returns its type, or -1 at the end of the pipe or on a violation.
 */
static int next_frame(Parent *p, int *expected, int *sequence, uint8_t *body,
                      int *length)
{
    uint8_t header[6];

    if (read_all(p->from_child, header, 6) < 0)
        return -1;
    *sequence = header[1];
    *length   = AV_RL32(header + 2);
    if (*sequence != *expected || *length < 0 || *length > 1040 ||
        read_all(p->from_child, body, *length) < 0) {
        p->violation = 1;
        return -1;
    }
    *expected = (*expected + 1) % 128;
    return header[0];
}

/** The opening exchange: HELLO for HELLO, then REQUEST for CAPABILITY. */
static int open_session(Parent *p, int *expected)
{
    uint8_t body[1040], terms[27];
    int sequence, length;

    if (next_frame(p, expected, &sequence, body, &length) != HELLO)
        return -1;
    AV_WL16(body, 1);
    AV_WL16(body + 2, 1);
    if (write_frame(p->to_child, HELLO, 0, body, 4) < 0 ||
        next_frame(p, expected, &sequence, body, &length) != CAPABILITY)
        return -1;
    AV_WL16(terms, 1);              /* version */
    AV_WL64(terms + 2, 7);          /* run */
    AV_WL64(terms + 10, p->size);
    AV_WL32(terms + 18, p->block);
    AV_WL32(terms + 22, p->block);  /* longest */
    terms[26] = 2;                  /* outstanding */
    return write_frame(p->to_child, REQUEST, sequence, terms, 27);
}

/** The parent: the opening exchange, then a DATA for each READ, as told. */
static void *serve(void *opaque)
{
    Parent *p = opaque;
    uint8_t body[1040];
    int expected = 0, sequence, length, type, answered = 0;

    if (open_session(p, &expected) < 0)
        goto end;
    while ((type = next_frame(p, &expected, &sequence, body, &length)) > 0) {
        int64_t offset;
        int32_t wanted;
        int misbehaves = p->behaviour != ANSWERS && answered >= p->answers;

        if (type == DONE) {
            p->done_result = length >= 9 ? body[8] : -1;
            p->done_kind   = length >= 10 ? body[9] : 0;
            continue;
        }
        if (type != READ || length != 12) {
            p->violation = 1;
            break;
        }
        offset = AV_RL64(body);
        wanted = AV_RL32(body + 8);
        if (offset < 0 || wanted <= 0 || wanted > p->block ||
            wanted > p->size - offset) {
            p->violation = 1;
            break;
        }
        if (misbehaves && p->behaviour == STOPS_READING) {
            /* The protocol sees its channel end, and its DONE meets a
             * pipe no one reads: the one it writes is closed first, so
             * no write of its lands before the close. */
            close(p->from_child);
            close(p->to_child);
            p->to_child = p->from_child = -1;
            return NULL;
        }
        if (misbehaves && p->behaviour == LEAVES && p->to_child >= 0) {
            close(p->to_child);
            p->to_child = -1;
        }
        if (misbehaves && p->behaviour == MISNUMBERS)
            sequence = (sequence + 1) % 128;
        if (misbehaves && p->behaviour == STALLS && p->stalled)
            *p->stalled = 1;
        if (misbehaves && p->behaviour == TRICKLES && p->to_child >= 0) {
            /* Interrupted once its first byte is sent, the protocol must
             * not wait out the rest, which never stops coming. */
            uint8_t header[6] = { DATA, sequence };

            AV_WL32(header + 2, wanted);
            if (write(p->to_child, header, 6) != 6)
                wanted = -1;
            for (int i = 0; i < wanted; i++) {
                if (write(p->to_child, p->file + offset + i, 1) != 1)
                    break;
                if (p->stalled)
                    *p->stalled = 1;
                usleep(5000);
            }
            close(p->to_child);
            p->to_child = -1;
            continue;
        }
        if (p->to_child < 0 || (misbehaves && p->behaviour == STALLS))
            continue;
        if (write_frame(p->to_child, DATA, sequence, p->file + offset,
                        wanted) < 0) {
            /* The protocol closed while this READ, a prefetch, was
             * outstanding: its DONE is still to be read. */
            close(p->to_child);
            p->to_child = -1;
            continue;
        }
        answered++;
    }
end:
    /* Read on to the end, so a DONE after a violation is not lost. */
    while (read(p->from_child, body, sizeof(body)) > 0)
        ;
    return NULL;
}

/** One session's pipes, its parent's thread and the descriptors' options. */
typedef struct Session {
    Parent parent;
    pthread_t thread;
    int child_read, child_write;
    AVDictionary *options;
} Session;

static int start(Session *s, const uint8_t *file, int32_t block,
                 enum Behaviour behaviour, int answers)
{
    int to_child[2], from_child[2];

    memset(s, 0, sizeof(*s));
    if (pipe(to_child) < 0)
        return -1;
    if (pipe(from_child) < 0) {
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }
    s->child_read  = to_child[0];
    s->child_write = from_child[1];
    s->parent = (Parent){
        .from_child  = from_child[0], .to_child  = to_child[1],
        .file        = file,          .size      = FILE_SIZE,
        .block       = block,         .behaviour = behaviour,
        .answers     = answers,       .done_result = -1,
    };
    av_dict_set_int(&s->options, "read_fd", s->child_read, 0);
    av_dict_set_int(&s->options, "write_fd", s->child_write, 0);
    if (pthread_create(&s->thread, NULL, serve, &s->parent)) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        av_dict_free(&s->options);
        return -1;
    }
    return 0;
}

/**
 * Waits for the parent, which ends when the protocol's session ended, and
 * closes the descriptors the session was given, which stay the caller's.
 */
static void finish(Session *s)
{
    close(s->child_read);
    close(s->child_write);
    pthread_join(s->thread, NULL);
    if (s->parent.from_child >= 0)
        close(s->parent.from_child);
    if (s->parent.to_child >= 0)
        close(s->parent.to_child);
    av_dict_free(&s->options);
}

static void print_done(const Parent *p)
{
    if (!p->done_result)
        printf("DONE: ok\n");
    else if (p->done_result == 1)
        printf("DONE: err, kind %d\n", p->done_kind);
    else
        printf("DONE: none\n");
    if (p->violation)
        printf("the protocol broke the specification\n");
}

static const char *yes(int is_so)
{
    return is_so ? "yes" : "no";
}

/** Whether a descriptor is open: the caller's, after a session. */
static int is_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

/** How many descriptors are open, to see the protocol keeps none of its own. */
static int open_count(void)
{
    int count = 0;

    for (int fd = 0; fd < 1024; fd++)
        count += is_open(fd);
    return count;
}

static int read_through(AVIOContext *pb, uint8_t *buffer, int size)
{
    int done = 0, ret;

    while (done < size &&
           (ret = avio_read(pb, buffer + done, size - done)) > 0)
        done += ret;
    return done;
}

static void test_read(const uint8_t *file, uint8_t *buffer)
{
    AVIOContext *pb = NULL;
    Session s;
    int ret, before;

    printf("read:\n");
    if (start(&s, file, BLOCK, ANSWERS, 0) < 0) {
        printf("cannot start\n");
        return;
    }
    /* Given non-blocking, which the protocol leaves as it is and copes
     * with. */
    fcntl(s.child_read, F_SETFL, fcntl(s.child_read, F_GETFL) | O_NONBLOCK);
    fcntl(s.child_write, F_SETFL, fcntl(s.child_write, F_GETFL) | O_NONBLOCK);
    before = open_count();
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, NULL, &s.options);
    printf("open: %d\n", ret);
    if (ret >= 0) {
        printf("their flags left as given: %s\n",
               yes((fcntl(s.child_read, F_GETFL) & O_NONBLOCK) &&
                   (fcntl(s.child_write, F_GETFL) & O_NONBLOCK)));
        printf("size: %"PRId64"\n", avio_size(pb));
        ret = read_through(pb, buffer, FILE_SIZE);
        printf("read %d bytes, as served: %s\n", ret,
               yes(ret == FILE_SIZE && !memcmp(buffer, file, FILE_SIZE)));
        printf("seek to 50000: %"PRId64"\n", avio_seek(pb, 50000, SEEK_SET));
        ret = read_through(pb, buffer, 3000);
        printf("read %d bytes, as served: %s\n", ret,
               yes(!memcmp(buffer, file + 50000, 3000)));
        printf("seek to 10 before the end: %"PRId64"\n",
               avio_seek(pb, FILE_SIZE - 10, SEEK_SET));
        ret = read_through(pb, buffer, 100);
        printf("read %d bytes, as served: %s, at the end: %d\n", ret,
               yes(!memcmp(buffer, file + FILE_SIZE - 10, 10)), avio_feof(pb));
        avio_closep(&pb);
    }
    printf("its descriptors left open, and none of its own: %s\n",
           yes(is_open(s.child_read) && is_open(s.child_write) &&
               open_count() == before));
    finish(&s);
    print_done(&s.parent);
}

/**
 * Reads at random through a cache of two blocks, read directly, so every
 * read reaches the protocol and blocks are evicted all the time.
 */
static void test_random_reads(const uint8_t *file, uint8_t *buffer)
{
    AVIOContext *pb = NULL;
    Session s;
    uint32_t random = 1;
    int ret, matched = 0;

    printf("random reads through a cache of two blocks:\n");
    if (start(&s, file, 1000, ANSWERS, 0) < 0) {
        printf("cannot start\n");
        return;
    }
    av_dict_set_int(&s.options, "cache_blocks", 2, 0);
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ | AVIO_FLAG_DIRECT,
                     NULL, &s.options);
    printf("open: %d\n", ret);
    if (ret >= 0) {
        for (int i = 0; i < 300; i++) {
            int64_t offset;
            int length;

            random = random * 1664525 + 1013904223;
            offset = random % FILE_SIZE;
            random = random * 1664525 + 1013904223;
            length = FFMIN(1 + random % 2500, FILE_SIZE - offset);
            if (avio_seek(pb, offset, SEEK_SET) != offset)
                break;
            if (read_through(pb, buffer, length) == length &&
                !memcmp(buffer, file + offset, length))
                matched++;
        }
        printf("300 reads, as served: %d\n", matched);
        avio_closep(&pb);
    }
    finish(&s);
    print_done(&s.parent);
}

static int is_interrupted(void *opaque)
{
    return *(volatile int *)opaque;
}

/** A parent that misbehaves after answering some READs. */
static void test_misbehaving(const uint8_t *file, uint8_t *buffer,
                             const char *what, enum Behaviour behaviour,
                             int answers)
{
    volatile int stalled = 0;
    AVIOInterruptCB interrupt = { is_interrupted, (void *)&stalled };
    AVIOContext *pb = NULL;
    Session s;
    int ret;

    printf("%s:\n", what);
    if (start(&s, file, BLOCK, behaviour, answers) < 0) {
        printf("cannot start\n");
        return;
    }
    /* A stalling parent interrupts the protocol once it stalls, while the
     * protocol can only be waiting for its DATA. */
    s.parent.stalled = &stalled;
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, &interrupt,
                     &s.options);
    printf("open: %d\n", ret);
    if (ret >= 0) {
        ret = read_through(pb, buffer, FILE_SIZE);
        printf("read %d bytes\n", ret);
        printf("then failed: %s, interrupted: %s\n",
               yes(pb->error < 0 && pb->error != AVERROR_EOF),
               yes(pb->error == AVERROR_EXIT));
        if (behaviour == MISNUMBERS) {
            /* The fault is sticky: the first block, in the cache, is not
             * served after it, straight from the protocol either. */
            URLContext *h = pb->opaque;
            int64_t back = ffurl_seek(h, 0, SEEK_SET);

            printf("a read after it fails too: %s\n",
                   yes(back < 0 || ffurl_read(h, buffer, 10) < 0));
        }
        avio_closep(&pb);
    }
    finish(&s);
    print_done(&s.parent);
}

static void test_bad_terms(const uint8_t *file)
{
    AVIOContext *pb = NULL;
    Session s;
    int ret, before;

    printf("a REQUEST whose block is 0:\n");
    if (start(&s, file, 0, ANSWERS, 0) < 0) {
        printf("cannot start\n");
        return;
    }
    before = open_count();
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, NULL, &s.options);
    printf("open refused: %s\n", yes(ret == AVERROR(EINVAL)));
    printf("its descriptors left open, and none of its own: %s\n",
           yes(is_open(s.child_read) && is_open(s.child_write) &&
               open_count() == before));
    avio_closep(&pb);
    finish(&s);
    print_done(&s.parent);
}

/** Descriptors that are no pipe are refused, and left to the caller. */
static void test_no_pipe(void)
{
    AVDictionary *options = NULL;
    AVIOContext *pb = NULL;
    int read_fd = open("/dev/null", O_RDONLY);
    int write_fd = open("/dev/null", O_WRONLY);
    int ret;

    printf("descriptors that are no pipe:\n");
    av_dict_set_int(&options, "read_fd", read_fd, 0);
    av_dict_set_int(&options, "write_fd", write_fd, 0);
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, NULL, &options);
    printf("open refused: %s\n", yes(ret == AVERROR(EBADF)));
    printf("its descriptors left open: %s\n",
           yes(!close(read_fd) && !close(write_fd)));
    av_dict_free(&options);
}

/** Opens with the descriptors given, and says whether it was refused. */
static void refuse(const char *what, int read_fd, int write_fd, int error)
{
    AVDictionary *options = NULL;
    AVIOContext *pb = NULL;
    int ret;

    av_dict_set_int(&options, "read_fd", read_fd, 0);
    av_dict_set_int(&options, "write_fd", write_fd, 0);
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, NULL, &options);
    printf("%s: refused: %s\n", what, yes(ret == error));
    avio_closep(&pb);
    av_dict_free(&options);
}

/** Descriptors that cannot be a channel are refused, and left open. */
static void test_bad_descriptors(void)
{
    int channel[2];

    printf("descriptors that cannot be a channel:\n");
    if (pipe(channel) < 0) {
        printf("cannot start\n");
        return;
    }
    refuse("one descriptor for both", channel[0], channel[0], AVERROR(EINVAL));
    refuse("stdin", STDIN_FILENO, channel[1], AVERROR(EINVAL));
    refuse("each the other's direction", channel[1], channel[0],
           AVERROR(EBADF));
    printf("its descriptors left open: %s\n",
           yes(!close(channel[0]) && !close(channel[1])));
}

static int always(void *opaque)
{
    return 1;
}

/**
 * A pipe to the parent that is full, the parent reading none of it: the
 * protocol waits for room, and its interrupt ends the wait.
 */
static void test_full_pipe(void)
{
    AVIOInterruptCB interrupt = { always, NULL };
    AVDictionary *options = NULL;
    AVIOContext *pb = NULL;
    uint8_t filler[4096] = { 0 };
    int to_child[2], from_child[2], ret, before;

    printf("a pipe to the parent that is full:\n");
    if (pipe(to_child) < 0 || pipe(from_child) < 0) {
        printf("cannot start\n");
        return;
    }
    fcntl(from_child[1], F_SETFL, O_NONBLOCK);
    while (write(from_child[1], filler, sizeof(filler)) > 0)
        ;
    av_dict_set_int(&options, "read_fd", to_child[0], 0);
    av_dict_set_int(&options, "write_fd", from_child[1], 0);
    before = open_count();
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, &interrupt, &options);
    printf("open interrupted: %s\n", yes(ret == AVERROR_EXIT));
    printf("its descriptors left open, and none of its own: %s\n",
           yes(is_open(to_child[0]) && is_open(from_child[1]) &&
               open_count() == before));
    avio_closep(&pb);
    av_dict_free(&options);
    for (int i = 0; i < 2; i++) {
        close(to_child[i]);
        close(from_child[i]);
    }
}

static void test_one_session_a_pipe(const uint8_t *file, uint8_t *buffer)
{
    AVIOContext *first = NULL, *second = NULL;
    AVDictionary *options = NULL;
    Session s;
    int read_copy, write_copy, fresh[2], ret;

    printf("a second session on an open pipe:\n");
    if (start(&s, file, BLOCK, ANSWERS, 0) < 0) {
        printf("cannot start\n");
        return;
    }
    ret = avio_open2(&first, "ffstreamer:", AVIO_FLAG_READ, NULL, &s.options);
    printf("first open: %d\n", ret);
    read_copy  = dup(s.child_read);
    write_copy = dup(s.child_write);
    av_dict_set_int(&options, "read_fd", read_copy, 0);
    av_dict_set_int(&options, "write_fd", write_copy, 0);
    ret = avio_open2(&second, "ffstreamer:", AVIO_FLAG_READ, NULL, &options);
    printf("second open refused: %s\n", yes(ret == AVERROR(EBUSY)));
    printf("its descriptors left open: %s\n",
           yes(!close(read_copy) && !close(write_copy)));
    av_dict_free(&options);
    avio_closep(&second);
    /* A pipe of its own to read, but the open session's to write: refused
     * before it writes a frame into that session's. */
    if (pipe(fresh) < 0) {
        printf("cannot start\n");
        return;
    }
    write_copy = dup(s.child_write);
    av_dict_set_int(&options, "read_fd", fresh[0], 0);
    av_dict_set_int(&options, "write_fd", write_copy, 0);
    ret = avio_open2(&second, "ffstreamer:", AVIO_FLAG_READ, NULL, &options);
    printf("an open sharing only its write pipe refused: %s\n",
           yes(ret == AVERROR(EBUSY)));
    printf("its descriptors left open: %s\n",
           yes(!close(write_copy) && !close(fresh[0]) && !close(fresh[1])));
    av_dict_free(&options);
    avio_closep(&second);
    if (first)
        printf("the open session reads on: %s\n",
               yes(read_through(first, buffer, 5000) == 5000 &&
                   !memcmp(buffer, file, 5000)));
    avio_closep(&first);
    finish(&s);
    print_done(&s.parent);
}

int main(void)
{
    uint8_t *file = av_malloc(FILE_SIZE), *buffer = av_malloc(FILE_SIZE);

    if (!file || !buffer)
        return 1;
    for (int i = 0; i < FILE_SIZE; i++)
        file[i] = (i * 7) ^ (i >> 8);
    av_log_set_level(AV_LOG_QUIET);
    /* As every program using the protocol must, as ffmpeg and ffprobe do:
     * a write to a pipe whose reader has gone fails instead. */
    signal(SIGPIPE, SIG_IGN);
    /* A protocol that waits through its interrupt, or for a parent that
     * left, waits for ever: a minute ends the test instead. */
    alarm(60);

    test_read(file, buffer);
    test_random_reads(file, buffer);
    test_misbehaving(file, buffer, "the parent leaves after 3 answers",
                     LEAVES, 3);
    test_misbehaving(file, buffer, "the parent misnumbers its 4th DATA",
                     MISNUMBERS, 3);
    test_misbehaving(file, buffer, "the parent stalls at once",
                     STALLS, 0);
    test_misbehaving(file, buffer, "the parent stops reading after 3 answers",
                     STOPS_READING, 3);
    /* Its first: the protocol is certainly waiting for it when the
     * interrupt comes. */
    test_misbehaving(file, buffer, "the parent trickles its first DATA",
                     TRICKLES, 0);
    test_bad_terms(file);
    test_no_pipe();
    test_bad_descriptors();
    test_full_pipe();
    test_one_session_a_pipe(file, buffer);
    av_free(buffer);
    av_free(file);
    return 0;
}
