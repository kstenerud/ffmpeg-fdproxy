/*
 * Tests the ffstreamer protocol through the public avio API, against a parent
 * served by a thread over two pipes: a file read through, seeks, a parent that
 * leaves mid-session, terms the protocol refuses, and a pipe that serves one
 * session at a time.
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

enum {
    HELLO = 1, CAPABILITY = 2, REQUEST = 3, READ = 4, DATA = 5, DONE = 6,
};

#define FILE_SIZE  100003
#define BLOCK      4096

/** The parent's side of one session, which serve() runs in a thread. */
typedef struct Parent {
    int from_child;     /**< where the protocol's frames arrive */
    int to_child;       /**< where the parent's frames go */
    const uint8_t *file;
    int64_t size;
    int32_t block;
    int answers;        /**< READs answered before leaving; -1 for all */
    int done_result;    /**< DONE's result: -1 when none came */
    int done_kind;      /**< DONE's error kind, for an ERR */
    int violation;      /**< a frame broke the specification */
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

/** The parent: the opening exchange, then a DATA for each READ. */
static void *serve(void *opaque)
{
    Parent *p = opaque;
    uint8_t body[1040], terms[27];
    int expected = 0, sequence, length, type, answered = 0;

    if (next_frame(p, &expected, &sequence, body, &length) != HELLO)
        goto end;
    AV_WL16(body, 1);
    AV_WL16(body + 2, 1);
    if (write_frame(p->to_child, HELLO, 0, body, 4) < 0 ||
        next_frame(p, &expected, &sequence, body, &length) != CAPABILITY)
        goto end;
    AV_WL16(terms, 1);              /* version */
    AV_WL64(terms + 2, 7);          /* run */
    AV_WL64(terms + 10, p->size);
    AV_WL32(terms + 18, p->block);
    AV_WL32(terms + 22, p->block);  /* longest */
    terms[26] = 2;                  /* outstanding */
    if (write_frame(p->to_child, REQUEST, sequence, terms, 27) < 0)
        goto end;

    while ((type = next_frame(p, &expected, &sequence, body, &length)) > 0) {
        int64_t offset;
        int32_t wanted;

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
        if (p->answers >= 0 && answered == p->answers) {
            close(p->to_child);
            p->to_child = -1;
            continue;
        }
        if (p->to_child >= 0 &&
            write_frame(p->to_child, DATA, sequence, p->file + offset,
                        wanted) < 0)
            break;
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

static int start(Session *s, const uint8_t *file, int64_t size, int32_t block,
                 int answers)
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
        .from_child  = from_child[0], .to_child = to_child[1],
        .file        = file,          .size     = size,
        .block       = block,         .answers  = answers,
        .done_result = -1,
    };
    av_dict_set_int(&s->options, "ffstreamer_read_fd", s->child_read, 0);
    av_dict_set_int(&s->options, "ffstreamer_write_fd", s->child_write, 0);
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

/** Waits for the parent, which ends when the protocol closed its pipe. */
static void finish(Session *s)
{
    pthread_join(s->thread, NULL);
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

static int read_through(AVIOContext *pb, uint8_t *buffer, int size)
{
    int done = 0, ret;

    while (done < size &&
           (ret = avio_read(pb, buffer + done, size - done)) > 0)
        done += ret;
    return done;
}

static void test_read(const uint8_t *file)
{
    uint8_t *buffer = av_malloc(FILE_SIZE);
    AVIOContext *pb = NULL;
    Session s;
    int ret;

    printf("read:\n");
    if (!buffer || start(&s, file, FILE_SIZE, BLOCK, -1) < 0) {
        av_free(buffer);
        printf("cannot start\n");
        return;
    }
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, NULL, &s.options);
    printf("open: %d\n", ret);
    if (ret >= 0) {
        printf("size: %"PRId64"\n", avio_size(pb));
        ret = read_through(pb, buffer, FILE_SIZE);
        printf("read %d bytes, as served: %s\n", ret,
               ret == FILE_SIZE && !memcmp(buffer, file, FILE_SIZE) ?
               "yes" : "no");
        printf("seek to 50000: %"PRId64"\n", avio_seek(pb, 50000, SEEK_SET));
        ret = read_through(pb, buffer, 3000);
        printf("read %d bytes, as served: %s\n", ret,
               !memcmp(buffer, file + 50000, 3000) ? "yes" : "no");
        printf("seek to 10 before the end: %"PRId64"\n",
               avio_seek(pb, FILE_SIZE - 10, SEEK_SET));
        ret = read_through(pb, buffer, 100);
        printf("read %d bytes, as served: %s, at the end: %d\n", ret,
               !memcmp(buffer, file + FILE_SIZE - 10, 10) ? "yes" : "no",
               avio_feof(pb));
        avio_closep(&pb);
    }
    finish(&s);
    print_done(&s.parent);
    av_free(buffer);
}

static void test_parent_leaves(const uint8_t *file)
{
    uint8_t *buffer = av_malloc(FILE_SIZE);
    AVIOContext *pb = NULL;
    Session s;
    int ret;

    printf("the parent leaves after 3 answers:\n");
    if (!buffer || start(&s, file, FILE_SIZE, BLOCK, 3) < 0) {
        av_free(buffer);
        printf("cannot start\n");
        return;
    }
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, NULL, &s.options);
    printf("open: %d\n", ret);
    if (ret >= 0) {
        ret = read_through(pb, buffer, FILE_SIZE);
        printf("read %d bytes, then failed: %s\n", ret,
               pb->error < 0 && pb->error != AVERROR_EOF ? "yes" : "no");
        avio_closep(&pb);
    }
    finish(&s);
    print_done(&s.parent);
    av_free(buffer);
}

static void test_bad_terms(const uint8_t *file)
{
    AVIOContext *pb = NULL;
    Session s;
    int ret;

    printf("a REQUEST whose block is 0:\n");
    if (start(&s, file, FILE_SIZE, 0, -1) < 0) {
        printf("cannot start\n");
        return;
    }
    ret = avio_open2(&pb, "ffstreamer:", AVIO_FLAG_READ, NULL, &s.options);
    printf("open refused: %s\n", ret == AVERROR(EINVAL) ? "yes" : "no");
    avio_closep(&pb);
    finish(&s);
    print_done(&s.parent);
}

static void test_one_session_a_pipe(const uint8_t *file)
{
    AVIOContext *first = NULL, *second = NULL;
    AVDictionary *options = NULL;
    Session s;
    int read_copy, write_copy, ret;

    printf("a second session on an open pipe:\n");
    if (start(&s, file, FILE_SIZE, BLOCK, -1) < 0) {
        printf("cannot start\n");
        return;
    }
    ret = avio_open2(&first, "ffstreamer:", AVIO_FLAG_READ, NULL, &s.options);
    printf("first open: %d\n", ret);
    read_copy  = dup(s.child_read);
    write_copy = dup(s.child_write);
    av_dict_set_int(&options, "ffstreamer_read_fd", read_copy, 0);
    av_dict_set_int(&options, "ffstreamer_write_fd", write_copy, 0);
    ret = avio_open2(&second, "ffstreamer:", AVIO_FLAG_READ, NULL, &options);
    printf("second open refused: %s\n", ret == AVERROR(EBUSY) ? "yes" : "no");
    printf("its descriptors left open: %s\n",
           !close(read_copy) && !close(write_copy) ? "yes" : "no");
    av_dict_free(&options);
    avio_closep(&second);
    avio_closep(&first);
    finish(&s);
    print_done(&s.parent);
}

int main(void)
{
    uint8_t *file = av_malloc(FILE_SIZE);

    if (!file)
        return 1;
    for (int i = 0; i < FILE_SIZE; i++)
        file[i] = (i * 7) ^ (i >> 8);
    av_log_set_level(AV_LOG_QUIET);
    signal(SIGPIPE, SIG_IGN);

    test_read(file);
    test_parent_leaves(file);
    test_bad_terms(file);
    test_one_session_a_pipe(file);
    av_free(file);
    return 0;
}
