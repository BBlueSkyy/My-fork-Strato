// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <concepts>
#include <system_error>
#include <common.h>

namespace skyline::vfs {
    /**
     * @brief The Backing class provides abstract access to a storage device, all access can be done without using a specific backing
     */
    class Backing {
      protected:
        virtual size_t ReadImpl(span <u8> output, size_t offset) = 0;

        virtual std::pair<size_t, std::error_code> WriteWithErrorImpl(span<u8>, size_t) {
            return {0, std::make_error_code(std::errc::operation_not_supported)};
        }

        virtual std::error_code ResizeWithErrorImpl(size_t) {
            return std::make_error_code(std::errc::operation_not_supported);
        }

        virtual std::error_code FlushImpl() { return std::make_error_code(std::errc::operation_not_supported); }

      public:
        union Mode {
            struct {
                bool read : 1; //!< The backing is readable
                bool write : 1; //!< The backing is writable
                bool append : 1; //!< The backing can be appended
            };
            u32 raw;
        } mode;
        static_assert(sizeof(Mode) == 0x4);

        size_t size; //!< The size of the backing in bytes

        /**
         * @param mode The mode to use for the backing
         * @param size The initial size of the backing
         */
        Backing(Mode mode = {true, false, false}, size_t size = 0) : mode(mode), size(size) {}

        /* Delete the move constructor to prevent multiple instances of the same backing */
        Backing(const Backing &) = delete;

        Backing &operator=(const Backing &) = delete;

        virtual ~Backing() = default;

        /**
         * @brief Read bytes from the backing at a particular offset to a buffer
         * @param output The object to write the data read to
         * @param offset The offset to start reading from
         * @return The amount of bytes read
         */
        size_t ReadUnchecked(span <u8> output, size_t offset = 0) {
            if (!mode.read)
                LOGW("Attempting to read a backing that is not readable");

            return ReadImpl(output, offset);
        };

        /**
         * @brief Read bytes from the backing at a particular offset to a buffer and check to ensure the full size was read
         * @param output The object to write the data read to
         * @param offset The offset to start reading from
         * @return The amount of bytes read
         */
        size_t Read(span <u8> output, size_t offset = 0) {
            if (offset > size)
                throw exception("Offset cannot be past the end of a backing");

            if ((size - offset) < output.size())
                throw exception("Trying to read past the end of a backing: 0x{:X}/0x{:X} (Offset: 0x{:X})", output.size(), size, offset);

            size_t read{ReadUnchecked(output, offset)};
            if (read != output.size())
                LOGW("Failed to read the requested size from backing");

            return read;
        };

        std::pair<size_t, std::error_code> ReadWithError(span<u8> output, size_t offset = 0) {
            if (!mode.read)
                return {0, std::make_error_code(std::errc::permission_denied)};
            if (offset > size || output.size() > size - offset)
                return {0, std::make_error_code(std::errc::result_out_of_range)};
            if (output.empty())
                return {0, {}};

            try {
                const auto read{ReadImpl(output, offset)};
                if (read != output.size())
                    return {read, std::make_error_code(std::errc::io_error)};
                return {read, {}};
            } catch (...) {
                return {0, std::make_error_code(std::errc::io_error)};
            }
        }

        /**
         * @brief Implicit casting for reading into spans of different types
         */
        template<typename T> requires (!std::same_as<T, u8>)
        size_t Read(span <T> output, size_t offset = 0) {
            return Read(output.template cast<u8>(), offset);
        }

        /**
         * @brief Read bytes from the backing at a particular offset into an object
         * @param offset The offset to start reading from
         * @return The object that was read
         */
        template<typename T>
        T Read(size_t offset = 0) {
            T object;
            Read(span(reinterpret_cast<u8 *>(&object), sizeof(T)), offset);
            return object;
        }

        /**
         * @brief Writes from a buffer to a particular offset in the backing
         * @param input The data to write to the backing
         * @param offset The offset where the input buffer should be written
         * @return The amount of bytes written
         */
        std::pair<size_t, std::error_code> WriteWithError(span<u8> input, size_t offset = 0) {
            if (!mode.write)
                return {0, std::make_error_code(std::errc::read_only_file_system)};
            if (offset > size || input.size() > size - offset)
                return {0, std::make_error_code(std::errc::result_out_of_range)};
            if (input.empty())
                return {0, {}};
            return WriteWithErrorImpl(input, offset);
        }

        size_t Write(span <u8> input, size_t offset = 0) {
            if (offset > size || input.size() > size - offset) {
                if (!mode.append || input.size() > std::numeric_limits<size_t>::max() - offset)
                    throw exception("Trying to write past the end of a non-appendable backing");
                Resize(offset + input.size());
            }

            auto [written, error]{WriteWithError(input, offset)};
            if (error)
                throw exception("Failed to write backing: {}", error.message());
            return written;
        }

        /**
         * @brief Writes from an object into a particular offset in the backing
         * @param object The object to write to the backing
         * @param offset The offset where the input should be written
         */
        template<typename T>
        void WriteObject(const T &object, size_t offset = 0) {
            size_t lSize;
            if ((lSize = Write(span(reinterpret_cast<u8 *>(&object), sizeof(T)), offset)) != sizeof(T))
                LOGW("Object wasn't written fully into output backing: {}/{}", lSize, sizeof(T));
        }

        /**
         * @brief Resizes a backing to the given size
         * @param pSize The new size for the backing
         */
        std::error_code ResizeWithError(size_t pSize) {
            if (!mode.write)
                return std::make_error_code(std::errc::read_only_file_system);
            return ResizeWithErrorImpl(pSize);
        }

        void Resize(size_t pSize) {
            const auto error{ResizeWithError(pSize)};
            if (error)
                throw exception("Failed to resize backing: {}", error.message());
        }

        std::error_code Flush() { return FlushImpl(); }
    };
}
