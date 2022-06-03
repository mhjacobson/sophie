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
#include <sys/stat.h>
#include <sys/types.h>

#define PIXEL_DIFFERENCE_THRESHOLD 40
#define DIFFERENT_PIXELS_COUNT_THRESHOLD 30
#define AFTER_MOTION_RECORD_SECONDS 10
#define DIFFERENCE_START_Y 25 // to exclude the clock

// TODO: size this more scientifically somehow?  Could maybe have an AVFrame-specific ring buffer that keeps constant time or memory (or min time, max memory).
RingBuffer<AVFrame *, 1300> frame_buffer([](AVFrame *&frame) {
    av_frame_free(&frame);
});

RingBuffer<bool, 10> interesting_frames;

#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
void dump_picture_gray8(const uint8_t *const bytes, const unsigned int width, const unsigned int height, const unsigned int rowbytes, const std::string filename) {
    const CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, height * rowbytes);
    const CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    const CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();

    const CGImageRef img = CGImageCreate(width, height, 8, 8, rowbytes, space, kCGBitmapByteOrderDefault, provider, NULL, false, kCGRenderingIntentDefault);

    CGImageWriteToFile(img, filename.c_str());

    CGImageRelease(img);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
}

void dump_picture_bgra(const uint8_t *const bytes, const unsigned int width, const unsigned int height, const unsigned int rowbytes, const std::string filename){
    const CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, height * rowbytes);
    const CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    // TODO: improperly assuming colorspace
    const CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    const CGImageRef img = CGImageCreate(width, height, 8, 32, rowbytes, space, kCGBitmapByteOrder32Little | (int)kCGImageAlphaNoneSkipFirst, provider, NULL, false, kCGRenderingIntentDefault);

    CGImageWriteToFile(img, filename.c_str());

    CGImageRelease(img);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
}
#else /* __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ */
void dump_picture_gray8(const uint8_t *const bytes, const unsigned int width, const unsigned int height, const unsigned int rowbytes, const std::string filename) {
    png_bytep *const row_pointers = (png_bytep *)malloc(height * sizeof (uint8_t *));
    for (int i = 0; i < height; i++) {
        row_pointers[i] = (png_bytep)(bytes + i * rowbytes);
    }

    FILE *const fp = fopen(filename.c_str(), "wb");
    assert(fp);

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert(png_ptr);

    png_infop info_ptr = png_create_info_struct(png_ptr);
    assert(png_ptr);

    if (setjmp(png_jmpbuf(png_ptr))) {
        abort();
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr, width, height,
                 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    free(row_pointers);
    fclose(fp);
}

void dump_picture_bgra(const uint8_t *const bytes, const unsigned int width, const unsigned int height, const unsigned int rowbytes, const std::string filename){
    png_bytep *const row_pointers = (png_bytep *)malloc(height * sizeof (uint8_t *));
    for (int i = 0; i < height; i++) {
        row_pointers[i] = (png_bytep)(bytes + i * rowbytes);
    }

    FILE *const fp = fopen(filename.c_str(), "wb");
    assert(fp);

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert(png_ptr);

    png_infop info_ptr = png_create_info_struct(png_ptr);
    assert(png_ptr);

    if (setjmp(png_jmpbuf(png_ptr))) {
        abort();
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr, width, height,
                 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_bgr(png_ptr);
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    free(row_pointers);
    fclose(fp);
}
#endif /* __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ */

void dump_frame(AVFrame *const frame, const std::string filename) {
    const int width = frame->width;
    const int height = frame->height;
    const int bgra_rowbytes = width * 4 * sizeof (uint8_t);
    uint8_t *const bgra_buffer = (uint8_t *)malloc(height * bgra_rowbytes);

    struct SwsContext *convert_ctx = sws_getContext(width, height, (enum AVPixelFormat)frame->format, width, height, AV_PIX_FMT_BGRA, 0, NULL, NULL, NULL);
    // TODO: assuming ITU-R BT.709 encoding, AVCOL_RANGE_MPEG (0)
    sws_setColorspaceDetails(convert_ctx, sws_getCoefficients(SWS_CS_ITU709), 0, sws_getCoefficients(SWS_CS_DEFAULT), 0, 0, 1 << 16, 1 << 16);
    sws_scale(convert_ctx, frame->data, frame->linesize, 0, height, &bgra_buffer, &bgra_rowbytes);
    sws_freeContext(convert_ctx);

    dump_picture_bgra(bgra_buffer, width, height, bgra_rowbytes, filename);
    free(bgra_buffer);
}

// TODO: vector-optimize
Histogram<10> frame_difference_yuv(AVFrame *const frame1, AVFrame *const frame2, uint8_t *const difference_buffer) {
    assert(frame1->format == AV_PIX_FMT_YUV420P);
    assert(frame2->format == AV_PIX_FMT_YUV420P);
    assert(frame1->width  == frame2->width);
    assert(frame1->height == frame2->height);
    const int width = frame1->width;
    const int height = frame1->height;

    Histogram<10> histogram;

    for (int y = DIFFERENCE_START_Y; y < height; y++) {
        const uint8_t *const row1 = frame1->data[0] + y * frame1->linesize[0];
        const uint8_t *const row2 = frame2->data[0] + y * frame2->linesize[0];
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

void brand_frame(AVFrame *const frame) {
    const int padding = 10;
    const int box_side = 25;

    const int width = frame->width;
    const int height = frame->height;

    assert(width > 2 * padding + box_side);
    assert(height > 2 * padding + box_side);
    assert(frame->format == AV_PIX_FMT_YUV420P);

    const int linesize_u = frame->linesize[1];
    const int linesize_v = frame->linesize[2];

    for (int y = 10; y < 35; y++) {
        const int row_index = y / 2;
        uint8_t *const urow = (uint8_t *)(frame->data[1] + row_index * linesize_u);
        uint8_t *const vrow = (uint8_t *)(frame->data[2] + row_index * linesize_v);

        for (int x = width - 35; x < width - 10; x++) {
            const int sample_index = x / 2;
            uint8_t *const u = urow + sample_index;
            uint8_t *const v = vrow + sample_index;

            *u = 0;
            *v = 255;
        }
    }
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

    av_log_set_level(AV_LOG_WARNING); // TODO: this also blocks the dump input/output.  Can I get that back?
    signal(SIGUSR1, handle_usr1);

    const std::string input_filename = argv[1];
    const std::string output_dir = argv[2];

    Input input(input_filename);
    Output *output = NULL;
    int64_t last_motion_timestamp = 0;
    AVFrame *const previous_video_frame = av_frame_alloc();
    uint8_t *difference_buffer = NULL;
    int video_frame_total_index = 0;
    std::string temp_filename, destination_filename;

    for (;;) {
        bool is_audio;
        AVFrame *const frame = input.get_next_frame(&is_audio);

        static bool got_first_frame;
        if (!got_first_frame) {
            fprintf(stderr, "sophie on guard dog duty!\n");
            got_first_frame = true;
        }

        if (frame == NULL) {
            fprintf(stderr, "no frame\n");
            break;
        }

        // Detect motion.
        if (!is_audio) {
            // Delete some stuff from the frame to avoid affecting output encoding.  Seems like this state shouldn't really be on AVFrame itself.
            frame->key_frame = 0;
            frame->pict_type = AV_PICTURE_TYPE_NONE;

            if (previous_video_frame->data[0] != NULL) {
                if (difference_buffer == NULL) {
                    const size_t size = frame->width * frame->height * sizeof (uint8_t);
                    difference_buffer = (uint8_t *)malloc(size);
                    memset(difference_buffer, 0, size);
                }

                // Filter 1: pixels are only counted as different if they change by PIXEL_DIFFERENCE_THRESHOLD, to discard sensor noise.
                const Histogram<10> histogram = frame_difference_yuv(previous_video_frame, frame, difference_buffer);
                const uint32_t pixels_different = histogram.count_where([](const uint8_t value) {
                    return value >= PIXEL_DIFFERENCE_THRESHOLD;
                });

                // Filter 2: frames are only counted as interesting if DIFFERENT_PIXELS_COUNT_THRESHOLD pixels are different, to discard small differences like leaves in the wind and birds.
                const bool frame_interesting = pixels_different >= DIFFERENT_PIXELS_COUNT_THRESHOLD;

                // Filter 3: output is only generated if 3 out of the last 10 frames are different, to discard transient dazzle.
                interesting_frames.append(frame_interesting);
                const size_t interesting_count = interesting_frames.count_where([](const bool &value) {
                    return value == true;
                });

                if (pixels_different > 0 || interesting_count > 0) {
                    fprintf(stderr, "%d: %d%s\n", video_frame_total_index, pixels_different, frame_interesting ? " ***" : "");
                    fprintf(stderr, "%s\n", histogram.description().c_str());
                }

                // As a debugging technique, brand interesting frames with a red box in the upper-right corner.  The branding doesn't affect the Y channel.
                if (frame_interesting) {
                    brand_frame(frame);
                }

                if (interesting_count >= 3 || manual_trigger) {
                    if (output == NULL) {
                        char string_buffer[1024];
                        const time_t t = time(NULL);
                        const struct tm *const lt = localtime(&t);
                        strftime(string_buffer, sizeof (string_buffer), "%Y-%m-%d", lt);
                        const std::string datestamp_string = std::string(string_buffer);
                        strftime(string_buffer, sizeof (string_buffer), "%Y-%m-%dT%H:%M:%S%z", lt);
                        const std::string timestamp_string = std::string(string_buffer);

                        const std::string date_output_dir = output_dir + "/" + datestamp_string;
                        std::filesystem::create_directory(date_output_dir);

#if 0
                        dump_picture_gray8(difference_buffer, frame->width, frame->height, frame->width, date_output_dir + "/" + timestamp_string + "-difference.png");
#endif /* 0 */
                        dump_frame(frame, date_output_dir + "/" + timestamp_string + ".png");

                        char path[] = "/tmp/sophie.mp4.XXXXXX";
                        const int fd = mkstemp(path);
                        assert(fd != -1);
                        int rv = fchmod(fd, 0644);
                        assert(rv == 0);
                        temp_filename = std::string(path);

                        destination_filename = date_output_dir + "/" + timestamp_string + ".mp4";

                        fprintf(stderr, "%d: starting recording%s to %s\n", video_frame_total_index, manual_trigger ? " (manual)" : "", temp_filename.c_str());

                        output = input.create_output(temp_filename);

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
        fprintf(stderr, "END: ending recording; moving to %s\n", destination_filename.c_str());
        output->finish();

        move_file(temp_filename, destination_filename);
        temp_filename.clear();
        destination_filename.clear();

        delete output;
        output = NULL;
        last_motion_timestamp = 0;
    }

    return 0;
}
