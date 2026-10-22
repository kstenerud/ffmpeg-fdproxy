/*
 * A libFuzzer harness for FFmpeg's side of ffstreamer: each input is what the parent sends, fed
 * through a pipe as the protocol reads it, while what the protocol sends back is drained. The
 * protocol opens, reads, seeks and closes as FFmpeg would. Built with AddressSanitizer and
 * UndefinedBehaviorSanitizer by fuzz/build.sh, which says how to run it.
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

/* The protocol itself, included so the harness can forget its claims between inputs: the real one
 * lets a channel open once per process. Its URLProtocol is renamed, so the library's stays apart. */
#define ff_ffstreamer_protocol fuzz_ffstreamer_protocol
#include "libavformat/ffstreamer.c"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

/* An input a pipe takes whole, so writing it never waits on the protocol. */
#define MOST_INPUT_BYTES (60 << 10)

static void *drain(void *read_end)
{
    uint8_t buffer[4096];
    int fd = (int)(intptr_t)read_end;
    while (read(fd, buffer, sizeof(buffer)) > 0)
        ;
    return NULL;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    av_log_set_level(AV_LOG_QUIET);
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int to_ffmpeg[2], from_ffmpeg[2];
    pthread_t drainer;
    URLContext *h = NULL;
    FFStreamerContext *c;
    uint8_t buffer[64 << 10];
    int ret;

    if (size > MOST_INPUT_BYTES)
        size = MOST_INPUT_BYTES;
    claimed_count = 0;

    if (pipe(to_ffmpeg) < 0)
        goto end;
    if (pipe(from_ffmpeg) < 0)
        goto close_to;
    if (size && write(to_ffmpeg[1], data, size) != (ssize_t)size)
        goto close_from;
    close(to_ffmpeg[1]);    /* the parent has said all it will */
    to_ffmpeg[1] = -1;
    if (pthread_create(&drainer, NULL, drain, (void *)(intptr_t)from_ffmpeg[0]))
        goto close_from;

    h = av_mallocz(sizeof(*h));
    c = av_mallocz(sizeof(*c));
    if (!h || !c) {
        av_free(c);
        close(from_ffmpeg[1]);
        from_ffmpeg[1] = -1;
        goto join;
    }
    c->class = &ffstreamer_class;
    av_opt_set_defaults(c);
    c->read_fd = to_ffmpeg[0];
    c->write_fd = from_ffmpeg[1];
    c->cache_blocks = 2;
    h->priv_data = c;
    /* From here the protocol owns its two descriptors, and closes them. */
    to_ffmpeg[0] = from_ffmpeg[1] = -1;

    if (ffstreamer_init(h, "ffstreamer:", AVIO_FLAG_READ) >= 0) {
        /* Read straight through, then seek back into the middle and read on, as a demuxer might. */
        while ((ret = ffstreamer_read(h, buffer, sizeof(buffer))) > 0)
            ;
        if (ffstreamer_seek(h, ffstreamer_seek(h, 0, AVSEEK_SIZE) / 2, SEEK_SET) >= 0)
            ffstreamer_read(h, buffer, sizeof(buffer));
        /* Seeks at the edges of the arithmetic, against whatever size the parent gave: a crafted
         * film can make a demuxer ask any of these. */
        ffstreamer_seek(h, INT64_MAX, SEEK_CUR);
        ffstreamer_seek(h, INT64_MIN, SEEK_CUR);
        ffstreamer_seek(h, INT64_MAX, SEEK_END);
        if (ffstreamer_seek(h, -1, SEEK_END) >= 0)
            ffstreamer_read(h, buffer, sizeof(buffer));
        ffstreamer_close_all(h);
    }
    av_free(c);

join:
    pthread_join(drainer, NULL);
close_from:
    if (from_ffmpeg[0] >= 0)
        close(from_ffmpeg[0]);
    if (from_ffmpeg[1] >= 0)
        close(from_ffmpeg[1]);
close_to:
    if (to_ffmpeg[0] >= 0)
        close(to_ffmpeg[0]);
    if (to_ffmpeg[1] >= 0)
        close(to_ffmpeg[1]);
end:
    av_free(h);
    return 0;
}
