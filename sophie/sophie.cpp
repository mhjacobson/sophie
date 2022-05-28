//
//  sophie.cpp
//  sophie
//
//  Created by Matt Jacobson on 5/22/22.
//

// Keep absolutely still.  Her vision is based on movement.

// ONGOING:
// monitor for leaks

// TODO:
// [h264 @ 0x7f865502da00] mmco: unref short failure
// [h264_videotoolbox @ 0x7f8e6d992c00] Color range not set for yuv420p. Using MPEG range.
// tweak thresholds
// ffmpeg on server drops audio frames (which crashes us if we're recording at the time)
// figure out what to do with H.264 decoder workaround

// KNOWN LEAKS:
// (macOS only) of a single CVPixelBuffer every time an output file is created, fixed here: <https://github.com/FFmpeg/FFmpeg/commit/8a969e1280aa7aef31de6bd3db5ce46dc123fde0>

#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
#import <CoreGraphics/CoreGraphics.h>
extern "C" void CGImageWriteToFile(CGImageRef, const char *);
#else /* __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ */
#include <png.h>
#endif /* __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ */

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "input.h"
#include "output.h"
#include "util.h"
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <string>

#define PIXEL_DIFFERENCE_THRESHOLD 30
#define AFTER_MOTION_RECORD_SECONDS 10
#define DIFFERENCE_START_Y 25 // to exclude the clock
#define DIFFERENCE_THRESHOLD_YUV 40

// TODO: size this more scientifically somehow?  Could maybe have an AVFrame-specific ring buffer that keeps constant time or memory (or min time, max memory).
RingBuffer<AVFrame *, 1300> frame_buffer([](AVFrame *&frame) {
    av_frame_free(&frame);
});

RingBuffer<bool, 10> interesting_frames;

#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
void dump_picture_gray8(const uint8_t *const bytes, const unsigned int width, const unsigned int height, const unsigned int rowbytes, const char *const filename) {
    const CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, height * rowbytes);
    const CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    const CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();

    const CGImageRef img = CGImageCreate(width, height, 8, 8, rowbytes, space, kCGBitmapByteOrderDefault, provider, NULL, false, kCGRenderingIntentDefault);

    CGImageWriteToFile(img, filename);

    CGImageRelease(img);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
}
#else /* __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ */
void dump_picture_gray8(const uint8_t *const bytes, const unsigned int width, const unsigned int height, const unsigned int rowbytes, const char *const filename) {
    png_bytep *const row_pointers = (png_bytep *)malloc(height * sizeof (uint8_t *));
    for (int i = 0; i < height; i++) {
        row_pointers[i] = (png_bytep)(bytes + i * rowbytes);
    }

    FILE *const fp = fopen(filename, "wb");
    assert(fp);

    const png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert(png_ptr);

    const png_infop info_ptr = png_create_info_struct(png_ptr);
    assert(png_ptr);

    if (setjmp(png_jmpbuf(png_ptr))) {
        abort();
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr, width, height,
                 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);

    free(row_pointers);
    fclose(fp);
}
#endif /* __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ */

// TODO: vector-optimize
Histogram<10> frame_difference_yuv(AVFrame *const picture1, AVFrame *const picture2, uint8_t *const difference_buffer) {
    assert(picture1->width == picture2->width);
    assert(picture1->height == picture2->height);
    const int width = picture1->width;
    const int height = picture1->height;

    Histogram<10> histogram;

    for (int y = DIFFERENCE_START_Y; y < height; y++) {
        const uint8_t *const row1 = picture1->data[0] + y * picture1->linesize[0];
        const uint8_t *const row2 = picture2->data[0] + y * picture2->linesize[0];
        uint8_t *const diffrow = difference_buffer ? (difference_buffer + y * width) : NULL;

        for (int x = 0; x < width; x++) {
            const uint8_t *const pixel1 = row1 + x;
            const uint8_t *const pixel2 = row2 + x;
            uint8_t *const diffpixel = diffrow ? (diffrow + x) : NULL;

            const uint8_t difference = (*pixel1 > *pixel2) ? (*pixel1 - *pixel2) : (*pixel2 - *pixel1);
            histogram.increment(difference);

            if (diffpixel) {
                *diffpixel = difference;
            }
        }
    }

    return histogram;
}

bool manual_trigger = false;
void handle_usr1(int signal) {
    manual_trigger = true;
}

int64_t div_i64_rat(int64_t a, AVRational b) {
    return a * b.den / b.num;
}

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage:\n\tsophie <input specifier> <output directory>\n");
        exit(1);
    }

    av_log_set_level(AV_LOG_WARNING);// TODO: this also blocks the dump input/output.  Can I get that back?
    signal(SIGUSR1, handle_usr1);

    const std::string input_filename = argv[1];
    const std::string output_dir = argv[2];

    Input input(input_filename.c_str());
    Output *output = NULL;
    int64_t last_motion_timestamp = 0;
    AVFrame *const previous_video_frame = av_frame_alloc();
    uint8_t *difference_buffer = NULL;
    int video_frame_total_index = 0;
    std::string temp_filename, destination_filename;

    for (;;) {
        bool is_audio;
        AVFrame *const frame = input.get_next_frame(&is_audio);

        if (frame == NULL) {
            fprintf(stderr, "no frame\n");
            break;
        }

        // Detect motion.
        if (!is_audio) {
            if (previous_video_frame->data[0] != NULL) {
                if (difference_buffer == NULL) {
                    difference_buffer = (uint8_t *)malloc(frame->width * frame->height * sizeof (uint8_t));
                }

                // Filter 1: pixels are only counted as different if they change by DIFFERENCE_THRESHOLD_YUV, to discard sensor noise.
                const Histogram<10> histogram = frame_difference_yuv(previous_video_frame, frame, difference_buffer);
                const uint32_t pixels_different = histogram.count_where([](const uint8_t value) {
                    return value >= DIFFERENCE_THRESHOLD_YUV;
                });

                if (pixels_different > 0) {
                    fprintf(stderr, "%d: %d%s\n", video_frame_total_index, pixels_different, pixels_different > PIXEL_DIFFERENCE_THRESHOLD ? " ***" : "");
                    fprintf(stderr, "%s\n", histogram.description().c_str());
                }

                // Filter 2: frames are only counted as different if PIXEL_DIFFERENCE_THRESHOLD pixels are different, to discard small differences like leaves in the wind and birds.
                const bool frame_different = pixels_different >= PIXEL_DIFFERENCE_THRESHOLD;

                // Filter 3: output is only generated if 3 out of the last 10 frames are different, to discard transient dazzle.
                interesting_frames.append(frame_different);
                const size_t interesting_count = interesting_frames.count_where([](const bool &value) {
                    return value == true;
                });

                if (interesting_count >= 3 || manual_trigger) {
                    if (output == NULL) {
                        char timestamp[1024];
                        const time_t t = time(NULL);
                        strftime(timestamp, sizeof (timestamp), "%Y-%m-%dT%H:%M:%S%z", localtime(&t));
                        const std::string timestamp_string = std::string(timestamp);

                        const std::string frame_output_filename = output_dir + "/" + timestamp_string + ".png";
                        dump_picture_gray8(difference_buffer, frame->width, frame->height, frame->width, frame_output_filename.c_str());

                        char path[] = "/tmp/sophie.mp4.XXXXXX";
                        const int fd = mkstemp(path);
                        assert(fd != -1);
                        temp_filename = std::string(path);

                        // TODO: per-date subdirectory?
                        destination_filename = std::string(output_dir) + "/" + timestamp_string + ".mp4";

                        fprintf(stderr, "%d: starting recording%s to %s\n", video_frame_total_index, manual_trigger ? " (manual)" : "", temp_filename.c_str());

                        output = input.create_output(temp_filename.c_str());

                        // Output our buffered frames first.
                        bool encoded_video = false;
                        for (AVFrame *const frame : frame_buffer) {
                            const bool is_audio = frame_is_audio(frame);

                            // Always start with a video frame, to avoid black poster in AVFoundation.
                            if (encoded_video || !is_audio) {
                                encoded_video = true;
#if VERBOSE
                                fprintf(stderr, "outputting back frame %p\n", frame);
#endif /* VERBOSE */
                                output->encode_frame(frame, is_audio);
                            }
                        }
                    }

                    manual_trigger = false;
                    last_motion_timestamp = frame->pts;
                } else if (output != NULL && frame->pts >= last_motion_timestamp + div_i64_rat(AFTER_MOTION_RECORD_SECONDS, input.video_codec_time_base())) {
                    fprintf(stderr, "%d: ending recording; moving to %s\n", video_frame_total_index, destination_filename.c_str());
                    output->finish();

                    move_file(temp_filename, destination_filename);
                    temp_filename.clear();
                    destination_filename.clear();

                    delete output;
                    output = NULL;
                    last_motion_timestamp = 0;
                }
            }

            video_frame_total_index++;
            av_frame_unref(previous_video_frame);
            av_frame_ref(previous_video_frame, frame);
        }

        // Buffer the frame.
        frame_buffer.append(frame);

        if (output != NULL) {
            output->encode_frame(frame, is_audio);
        }
    }

    // If the input ends while output is active, end output before exiting.
    if (output != NULL) {
        fprintf(stderr, "END: ending recording\n");
        output->finish();

        delete output;
        output = NULL;
        last_motion_timestamp = 0;
    }

    return 0;
}
