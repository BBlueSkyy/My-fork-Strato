// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "plugin_image.h"
#include <cstring>
#include <initializer_list>
#include <memory>

namespace skyline::service::jit {
    /** Executes a sysmodule plugin with private code/stack and explicit shared mappings. */
    class PluginContext {
      public:
        explicit PluginContext(std::function<bool()> cancelled = {});
        ~PluginContext();
        void Load(const PluginImage &image);
        void Map(std::uint64_t address, std::span<std::uint8_t> backing, bool writable, bool executable);
        void ResetHeap();
        std::uint64_t Add(const void *data, std::size_t size);
        template<class T> std::uint64_t Add(const T &value) { return Add(&value,sizeof(value)); }
        void Get(std::uint64_t address, void *out, std::size_t size) const;
        template<class T> T Get(std::uint64_t address) const { T value; Get(address,&value,sizeof(value)); return value; }
        std::uint64_t Call(std::uint64_t address, std::initializer_list<std::uint64_t> arguments);
        std::uint64_t Helper(std::string_view name) const;
        static std::uint64_t HelperAddress(std::string_view name, std::uint64_t helperBase);
        std::pair<std::size_t, std::size_t> TakeWrites(std::uint64_t mappingAddress);
      private:
        class Impl;
        std::unique_ptr<Impl> impl;
    };
}
