//
//  input.cpp
//  sophie
//
//  Created by Matt Jacobson on 5/26/22.
//

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include "input.h"
#include "output.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

Input::Input(const std::string filename) {
    _input_ctx = NULL;
    if (avformat_open_input(&_input_ctx, filename.c_str(), NULL, NULL) != 0) {
        av_log(NULL, AV_LOG_ERROR, "Couldn't open file\n");
        abort();
    }

    if (avformat_find_stream_info(_input_ctx, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Couldn't find stream information\n");
        abort();
    }

    const AVCodec *video_codec;
    const AVCodec *audio_codec;
    _video_stream_index = av_find_best_stream(_input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0);
    _audio_stream_index = av_find_best_stream(_input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
    assert(_video_stream_index >= 0);
    assert(_audio_stream_index >= 0);
    assert(video_codec != NULL);
    assert(audio_codec != NULL);

    // Get the codec parameterses for the streams.
    AVStream *const video_stream = _input_ctx->streams[_video_stream_index];
    AVStream *const audio_stream = _input_ctx->streams[_audio_stream_index];
    _video_codecpar = video_stream->codecpar;
    _audio_codecpar = audio_stream->codecpar;

    // Create codec contexts.
    _video_codec_ctx = avcodec_alloc_context3(video_codec);
    avcodec_parameters_to_context(_video_codec_ctx, _video_codecpar);

    _audio_codec_ctx = avcodec_alloc_context3(audio_codec);
    avcodec_parameters_to_context(_audio_codec_ctx, _audio_codecpar);

    // Open codec contexts.
    if (avcodec_open2(_video_codec_ctx, video_codec, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        abort();
    }

    if (avcodec_open2(_audio_codec_ctx, audio_codec, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
        abort();
    }

    // Seek time zero.
    // NOTE: without this line, the first packet from the H.264 decoder has an unset PTS.
    // TODO: investigate further
    avformat_seek_file(_input_ctx, _video_stream_index, 0, 0, 0, AVSEEK_FLAG_FRAME);

    av_dump_format(_input_ctx, 0, filename.c_str(), 0);
    fprintf(stderr, "input video timebases: stream = %s, codec = %s\n", timebase_str(video_stream->time_base).c_str(), timebase_str(_video_codec_ctx->time_base).c_str());
    fprintf(stderr, "input audio timebases: stream = %s, codec = %s\n", timebase_str(audio_stream->time_base).c_str(), timebase_str(_audio_codec_ctx->time_base).c_str());
}

// Caller must free returned frame.
AVFrame *Input::get_next_frame(bool *const is_audio_out) {
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool got_frame = false;

    // See if there's already a frame waiting for us.  (This usually doesn't happen.)
    if (!got_frame) {
        if (avcodec_receive_frame(_video_codec_ctx, frame) >= 0) {
            assert(frame->pts >= 0);
            got_frame = true;
        } else if (avcodec_receive_frame(_audio_codec_ctx, frame) >= 0) {
            assert(frame->pts >= 0);
            got_frame = true;
        }
    }

    while (!got_frame) {
        const int rv = av_read_frame(_input_ctx, packet);

        if (rv >= 0) {
            const bool is_audio = packet->stream_index == _audio_stream_index;
#if VERBOSE
            fprintf(stderr, "< read %s: dts %" PRId64 "\n", is_audio ? "audio" : "video", packet->dts);
#endif /* VERBOSE */

            AVStream *const stream = _input_ctx->streams[packet->stream_index];
            AVCodecContext *const codec_ctx = is_audio ? _audio_codec_ctx : _video_codec_ctx;

            if (avcodec_send_packet(codec_ctx, packet) < 0) {
                abort();
            }

            if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                assert(frame->pts >= 0);
                got_frame = true;
            }
        } else if (rv == AVERROR_EOF) {
            break;
        }

        // av_read_frame() doesn't unref the packet first, so we need this here in case we loop back around.
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    assert(packet == NULL);

    if (got_frame) {
        const bool is_audio = frame_is_audio(frame);
        if (is_audio_out) *is_audio_out = is_audio;

#if VERBOSE
        fprintf(stderr, "< decode %s: %" PRId64 " (%" PRId64 ")\n", is_audio ? "audio" : "video", frame->pts, frame->pkt_dts);
#endif /* VERBOSE */
    } else {
        av_frame_free(&frame);
        assert(frame == NULL);
    }

    return frame;
}

AVRational Input::video_frame_time_base() {
    return _input_ctx->streams[_video_stream_index]->time_base;
}

Output *Input::create_output(const std::string filename) {
    return new Output(filename, _video_codecpar, _audio_codecpar, _input_ctx->streams[_video_stream_index]->time_base, _input_ctx->streams[_audio_stream_index]->time_base);
}

Input::~Input() {
    // NOTE: per avcodec.h, no need to also call avcodec_close()
    avcodec_free_context(&_video_codec_ctx);
    avcodec_free_context(&_audio_codec_ctx);

    avformat_free_context(_input_ctx);
}
