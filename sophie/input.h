//
//  input.h
//  sophie
//
//  Created by Matt Jacobson on 5/26/22.
//

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include "output.h"

#ifndef INPUT_H
#define INPUT_H

struct Input {
    Input(const char *filename);
    AVFrame *get_next_frame(bool *is_audio_out);
    AVRational video_codec_time_base();
    Output *create_output(const char *filename);
    ~Input();
private:
    AVFormatContext *_input_ctx;
    int _video_stream_index;
    int _audio_stream_index;
    AVCodecParameters *_video_codecpar;
    AVCodecParameters *_audio_codecpar;
    AVCodecContext *_video_codec_ctx;
    AVCodecContext *_audio_codec_ctx;
};

static bool frame_is_audio(AVFrame *const frame) {
    return frame->channels != 0;
}

#endif /* INPUT_H */
