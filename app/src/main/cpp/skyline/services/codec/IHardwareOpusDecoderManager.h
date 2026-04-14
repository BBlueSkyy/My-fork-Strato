// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/base_service.h>

namespace skyline::service::codec {
    /**
     * @brief Initialization parameters for the Opus multi-stream decoder
     * @see opus_multistream_decoder_init()
     */
    struct MultiStreamParameters {
        i32 sampleRate;
        i32 channelCount;
        i32 streamCount;
        i32 stereoStreamCount;
        std::array<u8, 0x100> mappings; //!< Array of channel mappings
    };
    static_assert(sizeof(MultiStreamParameters) == 0x110);

    /**
     * @brief Manages all instances of IHardwareOpusDecoder
     * @url https://switchbrew.org/wiki/Audio_services#hwopus
     */
    class IHardwareOpusDecoderManager : public BaseService {
      public:
        IHardwareOpusDecoderManager(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

        /**
         * @brief Returns an IHardwareOpusDecoder object
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoder
         */
        Result OpenHardwareOpusDecoder(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required size for the decoder's work buffer
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSize
         */
        Result GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns an IHardwareOpusDecoder object [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoder
         */
        Result OpenHardwareOpusDecoderEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns an IHardwareOpusDecoder object for multi-stream decoding
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoderForMultiStream
         */
        Result OpenHardwareOpusDecoderForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required work buffer size for multi-stream decoding
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeForMultiStream
         */
        Result GetWorkBufferSizeForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required size for the decoder's work buffer [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeEx
         */
        Result GetWorkBufferSizeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns an IHardwareOpusDecoder object for multi-stream decoding [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#OpenHardwareOpusDecoderForMultiStreamEx
         */
        Result OpenHardwareOpusDecoderForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required work buffer size for multi-stream decoding [12.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeForMultiStreamEx
         */
        Result GetWorkBufferSizeForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required work buffer size with extended parameters [16.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeExEx
         */
        Result GetWorkBufferSizeExEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        /**
         * @brief Returns the required work buffer size for multi-stream decoding with extended parameters [16.0.0+]
         * @url https://switchbrew.org/wiki/Audio_services#GetWorkBufferSizeForMultiStreamExEx
         */
        Result GetWorkBufferSizeForMultiStreamExEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Cmd100(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IHardwareOpusDecoderManager, OpenHardwareOpusDecoder),
            SFUNC(0x1, IHardwareOpusDecoderManager, GetWorkBufferSize),
            SFUNC(0x2, IHardwareOpusDecoderManager, OpenHardwareOpusDecoderForMultiStream),
            SFUNC(0x3, IHardwareOpusDecoderManager, GetWorkBufferSizeForMultiStream),
            SFUNC(0x4, IHardwareOpusDecoderManager, OpenHardwareOpusDecoderEx),
            SFUNC(0x5, IHardwareOpusDecoderManager, GetWorkBufferSizeEx),
            SFUNC(0x6, IHardwareOpusDecoderManager, OpenHardwareOpusDecoderForMultiStreamEx),
            SFUNC(0x7, IHardwareOpusDecoderManager, GetWorkBufferSizeForMultiStreamEx),
            SFUNC(0x8, IHardwareOpusDecoderManager, GetWorkBufferSizeExEx),
            SFUNC(0x9, IHardwareOpusDecoderManager, GetWorkBufferSizeForMultiStreamExEx),
            SFUNC(0x64, IHardwareOpusDecoderManager, Cmd100)
        )
    };
}
