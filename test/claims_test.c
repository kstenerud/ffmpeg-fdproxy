/*
 * Checks the claims of fdproxy's FFmpeg side: a pipe serves one session at a time, through
 * whichever descriptor reaches it, and a closed session's descriptor number serves a new pipe
 * (fdproxy-specification.md, Opening). Built with the fuzzer by fuzz/build.sh, under
 * AddressSanitizer and UndefinedBehaviorSanitizer; `make test-claims` builds it and runs it.
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

/* The protocol itself, included so the test can call its functions directly. Its URLProtocol is
 * renamed, so the library's stays apart. */
#define ff_fdproxy_protocol test_fdproxy_protocol
#include "libavformat/fdproxy.c"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

/* More sessions than the table of claims once held, which never let one go. */
#define SESSIONS 100

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* What a parent that keeps to the protocol sends before any READ: HELLO, then REQUEST's terms. */
static int write_opening(int fd)
{
    uint8_t frames[6 + 4 + 6 + 27];
    uint8_t *at = frames;

    at[0] = FDP_HELLO;
    at[1] = 0;
    AV_WL32(at + 2, 4);
    AV_WL16(at + 6, FDP_VERSION_LOWEST);
    AV_WL16(at + 8, FDP_VERSION_HIGHEST);
    at += 10;
    at[0] = FDP_REQUEST;
    at[1] = 1;
    AV_WL32(at + 2, 27);
    AV_WL16(at + 6, 1);         /* version */
    AV_WL64(at + 8, 7);         /* run */
    AV_WL64(at + 16, 1000);     /* size */
    AV_WL32(at + 24, 256);      /* block */
    AV_WL32(at + 28, 256);      /* longest */
    at[32] = 2;                 /* outstanding */
    return write(fd, frames, sizeof(frames)) == sizeof(frames) ? 0 : -1;
}

typedef struct Session {
    URLContext *h;
    int parent_write;   /* the parent's end of the pipe to FFmpeg */
    int parent_read;    /* the parent's end of the pipe from FFmpeg */
    int ffmpeg_read;    /* FFmpeg's ends, which the protocol owns once it has claimed them */
    int ffmpeg_write;
} Session;

/* Makes a session's two pipes, the parent's opening already written into the one to FFmpeg. */
static int make_pipes(Session *s)
{
    int to_ffmpeg[2], from_ffmpeg[2];

    if (pipe(to_ffmpeg) < 0)
        return -1;
    if (pipe(from_ffmpeg) < 0) {
        close(to_ffmpeg[0]);
        close(to_ffmpeg[1]);
        return -1;
    }
    s->ffmpeg_read  = to_ffmpeg[0];
    s->parent_write = to_ffmpeg[1];
    s->parent_read  = from_ffmpeg[0];
    s->ffmpeg_write = from_ffmpeg[1];
    return write_opening(s->parent_write);
}

static int open_session(Session *s)
{
    FDProxyContext *c;

    s->h = av_mallocz(sizeof(*s->h));
    c = av_mallocz(sizeof(*c));
    if (!s->h || !c) {
        av_freep(&s->h);
        av_free(c);
        return AVERROR(ENOMEM);
    }
    c->class = &fdproxy_class;
    av_opt_set_defaults(c);
    c->read_fd_option  = s->ffmpeg_read;
    c->write_fd_option = s->ffmpeg_write;
    s->h->priv_data = c;
    return fdproxy_open(s->h, "fdproxy:", AVIO_FLAG_READ);
}

static void free_session(Session *s)
{
    if (s->h)
        av_freep(&s->h->priv_data);
    av_freep(&s->h);
    close(s->parent_write);
    close(s->parent_read);
}

/* A second session on a pipe that is open already, through another descriptor to it, is refused,
 * and the descriptors it was given are left to their owner. */
static void test_open_pipe_refused(void)
{
    Session first, second;
    int ret;

    if (make_pipes(&first) < 0 || open_session(&first) < 0) {
        expect(0, "the first session opens");
        return;
    }
    second = first;
    second.h = NULL;
    second.ffmpeg_read = dup(first.ffmpeg_read);
    second.parent_read = dup(first.parent_read);
    second.parent_write = dup(first.parent_write);
    second.ffmpeg_write = dup(first.ffmpeg_write);
    ret = open_session(&second);
    expect(ret == AVERROR(EBUSY), "a second session on an open pipe is refused");
    expect(fcntl(second.ffmpeg_read, F_GETFD) >= 0, "a refused session leaves its descriptors open");
    close(second.ffmpeg_read);
    close(second.ffmpeg_write);
    free_session(&second);

    fdproxy_close(first.h);
    close(first.ffmpeg_read);
    close(first.ffmpeg_write);
    free_session(&first);
}

/* Each session's descriptor number goes to the next one's new pipes, as the kernel hands out the
 * lowest free number, once the caller has closed the last session's, and every one opens. */
static void test_numbers_reused(void)
{
    int opened = 0, reused = 0, first_read_fd = -1;

    for (int i = 0; i < SESSIONS; i++) {
        Session s;

        if (make_pipes(&s) < 0)
            break;
        if (i == 0)
            first_read_fd = s.ffmpeg_read;
        else if (s.ffmpeg_read == first_read_fd)
            reused++;
        if (open_session(&s) >= 0) {
            opened++;
            fdproxy_close(s.h);
        }
        close(s.ffmpeg_read);
        close(s.ffmpeg_write);
        free_session(&s);
    }
    expect(reused == SESSIONS - 1, "each new pipe takes the closed session's descriptor number");
    expect(opened == SESSIONS, "every session on a new pipe opens");
}

/* As PID 1 in a container, the default action of a signal is never taken, so the alarm needs one. */
static void on_alarm(int signal_number)
{
    static const char message[] = "FAIL: a session waited for a minute: two sessions shared a pipe\n";

    (void)signal_number;
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(1);
}

int main(void)
{
    av_log_set_level(AV_LOG_QUIET);
    signal(SIGPIPE, SIG_IGN);
    /* A protocol that lets two sessions share a pipe waits forever on the second's HELLO. */
    signal(SIGALRM, on_alarm);
    alarm(60);
    test_numbers_reused();
    test_open_pipe_refused();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("claims: all passed\n");
    return 0;
}
