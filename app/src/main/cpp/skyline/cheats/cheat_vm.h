// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <variant>
#include <vector>
#include "cheat_types.h"

namespace skyline::cheats {
    enum class OpcodeType : u32 {
        StoreStatic = 0,
        BeginConditional = 1,
        EndConditional = 2,
        ControlLoop = 3,
        LoadRegisterStatic = 4,
        LoadRegisterMemory = 5,
        StoreStaticToAddress = 6,
        ArithmeticStatic = 7,
        BeginKeypressConditional = 8,
        ArithmeticRegister = 9,
        StoreRegisterToAddress = 10,
        Extended = 12,
        BeginRegisterConditional = 0xC0,
        SaveRestoreRegister = 0xC1,
        SaveRestoreRegisterMask = 0xC2,
        ReadWriteStaticRegister = 0xC3,
        DoubleExtended = 0xF0,
        PauseProcess = 0xFF0,
        ResumeProcess = 0xFF1,
        DebugLog = 0xFFF,
    };

    enum class MemoryAccessType : u32 {
        MainNso = 0,
        Heap = 1,
        Alias = 2,
        Aslr = 3,
    };

    enum class ComparisonType : u32 {
        GT = 1,
        GE = 2,
        LT = 3,
        LE = 4,
        EQ = 5,
        NE = 6,
    };

    enum class ArithmeticType : u32 {
        Addition = 0,
        Subtraction = 1,
        Multiplication = 2,
        LeftShift = 3,
        RightShift = 4,
        LogicalAnd = 5,
        LogicalOr = 6,
        LogicalNot = 7,
        LogicalXor = 8,
        None = 9,
    };

    enum class StoreOffsetType : u32 {
        None = 0,
        Register = 1,
        Immediate = 2,
        MemoryRegister = 3,
        MemoryImmediate = 4,
        MemoryImmediateRegister = 5,
    };

    enum class CompareValueType : u32 {
        MemoryRelative = 0,
        MemoryOffsetRegister = 1,
        RegisterRelative = 2,
        RegisterOffsetRegister = 3,
        StaticValue = 4,
        OtherRegister = 5,
    };

    enum class SaveRestoreType : u32 {
        Restore = 0,
        Save = 1,
        ClearSaved = 2,
        ClearRegisters = 3,
    };

    enum class DebugValueType : u32 {
        MemoryRelative = 0,
        MemoryOffsetRegister = 1,
        RegisterRelative = 2,
        RegisterOffsetRegister = 3,
        RegisterValue = 4,
    };

    union VmInt {
        u8 u8Value;
        u16 u16Value;
        u32 u32Value;
        u64 u64Value;
    };

    struct StoreStaticOpcode { u32 width{}; MemoryAccessType memory{}; u32 offsetRegister{}; u64 address{}; VmInt value{}; };
    struct BeginConditionalOpcode { u32 width{}; MemoryAccessType memory{}; ComparisonType comparison{}; u64 address{}; VmInt value{}; };
    struct EndConditionalOpcode { bool isElse{}; };
    struct ControlLoopOpcode { bool start{}; u32 reg{}; u32 iterations{}; };
    struct LoadRegisterStaticOpcode { u32 reg{}; u64 value{}; };
    struct LoadRegisterMemoryOpcode { u32 width{}; MemoryAccessType memory{}; u32 reg{}; bool fromRegister{}; u64 address{}; };
    struct StoreStaticToAddressOpcode { u32 width{}; u32 reg{}; bool increment{}; bool addOffsetRegister{}; u32 offsetRegister{}; u64 value{}; };
    struct ArithmeticStaticOpcode { u32 width{}; u32 reg{}; ArithmeticType type{}; u32 value{}; };
    struct BeginKeypressConditionalOpcode { u32 mask{}; };
    struct ArithmeticRegisterOpcode { u32 width{}; ArithmeticType type{}; u32 dst{}; u32 src1{}; u32 src2{}; bool immediate{}; VmInt value{}; };
    struct StoreRegisterToAddressOpcode { u32 width{}; u32 sourceRegister{}; u32 addressRegister{}; bool increment{}; StoreOffsetType offsetType{}; MemoryAccessType memory{}; u32 offsetRegister{}; u64 address{}; };
    struct BeginRegisterConditionalOpcode { u32 width{}; ComparisonType comparison{}; u32 valueRegister{}; CompareValueType valueType{}; MemoryAccessType memory{}; u32 addressRegister{}; u32 otherRegister{}; u32 offsetRegister{}; u64 address{}; VmInt value{}; };
    struct SaveRestoreRegisterOpcode { u32 dst{}; u32 src{}; SaveRestoreType type{}; };
    struct SaveRestoreRegisterMaskOpcode { SaveRestoreType type{}; std::array<bool, 0x10> mask{}; };
    struct ReadWriteStaticRegisterOpcode { u32 staticIndex{}; u32 reg{}; };
    struct PauseProcessOpcode {};
    struct ResumeProcessOpcode {};
    struct DebugLogOpcode { u32 width{}; u32 id{}; DebugValueType valueType{}; MemoryAccessType memory{}; u32 addressRegister{}; u32 valueRegister{}; u32 offsetRegister{}; u64 address{}; };

    using DecodedOpcode = std::variant<
        StoreStaticOpcode, BeginConditionalOpcode, EndConditionalOpcode, ControlLoopOpcode,
        LoadRegisterStaticOpcode, LoadRegisterMemoryOpcode, StoreStaticToAddressOpcode,
        ArithmeticStaticOpcode, BeginKeypressConditionalOpcode, ArithmeticRegisterOpcode,
        StoreRegisterToAddressOpcode, BeginRegisterConditionalOpcode, SaveRestoreRegisterOpcode,
        SaveRestoreRegisterMaskOpcode, ReadWriteStaticRegisterOpcode, PauseProcessOpcode,
        ResumeProcessOpcode, DebugLogOpcode>;

    struct Instruction {
        bool beginsConditional{};
        DecodedOpcode opcode;
    };

    class CheatVm {
      public:
        class Callbacks {
          public:
            virtual ~Callbacks() = default;
            virtual bool ReadMemory(u64 address, void *data, size_t size) = 0;
            virtual bool WriteMemory(u64 address, const void *data, size_t size) = 0;
            virtual u64 KeysDown() = 0;
            virtual void PauseProcess() = 0;
            virtual void ResumeProcess() = 0;
            virtual void DebugLog(u8 id, u64 value) = 0;
        };

        static constexpr size_t MaximumProgramOpcodes{0x400};
        static constexpr size_t RegisterCount{0x10};
        static constexpr size_t ReadableStaticRegisters{0x80};
        static constexpr size_t StaticRegisterCount{0x100};

        explicit CheatVm(Callbacks &callbacks);

        bool LoadProgram(const std::vector<CheatEntry> &entries);
        void Execute(const CheatProcessMetadata &metadata);
        size_t ProgramSize() const { return opcodeCount; }

      private:
        Callbacks &callbacks;
        size_t opcodeCount{};
        size_t instructionPointer{};
        size_t conditionDepth{};
        bool decodeSuccess{};
        std::array<u32, MaximumProgramOpcodes> program{};
        std::array<u64, RegisterCount> registers{};
        std::array<u64, RegisterCount> savedValues{};
        std::array<u64, StaticRegisterCount> staticRegisters{};
        std::array<size_t, RegisterCount> loopTops{};

        bool DecodeNext(Instruction &out);
        void SkipConditional(bool isIf);
        void ResetState();
        static u64 GetValue(VmInt value, u32 width);
        static u64 GetAddress(const CheatProcessMetadata &metadata, MemoryAccessType memory, u64 relative);
        static bool Compare(u64 left, u64 right, ComparisonType comparison);
        static u64 MaskToWidth(u64 value, u32 width);
    };
}
