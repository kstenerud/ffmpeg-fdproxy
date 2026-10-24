def edit(p, old, new, count=1):
    s=open(p).read()
    assert s.count(old)==count, (p, old[:60], s.count(old))
    s=s.replace(old,new)
    open(p,"w").write(s)

edit("fftools/ffmpeg_demux.c", """    // EOF/EXIT is normal termination
    if (ret == AVERROR_EOF || ret == AVERROR_EXIT)
        ret = 0;
""", """    /* An input whose reads failed fails the run, where it would otherwise end
     * as if the input had simply ended (fdproxy's specification, Exit Status). */
    if ((ret >= 0 || ret == AVERROR_EOF || ret == AVERROR_EXIT) &&
        f->ctx->pb && f->ctx->pb->error < 0 && f->ctx->pb->error != AVERROR(EAGAIN)) {
        ret = f->ctx->pb->error == AVERROR_EXIT ? AVERROR(EINTR) : f->ctx->pb->error;
        av_log(d, AV_LOG_ERROR, "Reading the input failed: %s\\n", av_err2str(ret));
    }

    // EOF/EXIT is normal termination
    if (ret == AVERROR_EOF || ret == AVERROR_EXIT)
        ret = 0;
""")
edit("fftools/ffmpeg_demux.c", """        if (ret < 0) {
            av_log(d, AV_LOG_FATAL, "could not find codec parameters\\n");
            if (ic->nb_streams == 0)
                return ret;
        }
""", """        if (ret < 0) {
            av_log(d, AV_LOG_FATAL, "could not find codec parameters\\n");
            if (ic->nb_streams == 0)
                return ret;
        }
        /* A read that failed while probing is not the input ending (fdproxy). */
        if (ic->pb && ic->pb->error < 0 && ic->pb->error != AVERROR(EAGAIN)) {
            ret = ic->pb->error == AVERROR_EXIT ? AVERROR(EINTR) : ic->pb->error;
            av_log(d, AV_LOG_FATAL, "Reading the input failed while probing it: %s\\n", av_err2str(ret));
            return ret;
        }
""")
edit("fftools/ffmpeg.c", """finish:
    if (ret == AVERROR_EXIT)
        ret = 0;
""", """finish:
    /* An interrupt during the opening of an input never reaches transcode(); a
     * signal must still exit non-zero (fdproxy's specification, Exit Status). */
    if (ret == AVERROR_EXIT)
        ret = received_nb_signals ? 255 : 0;
""")
edit("libavformat/protocols.c", "extern const URLProtocol ff_fd_protocol;", "extern const URLProtocol ff_fd_protocol;\nextern const URLProtocol ff_fdproxy_protocol;")
edit("libavformat/Makefile", "OBJS-$(CONFIG_FD_PROTOCOL)               += file.o", "OBJS-$(CONFIG_FD_PROTOCOL)               += file.o\nOBJS-$(CONFIG_FDPROXY_PROTOCOL)       += fdproxy.o")
edit("configure", "async_protocol_deps=\"threads\"", "async_protocol_deps=\"threads\"\nfdproxy_protocol_deps=\"poll_h threads\"")
print("edited")
