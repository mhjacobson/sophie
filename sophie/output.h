//
//  output.h
//  sophie
//
//  Created by Matt Jacobson on 5/23/22.
//

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include "util.h"
#include <stdint.h>

#ifndef OUTPUT_H
#define OUTPUT_H

struct Output : DeleteImplicit {
    Output(const char *const filename, const AVCodecParameters *const video_codecpar, const AVCodecParameters *const audio_codecpar, const AVRational video_time_base, const AVRational audio_time_base);
    void encode_frame(AVFrame *frame, bool is_audio);
    void flush(bool is_audio);
    void finish();
    ~Output();

private:
    AVFormatContext *_output_ctx;
    AVIOContext *_io_ctx;
    AVStream *_video_stream;
    AVStream *_audio_stream;
    AVCodecContext *_video_codec_ctx;
    AVCodecContext *_audio_codec_ctx;
    int64_t _epoch;
    bool _have_epoch;
};

#endif /* OUTPUT_H */
