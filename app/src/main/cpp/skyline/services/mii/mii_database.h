// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

#include <common.h>

namespace skyline::service::mii {
    constexpr u32 DatabaseSourceFlag{1U << 0};
    constexpr u32 DefaultSourceFlag{1U << 1};
    constexpr size_t MaxMiiCount{100};

    enum class Source : u32 {
        Database = 0,
        Default = 1,
    };

    struct CharInfo {
        std::array<u8, 0x10> createId{};
        std::array<char16_t, 10> name{};
        u16 nullTerminator{};
        u8 fontRegion{};
        u8 favoriteColor{};
        u8 gender{};
        u8 height{};
        u8 build{};
        u8 type{};
        u8 regionMove{};
        u8 facelineType{};
        u8 facelineColor{};
        u8 facelineWrinkle{};
        u8 facelineMake{};
        u8 hairType{};
        u8 hairColor{};
        u8 hairFlip{};
        u8 eyeType{};
        u8 eyeColor{};
        u8 eyeScale{};
        u8 eyeAspect{};
        u8 eyeRotate{};
        u8 eyeX{};
        u8 eyeY{};
        u8 eyebrowType{};
        u8 eyebrowColor{};
        u8 eyebrowScale{};
        u8 eyebrowAspect{};
        u8 eyebrowRotate{};
        u8 eyebrowX{};
        u8 eyebrowY{};
        u8 noseType{};
        u8 noseScale{};
        u8 noseY{};
        u8 mouthType{};
        u8 mouthColor{};
        u8 mouthScale{};
        u8 mouthAspect{};
        u8 mouthY{};
        u8 beardColor{};
        u8 beardType{};
        u8 mustacheType{};
        u8 mustacheScale{};
        u8 mustacheY{};
        u8 glassType{};
        u8 glassColor{};
        u8 glassScale{};
        u8 glassY{};
        u8 moleType{};
        u8 moleScale{};
        u8 moleX{};
        u8 moleY{};
        u8 padding{};
    };
    static_assert(sizeof(CharInfo) == 0x58);

    struct CharInfoElement {
        CharInfo charInfo{};
        Source source{};
    };
    static_assert(sizeof(CharInfoElement) == 0x5C);

    class MiiDatabase {
      public:
        enum class AppendResult {
            Success,
            Full,
            InvalidSpecial,
        };

      private:
        struct FileHeader {
            std::array<char, 4> magic{'S', 'M', 'D', 'B'};
            u32 version{1};
            u32 count{};
            u32 reserved{};
        };
        static_assert(sizeof(FileHeader) == 0x10);

        std::filesystem::path backingPath;
        mutable std::mutex mutex;
        std::vector<CharInfo> entries;
        u64 updateCounter{};

        bool Load();
        bool SaveLocked() const;
        bool ContainsCreateIdLocked(const std::array<u8, 0x10> &createId) const;
        static void GenerateCreateId(std::array<u8, 0x10> &createId);

      public:
        explicit MiiDatabase(const DeviceState &state);

        u64 GetUpdateCounter() const;
        u32 GetCount() const;
        bool IsFull() const;
        std::vector<CharInfo> Snapshot() const;
        std::optional<CharInfo> FindByCreateId(const std::array<u8, 0x10> &createId) const;
        s32 FindIndex(const std::array<u8, 0x10> &createId) const;
        AppendResult Append(const CharInfo &charInfo);
    };
}
