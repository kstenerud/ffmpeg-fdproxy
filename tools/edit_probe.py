p="fftools/ffprobe.c"
s=open(p).read()
old="""end:
    if (ifile.fmt_ctx)
        close_input_file(&ifile);
    av_freep(&nb_streams_frames);"""
new="""end:
    /* A read that failed is not the input ending (ffstreamer, PROTOCOL.md section 10.3). */
    if (ret >= 0 && ifile.fmt_ctx && ifile.fmt_ctx->pb &&
        ifile.fmt_ctx->pb->error < 0 && ifile.fmt_ctx->pb->error != AVERROR(EAGAIN)) {
        ret = ifile.fmt_ctx->pb->error;
        av_log(NULL, AV_LOG_ERROR, "Reading the input failed: %s\\n", av_err2str(ret));
    }
    if (ifile.fmt_ctx)
        close_input_file(&ifile);
    av_freep(&nb_streams_frames);"""
assert s.count(old)==1
open(p,"w").write(s.replace(old,new))
print("ffprobe edited")
