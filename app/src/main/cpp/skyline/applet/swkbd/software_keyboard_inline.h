// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

#pragma once

#include <array>
#include <common.h>

namespace skyline::applet::swkbd::inline_protocol {
    enum class State : u32 {
        NotInitialized = 0x0,
        InitializedIsHidden = 0x1,
        InitializedIsAppearing = 0x2,
        InitializedIsShown = 0x3,
        InitializedIsDisappearing = 0x4,
    };

    enum class Request : u32 {
        Finalize = 0x4,
        SetUserWordInfo = 0x6,
        SetCustomizeDic = 0x7,
        Calc = 0xA,
        SetCustomizedDictionaries = 0xB,
        UnsetCustomizedDictionaries = 0xC,
        SetChangedStringV2Flag = 0xD,
        SetMovedCursorV2Flag = 0xE,
    };

    enum class Reply : u32 {
        FinishedInitialize = 0x0,
        Default = 0x1,
        ChangedString = 0x2,
        MovedCursor = 0x3,
        MovedTab = 0x4,
        DecidedEnter = 0x5,
        DecidedCancel = 0x6,
        ChangedStringUtf8 = 0x7,
        MovedCursorUtf8 = 0x8,
        DecidedEnterUtf8 = 0x9,
        UnsetCustomizeDic = 0xA,
        ReleasedUserWordInfo = 0xB,
        UnsetCustomizedDictionaries = 0xC,
        ChangedStringV2 = 0xD,
        MovedCursorV2 = 0xE,
        ChangedStringUtf8V2 = 0xF,
        MovedCursorUtf8V2 = 0x10,
    };

    constexpr u64 SetInitializeArg{1ULL << 0};
    constexpr u64 SetVolume{1ULL << 1};
    constexpr u64 Appear{1ULL << 2};
    constexpr u64 SetInputText{1ULL << 3};
    constexpr u64 SetCursorPosition{1ULL << 4};
    constexpr u64 SetUtf8Mode{1ULL << 5};
    constexpr u64 UnsetCustomizeDic{1ULL << 6};
    constexpr u64 Disappear{1ULL << 7};
    constexpr u64 SetKeyTopTranslateScale{1ULL << 9};
    constexpr u64 UnsetUserWordInfo{1ULL << 10};
    constexpr u64 SetDisableHardwareKeyboard{1ULL << 11};

    constexpr size_t Utf16TextBytes{0x3EC};
    constexpr size_t Utf8TextBytes{0x7D4};

#pragma pack(push, 1)
    struct InitializeArg {
        u32 unknown{};
        u8 mode{};
        u8 isAboveHos500{};
        std::array<u8, 2> padding{};
    };
    static_assert(sizeof(InitializeArg) == 0x8);

    struct AppearArgOld {
        u32 type{};
        std::array<char16_t, 9> okText{};
        char16_t leftOptionalSymbolKey{};
        char16_t rightOptionalSymbolKey{};
        u8 usePrediction{};
        u8 disableCancelButton{};
        u32 keyDisableFlags{};
        u32 maxTextLength{};
        u32 minTextLength{};
        u8 enableReturnButton{};
        std::array<u8, 3> padding0{};
        u32 flags{};
        u8 isUseSaveData{};
        std::array<u8, 7> padding1{};
        std::array<u8, 16> userId{};
    };
    static_assert(sizeof(AppearArgOld) == 0x48);

    struct AppearArgNew {
        u32 type{};
        std::array<char16_t, 9> okText{};
        char16_t leftOptionalSymbolKey{};
        char16_t rightOptionalSymbolKey{};
        u8 usePrediction{};
        u8 disableCancelButton{};
        u32 keyDisableFlags{};
        u32 maxTextLength{};
        u32 minTextLength{};
        u8 enableReturnButton{};
        std::array<u8, 3> padding0{};
        u32 flags{};
        u8 isUseSaveData{};
        std::array<u8, 7> padding1{};
        std::array<u8, 16> userId{};
        u64 startSamplingNumber{};
        std::array<u8, 0x20> reserved{};
    };
    static_assert(sizeof(AppearArgNew) == 0x70);

    struct CalcArgCommon {
        u32 unknown{};
        u16 calcArgSize{};
        std::array<u8, 2> padding{};
        u64 flags{};
        InitializeArg initializeArg{};
    };
    static_assert(sizeof(CalcArgCommon) == 0x18);

    struct CalcArgOldBody {
        float volume{};
        i32 cursorPosition{};
        AppearArgOld appearArg{};
        std::array<char16_t, 0x1FA> inputText{};
        u8 utf8Mode{};
        u8 padding0{};
        u8 enableBackspaceButton{};
        std::array<u8, 3> padding1{};
        u8 keyTopAsFloating{};
        u8 footerScalable{};
        u8 alphaEnabledInInputMode{};
        u8 inputModeFadeType{};
        u8 disableTouch{};
        u8 disableHardwareKeyboard{};
        std::array<u8, 8> padding2{};
        float keyTopScaleX{};
        float keyTopScaleY{};
        float keyTopTranslateX{};
        float keyTopTranslateY{};
        float keyTopBgAlpha{};
        float footerBgAlpha{};
        float balloonScale{};
        std::array<u8, 0x10> padding3{};
        u8 seGroup{};
        std::array<u8, 3> padding4{};
    };
    static_assert(sizeof(CalcArgOldBody) == 0x488);

    struct CalcArgNewBody {
        AppearArgNew appearArg{};
        float volume{};
        i32 cursorPosition{};
        std::array<char16_t, 0x1FA> inputText{};
        u8 utf8Mode{};
        u8 padding0{};
        u8 enableBackspaceButton{};
        std::array<u8, 3> padding1{};
        u8 keyTopAsFloating{};
        u8 footerScalable{};
        u8 alphaEnabledInInputMode{};
        u8 inputModeFadeType{};
        u8 disableTouch{};
        u8 disableHardwareKeyboard{};
        std::array<u8, 8> padding2{};
        float keyTopScaleX{};
        float keyTopScaleY{};
        float keyTopTranslateX{};
        float keyTopTranslateY{};
        float keyTopBgAlpha{};
        float footerBgAlpha{};
        float balloonScale{};
        std::array<u8, 0x10> padding3{};
        u8 seGroup{};
        std::array<u8, 3> padding4{};
        std::array<u8, 0x20> reserved{};
    };
    static_assert(sizeof(CalcArgNewBody) == 0x4D0);

    struct ChangedStringArg {
        u32 textLength{};
        i32 dictionaryStartCursorPosition{-1};
        i32 dictionaryEndCursorPosition{-1};
        i32 cursorPosition{};
    };
    static_assert(sizeof(ChangedStringArg) == 0x10);

    struct MovedCursorArg {
        u32 textLength{};
        i32 cursorPosition{};
    };
    static_assert(sizeof(MovedCursorArg) == 0x8);

    struct DecidedEnterArg {
        u32 textLength{};
    };
    static_assert(sizeof(DecidedEnterArg) == 0x4);
#pragma pack(pop)

    static_assert(sizeof(CalcArgCommon) + sizeof(CalcArgOldBody) == 0x4A0);
    static_assert(sizeof(CalcArgCommon) + sizeof(CalcArgNewBody) == 0x4E8);
}
