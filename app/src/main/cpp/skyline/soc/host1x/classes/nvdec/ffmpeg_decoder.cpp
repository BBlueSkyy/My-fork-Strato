// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <utility>
#include "ffmpeg_decoder.h"

namespace skyline::soc::host1x::nvdec {
    int FfmpegDecoder::AllocateFrame(AVCodecContext *context, AVFrame *frame, int flags) {
        int result{avcodec_default_get_buffer2(context, frame, flags)};
        if (result < 0)
            return result;
        auto &decoder{*static_cast<FfmpegDecoder *>(context->opaque)};
        if (!decoder.captureDecodedSurfaces)
            return 0;

        // Retain the buffer allocated for the picture being decoded. With frame
        // threading disabled, draining receive_frame() to EAGAIN completes the
        // current packet before these shared buffers are consumed by the caller.
        // This preserves H.264 decode order even when presentation is delayed by
        // B-frame reordering. Invisible VP9 reference frames use the same path.
        AVFramePtr decoded{av_frame_clone(frame), [](AVFrame *ptr) { av_frame_free(&ptr); }};
        if (!decoded)
            return AVERROR(ENOMEM);
        decoded->pts = static_cast<i64>(decoder.decodeToken);
        try {
            decoder.decodedSurfaces.push_back(std::move(decoded));
        } catch (const std::bad_alloc &) {
            return AVERROR(ENOMEM);
        }
        return 0;
    }

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

        // Preserve the existing low-delay decoder behavior, but do not rely on it
        // for NVDEC completion: H.264 may still retain B-frames for presentation.
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;

        // NVDEC writes the complete coded surface, including SPS crop margins.
        // Apply the visible crop only after materialization, before VIC handoff.
        context->apply_cropping = 0;
        context->opaque = this;
        context->get_buffer2 = AllocateFrame;

        if (int result{avcodec_open2(context, codec, nullptr)}; result < 0) {
            LOGE("Failed to open the decoder: {}", result);
            avcodec_free_context(&context);
            return false;
        }

        packet = av_packet_alloc();
        return packet != nullptr;
    }

    bool FfmpegDecoder::SendPacket(span<const u8> data, u64 submissionToken, bool hidden) {
        if (!context || !packet)
            return false;

        decodedSurfaces.clear();
        decodeToken = submissionToken;
        // NVDEC writes H.264 picture surfaces in decode order, while libavcodec
        // receive_frame() reports them in presentation order. Capture the decode
        // target independently. VP9 needs this only for invisible reference frames.
        captureDecodedSurfaces = context->codec_id == AV_CODEC_ID_H264 ||
                                 (hidden && context->codec_id == AV_CODEC_ID_VP9);
        packet->data = const_cast<u8 *>(data.data());
        packet->size = static_cast<int>(data.size());
        // Every returned frame identifies its decode destination. Presentation
        // eligibility belongs to the submission, not to the existence of its surface.
        packet->pts = static_cast<i64>(submissionToken);

        if (int result{avcodec_send_packet(context, packet)}; result < 0) {
            captureDecodedSurfaces = false;
            decodedSurfaces.clear();
            LOGW("Failed to send a packet to the decoder: {}", result);
            return false;
        }

        return true;
    }

    AVFramePtr FfmpegDecoder::ReceiveFrame() {
        AVFramePtr frame{av_frame_alloc(), [](AVFrame *ptr) { av_frame_free(&ptr); }};

        if (int result{avcodec_receive_frame(context, frame.get())}; result < 0) {
            captureDecodedSurfaces = false;
            if (result != AVERROR(EAGAIN)) {
                decodedSurfaces.clear();
                LOGW("Failed to receive a frame from the decoder: {}", result);
            }
            return AVFramePtr{nullptr, nullptr};
        }

        return frame;
    }

    std::vector<AVFramePtr> FfmpegDecoder::TakeDecodedSurfaces() {
        captureDecodedSurfaces = false;
        return std::exchange(decodedSurfaces, {});
    }
}
