//
//  output.cpp
//  sophie
//
//  Created by Matt Jacobson on 5/23/22.
//

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include "output.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

Output::Output(const std::string filename, const AVCodecParameters *const video_codecpar, const AVCodecParameters *const audio_codecpar, const AVRational video_time_base, const AVRational audio_time_base) : _io_ctx(NULL), _epoch(0), _have_epoch(false) {
    const AVCodec *const video_codec = avcodec_find_encoder(video_codecpar->codec_id);
    const AVCodec *const audio_codec = avcodec_find_encoder(audio_codecpar->codec_id);

    avformat_alloc_output_context2(&_output_ctx, NULL, "mp4", filename.c_str());
    assert(_output_ctx);

    _video_stream = avformat_new_stream(_output_ctx, video_codec);
    _audio_stream = avformat_new_stream(_output_ctx, audio_codec);

    // NOTE: don't just blindly copy video_codecpar over, since it may have decoding values that don't make sense for encoding.
    _video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    _video_stream->codecpar->codec_id = video_codecpar->codec_id;
    _video_stream->codecpar->codec_tag = av_codec_get_tag(_output_ctx->oformat->codec_tag, video_codec->id);
    _video_stream->codecpar->width = video_codecpar->width;
    _video_stream->codecpar->height = video_codecpar->height;
    _video_stream->codecpar->format = video_codecpar->format;
    _video_stream->codecpar->color_range = AVCOL_RANGE_MPEG;
    _video_stream->codecpar->profile = video_codecpar->profile;
    _video_stream->codecpar->level = video_codecpar->level;

    _audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    _audio_stream->codecpar->codec_id = audio_codecpar->codec_id;
    _audio_stream->codecpar->codec_tag = av_codec_get_tag(_output_ctx->oformat->codec_tag, audio_codec->id);
    _audio_stream->codecpar->channels = audio_codecpar->channels;
    _audio_stream->codecpar->channel_layout = audio_codecpar->channel_layout;
    _audio_stream->codecpar->sample_rate = audio_codecpar->sample_rate;
    _audio_stream->codecpar->format = audio_codecpar->format;

    // NOTE: explanations as to why not to use _video_stream->codec:
    //     <https://lists.libav.org/pipermail/libav-commits/2016-February/018031.html>
    //     <https://github.com/FFmpeg/FFmpeg/commit/9200514ad8717c63f82101dc394f4378854325bf>
    _video_codec_ctx = avcodec_alloc_context3(video_codec);
    _video_codec_ctx->time_base = video_time_base; // decoder and encoder codec_ctx must match in timebase
    avcodec_parameters_to_context(_video_codec_ctx, _video_stream->codecpar);

    if (_output_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        _video_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(_video_codec_ctx, video_codec, NULL) < 0) {
        abort();
    }

    avcodec_parameters_from_context(_video_stream->codecpar, _video_codec_ctx);

    // ---

    _audio_codec_ctx = avcodec_alloc_context3(audio_codec);
    _audio_codec_ctx->time_base = audio_time_base; // decoder and encoder codec_ctx must match in timebase
    avcodec_parameters_to_context(_audio_codec_ctx, _audio_stream->codecpar);

    if (_output_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        _audio_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(_audio_codec_ctx, audio_codec, NULL) < 0) {
        abort();
    }

    avcodec_parameters_from_context(_audio_stream->codecpar, _audio_codec_ctx);

    if (avio_open(&_io_ctx, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
        abort();
    }

    _output_ctx->pb = _io_ctx;

    if (avformat_write_header(_output_ctx, NULL) < 0) {
        abort();
    }

    av_dump_format(_output_ctx, 0, filename.c_str(), 1);
}

void Output::encode_frame(AVFrame *const frame, const bool is_audio) {
    // Abort if called after finish().
    assert(_output_ctx != NULL);

    AVCodecContext *const codec_ctx = is_audio ? _audio_codec_ctx : _video_codec_ctx;

    // Translate the frame's PTS to our epoch.  First, rescale the PTS into our epoch's timebase (for which we use _video_stream->time_base).  Then, subtract out the epoch.  Finally, rescale back into the codec_ctx timebase.
    const int64_t original_pts = frame->pts;
    const int64_t pts_in_video_stream = av_rescale_q(original_pts, codec_ctx->time_base, _video_stream->time_base);

    if (!_have_epoch) {
        _epoch = pts_in_video_stream;
        _have_epoch = true;
    }

    const int64_t translated_pts_in_video_stream = pts_in_video_stream - _epoch;
    const int64_t translated_pts_in_codec = av_rescale_q(translated_pts_in_video_stream, _video_stream->time_base, codec_ctx->time_base);
    frame->pts = translated_pts_in_codec;

#if VERBOSE
    fprintf(stderr, "> encode %s: %" PRId64 " (orig: %" PRId64 ")\n", is_audio ? "audio" : "video", frame->pts, original_pts);
#endif /* VERBOSE */

    if (avcodec_send_frame(codec_ctx, frame) < 0) {
        abort();
    }

    // Restore the original pts, since this frame is being kept around by the caller.
    frame->pts = original_pts;

    flush(is_audio);
}

void Output::flush(const bool is_audio) {
    // Abort if called after finish().
    assert(_output_ctx != NULL);

    AVCodecContext *const codec_ctx = is_audio ? _audio_codec_ctx : _video_codec_ctx;
    AVStream *const stream = is_audio ? _audio_stream : _video_stream;
    AVPacket *packet = av_packet_alloc();

    for (;;) {
        const int rv = avcodec_receive_packet(codec_ctx, packet);

        if (rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
            break;
        } else if (rv < 0) {
            abort();
        }

        av_packet_rescale_ts(packet, codec_ctx->time_base, stream->time_base);
#if VERBOSE
        fprintf(stderr, "> write %s: dts %" PRId64 "\n", is_audio ? "audio" : "video", packet->dts);
#endif /* VERBOSE */

        packet->stream_index = stream->index;

        if (av_interleaved_write_frame(_output_ctx, packet) < 0) {
            abort();
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    assert(packet == NULL);
}

void Output::finish() {
    avcodec_send_frame(_video_codec_ctx, NULL);
    flush(false);

    avcodec_send_frame(_audio_codec_ctx, NULL);
    flush(true);

    av_write_trailer(_output_ctx);

    // NOTE: per avcodec.h, no need to also call avcodec_close()
    avcodec_free_context(&_video_codec_ctx);
    _video_codec_ctx = NULL;

    avcodec_free_context(&_audio_codec_ctx);
    _audio_codec_ctx = NULL;

    avformat_free_context(_output_ctx);
    _output_ctx = NULL;
    _video_stream = NULL;
    _audio_stream = NULL;

    avio_close(_io_ctx);
    _io_ctx = NULL;
}

Output::~Output() {
    // Abort if finish() was never called.
    assert(_output_ctx == NULL);
}
