/**
 * Copyright 2013-2014, Dominik Schnitzer <dominik@schnitzer.at>
 *           2014-2016, Jan Schlueter <jan.schlueter@ofai.at>
 *                2026, Musly maintainers
 *
 * This file is part of Musly, a program for high performance music
 * similarity computation: http://www.musly.org/.
 *
 * This Source Code Form is subject to the terms of the Mozilla
 * Public License v. 2.0. If a copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS

#include <inttypes.h>
#include <stdint.h>
#include <cstdarg>
#include <vector>
#include <algorithm>
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
#ifdef HAVE_AVUTIL_CHANNEL_LAYOUT
    #include <libavutil/channel_layout.h>
#endif
}

#include "minilog.h"
#include "resampler.h"
#include "libav.h"

// We define some macros to be compatible to different libav versions
// without spreading #if and #else all over the place.
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 45, 101)
#define AV_FRAME_ALLOC avcodec_alloc_frame
#define AV_FRAME_UNREF avcodec_get_frame_defaults
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(54, 28, 0)
#define AV_FRAME_FREE(X) av_free(*(X))
#else
#define AV_FRAME_FREE avcodec_free_frame
#endif
#else
#define AV_FRAME_ALLOC av_frame_alloc
#define AV_FRAME_UNREF av_frame_unref
#define AV_FRAME_FREE av_frame_free
#endif

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 7, 0)
#define AV_PACKET_UNREF av_free_packet
#else
#define AV_PACKET_UNREF av_packet_unref
#endif

// FFmpeg 5+: avcodec_find_decoder returns const AVCodec*
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 0, 100)
typedef const AVCodec MuslyAVCodec;
#else
typedef AVCodec MuslyAVCodec;
#endif

// FFmpeg 6+: av_init_packet removed; use heap-allocated packets
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 0, 100)
#define MUSLY_USE_PACKET_ALLOC 1
#else
#define MUSLY_USE_PACKET_ALLOC 0
#endif

// FFmpeg 7+: channels / channel_layout replaced by ch_layout
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 100)
#define MUSLY_CHANNELS(ctx) ((ctx)->ch_layout.nb_channels)
#define MUSLY_HAS_REQUEST_CHANNEL_LAYOUT 0
#else
#define MUSLY_CHANNELS(ctx) ((ctx)->channels)
#define MUSLY_HAS_REQUEST_CHANNEL_LAYOUT 1
#endif

// send/receive API available from libavcodec 57.48.101
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
#define MUSLY_USE_SEND_RECEIVE 1
#else
#define MUSLY_USE_SEND_RECEIVE 0
#endif

namespace musly {
namespace decoders {

MUSLY_DECODER_REGIMPL(libav, 0);

libav::libav()
{
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    avcodec_register_all();
#endif
}

int
libav::samples_tofloat(
        void* const out,
        const void* const in,
        const int out_stride,
        const int in_stride,
        const AVSampleFormat in_fmt,
        int len)
{
    const int is = in_stride;
    const int os = out_stride;
    const uint8_t* pi = (uint8_t*)in;
    uint8_t* po = (uint8_t*)out;
    uint8_t* end = po + os*len;

#define CONVFLOAT(ifmt, ifmtp, expr)\
if (in_fmt == ifmt || in_fmt == ifmtp) {\
    do{\
        *(float*)po = expr; pi += is; po += os;\
    } while(po < end);\
}
    // Implementation note: We could use av_get_packed_sample_fmt
    // to avoid checking for two formats each time, but we want to
    // stay compatible to libav versions that do not have it yet.
    CONVFLOAT(AV_SAMPLE_FMT_U8, AV_SAMPLE_FMT_U8P,
            (*(const uint8_t*)pi - 0x80)*(1.0 / (1<<7)))
    else CONVFLOAT(AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P,
            *(const int16_t*)pi*(1.0 / (1<<15)))
    else CONVFLOAT(AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S32P,
            *(const int32_t*)pi*(1.0 / (1U<<31)))
    else CONVFLOAT(AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
            *(const float*)pi)
    else CONVFLOAT(AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP,
            *(const double*)pi)
    else return -1;

    return 0;
}

void libav_log_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
    (void)ptr;
    if (level <= av_log_get_level()) {
#if __cplusplus > 199711L
        va_list vargs_copy;
        va_copy(vargs_copy, vargs);
        int len = vsnprintf(NULL, 0, fmt, vargs_copy);
        va_end(vargs_copy);
        // Note: len does not include the terminating '\0' character.
        // We intentionally make the buffer one character too short
        // to avoid including the end-of-line character of libav.
        if (len > 0) {
            char *buf = new char[len];
            vsnprintf(buf, len, fmt, vargs);
            MINILOG(logTRACE) << "libav: " << buf;
            delete[] buf;
        }
#else
        vfprintf(FileLogger::get_stream(), fmt, vargs);
#endif
    }
}

std::vector<float>
libav::decodeto_22050hz_mono_float(
        const std::string& file,
        float excerpt_length,
        float excerpt_start)
{
    MINILOG(logTRACE) << "Decoding: " << file << " started.";

    const int target_rate = 22050;
    int avret;

    // show libav messages only in verbose mode
    if (MiniLog::current_level() >= logTRACE) {
        av_log_set_level(AV_LOG_VERBOSE);
        av_log_set_callback(libav_log_callback);
    }
    else {
        av_log_set_level(AV_LOG_PANIC);
    }

    // guess input format
    AVFormatContext* fmtx = NULL;
    avret = avformat_open_input(&fmtx, file.c_str(), NULL, NULL);
    if (avret < 0) {
        MINILOG(logERROR) << "Could not open file, or detect file format";
        return std::vector<float>(0);
    }

    // retrieve stream information
#ifdef _OPENMP
    #pragma omp critical
#endif
    {
    avret = avformat_find_stream_info(fmtx, NULL);
    }
    if (avret < 0) {
        MINILOG(logERROR) << "Could not find stream info";

        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }

    // if there are multiple audio streams, find the best one..
    int audio_stream_idx =
            av_find_best_stream(fmtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio_stream_idx < 0) {
        MINILOG(logERROR) << "Could not find audio stream in input file";

        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }
    AVStream *st = fmtx->streams[audio_stream_idx];

    // find a decoder for the stream
#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 14, 0)) || ((LIBAVCODEC_VERSION_MICRO >= 100) && (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 33, 100)))
    // old libav version (libavcodec < 57.14 for libav, < 57.33 for ffmpeg):
    // stream has a codec context we can use
    AVCodecContext *decx = st->codec;
    #define AVCODEC_FREE_CONTEXT(x)
#else
    // new libav version: need to create codec context for stream
    AVCodecParameters *decp = st->codecpar;
    AVCodecContext *decx = avcodec_alloc_context3(NULL);
    if (!decx) {
        MINILOG(logERROR) << "Could not allocate codec context";

        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }
    avret = avcodec_parameters_to_context(decx, decp);
    if (avret < 0) {
        MINILOG(logERROR) << "Could not set codec context";

        avcodec_free_context(&decx);
        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }
    #if LIBAVCODEC_VERSION_MICRO >= 100
    #if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,3,102)
    // only available in ffmpeg, deprecated after 58
    av_codec_set_pkt_timebase(decx, st->time_base);
    #endif
    #endif
    #define AVCODEC_FREE_CONTEXT(x) avcodec_free_context(x)
#endif
    MuslyAVCodec *dec = avcodec_find_decoder(decx->codec_id);
    if (!dec) {
        MINILOG(logERROR) << "Could not find codec.";

        AVCODEC_FREE_CONTEXT(&decx);
        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }

    // open the decoder
    // (kindly ask for stereo downmix and floats, but not all decoders care)
#if MUSLY_HAS_REQUEST_CHANNEL_LAYOUT
    decx->request_channel_layout = AV_CH_LAYOUT_STEREO_DOWNMIX;
#endif
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 0, 100)
    decx->request_sample_fmt = AV_SAMPLE_FMT_FLT;
#endif
#ifdef _OPENMP
    #pragma omp critical
#endif
    {
    avret = avcodec_open2(decx, dec, NULL);
    }
    if (avret < 0) {
        MINILOG(logERROR) << "Could not open codec.";

        AVCODEC_FREE_CONTEXT(&decx);
        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }

    // Currently only mono and stereo files are supported.
    if ((MUSLY_CHANNELS(decx) != 1) && (MUSLY_CHANNELS(decx) != 2)) {
        MINILOG(logWARNING) << "Unsupported number of channels: "
                << MUSLY_CHANNELS(decx);

        AVCODEC_FREE_CONTEXT(&decx);
        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }

    // allocate a frame
    AVFrame* frame = AV_FRAME_ALLOC();
    if (!frame) {
        MINILOG(logWARNING) << "Could not allocate frame";

        AVCODEC_FREE_CONTEXT(&decx);
        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }

    // allocate and initialize a packet
#if MUSLY_USE_PACKET_ALLOC
    AVPacket* pkt_ptr = av_packet_alloc();
    if (!pkt_ptr) {
        MINILOG(logWARNING) << "Could not allocate packet";
        AV_FRAME_FREE(&frame);
        AVCODEC_FREE_CONTEXT(&decx);
        avformat_close_input(&fmtx);
        return std::vector<float>(0);
    }
    AVPacket& pkt = *pkt_ptr;
#else
    AVPacket pkt_storage;
    av_init_packet(&pkt_storage);
    pkt_storage.data = NULL;
    pkt_storage.size = 0;
    AVPacket& pkt = pkt_storage;
#endif

    // configuration
    const int input_stride = av_get_bytes_per_sample(decx->sample_fmt);
    const int num_planes = av_sample_fmt_is_planar(decx->sample_fmt)
            ? MUSLY_CHANNELS(decx) : 1;
    const int output_stride = sizeof(float) * num_planes;
    int decode_samples;  // how many samples to decode; zero to decode all

    if (st->duration) {  // if the file length is (at least approximately) known:
        float file_length = (float)st->duration * st->time_base.num / st->time_base.den;

        // adjust excerpt boundaries
        if ((excerpt_length <= 0) || (excerpt_length > file_length)) {
            // use full file
            excerpt_length = 0;
            excerpt_start = 0;
        }
        else if (excerpt_start < 0) {
            // center in file, but start at -excerpt_start the latest
            excerpt_start = std::min(-excerpt_start,
                    (file_length - excerpt_length) / 2);
        }
        else if (excerpt_start + excerpt_length > file_length) {
            // right-align excerpt
            excerpt_start = file_length - excerpt_length;
        }

        MINILOG(logDEBUG) << "Audio file length: " << file_length <<
                " seconds. Will decode from " << excerpt_start << " to " <<
                (excerpt_length > 0 ? (excerpt_start + excerpt_length) : file_length);

        // try to skip to requested position in stream
        // (Note: without AVSEEK_FLAG_BACKWARD, some MP3s cause libav to skip
        // to a position where it sees mono MP1 frames, causing a segmentation
        // fault when trying to access frame->data[i] for i > 0 further below)
        if ((excerpt_start > 0) && (av_seek_frame(fmtx, audio_stream_idx,
                    (int64_t)(excerpt_start * st->time_base.den / st->time_base.num),
                    AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY) >= 0)) {
            // skipping went fine: decode only what's needed
            decode_samples = excerpt_length * decx->sample_rate;
            excerpt_start = 0;
            avcodec_flush_buffers(decx);
        }
        else {
            if (excerpt_start > 0) {
                MINILOG(logDEBUG) << "Could not seek in audio file.";
            }
            // skipping failed or not needed: decode from beginning
            decode_samples = (excerpt_start + excerpt_length) * decx->sample_rate;
        }
    }
    else {  // if the file length is unknown:
        MINILOG(logDEBUG) << "Audio file length: unknown";
        if (excerpt_length <= 0) {
            // use full file
            excerpt_length = 0;
            excerpt_start = 0;
            decode_samples = 0;
        }
        else if (excerpt_start < 0) {
            // center in file, but start at -excerpt_start the latest,
            // so decode at most -excerpt_start+excerpt_length seconds
            decode_samples = (-excerpt_start + excerpt_length) * decx->sample_rate;
        }
        else {
            // uncentered excerpt: decode from beginning, cut out afterwards
            decode_samples = (excerpt_start + excerpt_length) * decx->sample_rate;
        }
    }
    // After this lengthy adjustment, decode_samples tells us how many samples
    // to decode at most (where zero means to decode the full file), and
    // excerpt_start tells us up to how many seconds to cut from the beginning.

    // read packets
    const int channels = MUSLY_CHANNELS(decx);
    const int sample_rate = decx->sample_rate;
    float* buffer = NULL;
    int buffersize = 0;
    std::vector<float> decoded_pcm;
    int subsequent_errors = 0;
    const int subsequent_errors_max = 20;

    auto process_frame = [&]() -> bool {
        int input_samples = frame->nb_samples * MUSLY_CHANNELS(decx);
        if (input_samples > buffersize) {
            delete[] buffer;
            buffer = new float[input_samples];
            buffersize = input_samples;
        }

        for (int i = 0; i < num_planes; i++) {
            if (samples_tofloat(buffer + i, frame->data[i],
                    output_stride, input_stride,
                    decx->sample_fmt,
                    input_samples / num_planes) < 0) {
                MINILOG(logERROR) << "Strange sample format. Abort.";
                return false;
            }
        }

        if (MUSLY_CHANNELS(decx) == 2) {
            for (int i = 0; i < frame->nb_samples; i++) {
                buffer[i] = (buffer[i*2] + buffer[i*2+1]) / 2.0f;
            }
        }

        decoded_pcm.insert(decoded_pcm.end(), buffer,
                buffer + frame->nb_samples);
        return true;
    };

    auto abort_decode = [&]() -> std::vector<float> {
        AV_FRAME_FREE(&frame);
        AV_PACKET_UNREF(&pkt);
#if MUSLY_USE_PACKET_ALLOC
        av_packet_free(&pkt_ptr);
#endif
        avformat_close_input(&fmtx);
        delete[] buffer;
        AVCODEC_FREE_CONTEXT(&decx);
        return std::vector<float>(0);
    };

    while ((decode_samples == 0) || ((int)decoded_pcm.size() < decode_samples))
    {
        // skip all frames that are not part of the audio stream, and spurious
        // frames possibly found after seeking (wrong channels / sample_rate)
        while (((avret = av_read_frame(fmtx, &pkt)) >= 0)
               && ((pkt.stream_index != audio_stream_idx) ||
                   (MUSLY_CHANNELS(decx) != channels) ||
                   (decx->sample_rate != sample_rate)))
        {
            AV_PACKET_UNREF(&pkt);
            MINILOG(logTRACE) << "Skipping frame...";
        }
        if (avret < 0) {
            // stop reading; drain decoder below
            AV_PACKET_UNREF(&pkt);
            break;
        }

#if MUSLY_USE_SEND_RECEIVE
        avret = avcodec_send_packet(decx, &pkt);
        AV_PACKET_UNREF(&pkt);
        if (avret < 0 && avret != AVERROR(EAGAIN) && avret != AVERROR_EOF) {
            MINILOG(logWARNING) << "Error sending an audio packet";
            if (subsequent_errors < subsequent_errors_max) {
                subsequent_errors++;
                continue;
            }
            MINILOG(logERROR) << "Too many errors, aborting.";
            return abort_decode();
        }

        while (true) {
            AV_FRAME_UNREF(frame);
            avret = avcodec_receive_frame(decx, frame);
            if (avret == AVERROR(EAGAIN) || avret == AVERROR_EOF) {
                break;
            }
            if (avret < 0) {
                MINILOG(logWARNING) << "Error decoding an audio frame";
                if (subsequent_errors < subsequent_errors_max) {
                    subsequent_errors++;
                    break;
                }
                MINILOG(logERROR) << "Too many errors, aborting.";
                return abort_decode();
            }
            subsequent_errors = 0;
            if (!process_frame()) {
                return abort_decode();
            }
            if ((decode_samples != 0) &&
                    ((int)decoded_pcm.size() >= decode_samples)) {
                break;
            }
        }
#else
        uint8_t* data = pkt.data;
        int size = pkt.size;
        while (pkt.size > 0) {
            AV_FRAME_UNREF(frame);

            int len = 0;
            int got_frame = 0;
            len = avcodec_decode_audio4(decx, frame, &got_frame, &pkt);
            if (len < 0) {
                avret = AVERROR(EINVAL);
            } else {
                avret = 0;
            }

            if (avret < 0) {
                MINILOG(logWARNING) << "Error decoding an audio frame";

                if (subsequent_errors < subsequent_errors_max) {
                    subsequent_errors++;
                    break;
                }

                MINILOG(logERROR) << "Too many errors, aborting.";
                return abort_decode();
            } else {
                subsequent_errors = 0;
            }

            if (got_frame) {
                if (!process_frame()) {
                    return abort_decode();
                }
            }

            pkt.data += len;
            pkt.size -= len;
        }
        pkt.data = data;
        pkt.size = size;

        AV_PACKET_UNREF(&pkt);
#endif
    }

#if MUSLY_USE_SEND_RECEIVE
    // Drain remaining frames buffered in the decoder.
    avcodec_send_packet(decx, NULL);
    while (true) {
        AV_FRAME_UNREF(frame);
        avret = avcodec_receive_frame(decx, frame);
        if (avret == AVERROR(EAGAIN) || avret == AVERROR_EOF || avret < 0) {
            break;
        }
        if (!process_frame()) {
            return abort_decode();
        }
        if ((decode_samples != 0) &&
                ((int)decoded_pcm.size() >= decode_samples)) {
            break;
        }
    }
#endif

    MINILOG(logTRACE) << "Decoding loop finished.";

    // cut out the requested excerpt if needed
    int skip_samples = 0;
    if (excerpt_start < 0) {
        // center excerpt, but start at -excerpt_start the latest
        float file_length = (float)decoded_pcm.size() / (float)decx->sample_rate;
        if (file_length > excerpt_length) {
            // skip beginning as needed
            excerpt_start = std::min(-excerpt_start,
                    (file_length - excerpt_length) / 2);
            skip_samples = excerpt_start * decx->sample_rate;
            // truncate end if needed
            int target_samples = skip_samples + excerpt_length * decx->sample_rate;
            if (target_samples < (int)decoded_pcm.size()) {
                decoded_pcm.resize(target_samples);
            }
        }
    }
    else if (excerpt_length > 0) {
        // truncate to target length if needed
        if ((int)decoded_pcm.size() > decode_samples) {
            decoded_pcm.resize(decode_samples);
        }
        // skip beginning if needed
        if (excerpt_start > 0) {
            skip_samples = excerpt_start * decx->sample_rate;
            int missed_samples = decode_samples - (int)decoded_pcm.size();
            skip_samples = std::max(0, skip_samples - missed_samples);
        }
    }

    // do we need to resample?
    std::vector<float> pcm;
    if (target_rate != decx->sample_rate) {
        MINILOG(logTRACE) << "Resampling signal. input="
                << decx->sample_rate << ", target=" << target_rate;
        resampler r(decx->sample_rate, target_rate);
        pcm = r.resample(decoded_pcm.data() + skip_samples,
                (int)decoded_pcm.size() - skip_samples);
        MINILOG(logTRACE) << "Resampling finished.";
    } else {
        pcm.resize(decoded_pcm.size() - skip_samples);
        std::copy(decoded_pcm.begin() + skip_samples, decoded_pcm.end(), pcm.begin());
    }

    // cleanup
    delete[] buffer;
    AV_FRAME_FREE(&frame);
#if MUSLY_USE_PACKET_ALLOC
    av_packet_free(&pkt_ptr);
#endif
#ifdef _OPENMP
    #pragma omp critical
#endif
    {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 0, 100)
    // avcodec_close is deprecated; still needed when AVCODEC_FREE_CONTEXT is a no-op
    avcodec_close(decx);
#endif
    AVCODEC_FREE_CONTEXT(&decx);
    avformat_close_input(&fmtx);
    }

    MINILOG(logTRACE) << "Decoding: " << file << " finalized.";
    return pcm;
}

} /* namespace decoders */
} /* namespace musly */
