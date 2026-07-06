// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "ffmpeg_decoder.h"

namespace skyline::soc::host1x::nvdec {
    FfmpegDecoder::~FfmpegDecoder() {
        if (packet)
            av_packet_free(&packet);
        if (context)
            avcodec_free_context(&context);
    }

    bool FfmpegDecoder::Initialize(CodecId codecId) {
        AVCodecID avCodecId{[&] {
            switch (codecId) {
                case CodecId::H264:
                    return AV_CODEC_ID_H264;
                case CodecId::Vp8:
                    return AV_CODEC_ID_VP8;
                case CodecId::Vp9:
                    return AV_CODEC_ID_VP9;
                default:
                    return AV_CODEC_ID_NONE;
            }
        }()};

        if (avCodecId == AV_CODEC_ID_NONE)
            return false;

        const AVCodec *codec{avcodec_find_decoder(avCodecId)};
        if (!codec) {
            LOGE("Failed to find a decoder for codec: {}", static_cast<u64>(codecId));
            return false;
        }

        context = avcodec_alloc_context3(codec);
        if (!context)
            return false;

        // Frame threading introduces a delay of several frames between submission and output which would
        // mismatch decoded frames with their target surfaces, slice threading has no such delay
        context->thread_count = 0;
        context->thread_type &= ~FF_THREAD_FRAME;

        // Hardware decodes in bitstream order and the guest handles display reordering itself, low delay
        // stops libavcodec buffering reordered frames so every submitted packet yields its frame immediately
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (int result{avcodec_open2(context, codec, nullptr)}; result < 0) {
            LOGE("Failed to open the decoder: {}", result);
            avcodec_free_context(&context);
            return false;
        }

        packet = av_packet_alloc();
        return packet != nullptr;
    }

    bool FfmpegDecoder::SendPacket(span<const u8> data, u64 surfaceKey) {
        if (!context || !packet)
            return false;

        packet->data = const_cast<u8 *>(data.data());
        packet->size = static_cast<int>(data.size());
        // The PTS is echoed onto the decoded frame even across reordering, letting frames be matched to the surface their submission targeted
        packet->pts = static_cast<i64>(surfaceKey);

        if (int result{avcodec_send_packet(context, packet)}; result < 0) {
            LOGW("Failed to send a packet to the decoder: {}", result);
            return false;
        }

        return true;
    }

    AVFramePtr FfmpegDecoder::ReceiveFrame() {
        AVFramePtr frame{av_frame_alloc(), [](AVFrame *ptr) { av_frame_free(&ptr); }};

        if (int result{avcodec_receive_frame(context, frame.get())}; result < 0) {
            if (result != AVERROR(EAGAIN))
                LOGW("Failed to receive a frame from the decoder: {}", result);
            return AVFramePtr{nullptr, nullptr};
        }

        return frame;
    }
}
