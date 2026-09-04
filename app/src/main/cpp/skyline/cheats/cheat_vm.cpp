// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cheat_vm.h"

namespace skyline::cheats {
    CheatVm::CheatVm(Callbacks &callbacks) : callbacks{callbacks} {}

    u64 CheatVm::GetValue(VmInt value, u32 width) {
        switch (width) {
            case 1: return value.u8Value;
            case 2: return value.u16Value;
            case 4: return value.u32Value;
            case 8: return value.u64Value;
            default: return 0;
        }
    }

    u64 CheatVm::MaskToWidth(u64 value, u32 width) {
        switch (width) {
            case 1: return static_cast<u8>(value);
            case 2: return static_cast<u16>(value);
            case 4: return static_cast<u32>(value);
            case 8: return value;
            default: return 0;
        }
    }

    u64 CheatVm::GetAddress(const CheatProcessMetadata &metadata, MemoryAccessType memory, u64 relative) {
        switch (memory) {
            case MemoryAccessType::Heap: return metadata.heap.base + relative;
            case MemoryAccessType::Alias: return metadata.alias.base + relative;
            case MemoryAccessType::Aslr: return metadata.aslr.base + relative;
            case MemoryAccessType::MainNso:
            default: return metadata.mainNso.base + relative;
        }
    }

    bool CheatVm::Compare(u64 left, u64 right, ComparisonType comparison) {
        switch (comparison) {
            case ComparisonType::GT: return left > right;
            case ComparisonType::GE: return left >= right;
            case ComparisonType::LT: return left < right;
            case ComparisonType::LE: return left <= right;
            case ComparisonType::EQ: return left == right;
            case ComparisonType::NE: return left != right;
            default: return false;
        }
    }

    void CheatVm::ResetState() {
        registers.fill(0);
        savedValues.fill(0);
        loopTops.fill(0);
        instructionPointer = 0;
        conditionDepth = 0;
        decodeSuccess = true;
    }

    bool CheatVm::LoadProgram(const std::vector<CheatEntry> &entries) {
        opcodeCount = 0;
        for (const auto &entry : entries) {
            if (!entry.enabled)
                continue;
            if (entry.definition.numOpcodes + opcodeCount > program.size()) {
                opcodeCount = 0;
                return false;
            }
            for (u32 index{}; index < entry.definition.numOpcodes; index++)
                program[opcodeCount++] = entry.definition.opcodes[index];
        }
        return true;
    }

    bool CheatVm::DecodeNext(Instruction &out) {
        bool valid{decodeSuccess};
        auto next = [&]() -> u32 {
            if (instructionPointer >= opcodeCount) {
                valid = false;
                return 0;
            }
            return program[instructionPointer++];
        };
        auto nextValue = [&](u32 width) {
            VmInt value{};
            const u32 first{next()};
            switch (width) {
                case 1: value.u8Value = static_cast<u8>(first); break;
                case 2: value.u16Value = static_cast<u16>(first); break;
                case 4: value.u32Value = first; break;
                case 8: value.u64Value = (static_cast<u64>(first) << 32) | next(); break;
                default: valid = false; break;
            }
            return value;
        };

        const u32 first{next()};
        if (!valid) {
            decodeSuccess = false;
            return false;
        }

        auto type{static_cast<OpcodeType>((first >> 28) & 0xF)};
        if (type >= OpcodeType::Extended)
            type = static_cast<OpcodeType>((static_cast<u32>(type) << 4) | ((first >> 24) & 0xF));
        if (type >= OpcodeType::DoubleExtended)
            type = static_cast<OpcodeType>((static_cast<u32>(type) << 4) | ((first >> 20) & 0xF));

        Instruction decoded{};
        decoded.beginsConditional = type == OpcodeType::BeginConditional ||
                                    type == OpcodeType::BeginKeypressConditional ||
                                    type == OpcodeType::BeginRegisterConditional;

        switch (type) {
            case OpcodeType::StoreStatic: {
                const u32 second{next()};
                const u32 width{(first >> 24) & 0xF};
                decoded.opcode = StoreStaticOpcode{
                    width,
                    static_cast<MemoryAccessType>((first >> 20) & 0xF),
                    (first >> 16) & 0xF,
                    (static_cast<u64>(first & 0xFF) << 32) | second,
                    nextValue(width),
                };
                break;
            }
            case OpcodeType::BeginConditional: {
                const u32 second{next()};
                const u32 width{(first >> 24) & 0xF};
                decoded.opcode = BeginConditionalOpcode{
                    width,
                    static_cast<MemoryAccessType>((first >> 20) & 0xF),
                    static_cast<ComparisonType>((first >> 16) & 0xF),
                    (static_cast<u64>(first & 0xFF) << 32) | second,
                    nextValue(width),
                };
                break;
            }
            case OpcodeType::EndConditional:
                decoded.opcode = EndConditionalOpcode{((first >> 24) & 0xF) == 1};
                break;
            case OpcodeType::ControlLoop: {
                const bool start{((first >> 24) & 0xF) == 0};
                decoded.opcode = ControlLoopOpcode{start, (first >> 20) & 0xF, start ? next() : 0};
                break;
            }
            case OpcodeType::LoadRegisterStatic:
                decoded.opcode = LoadRegisterStaticOpcode{
                    (first >> 16) & 0xF,
                    (static_cast<u64>(next()) << 32) | next(),
                };
                break;
            case OpcodeType::LoadRegisterMemory: {
                const u32 second{next()};
                decoded.opcode = LoadRegisterMemoryOpcode{
                    (first >> 24) & 0xF,
                    static_cast<MemoryAccessType>((first >> 20) & 0xF),
                    (first >> 16) & 0xF,
                    ((first >> 12) & 0xF) != 0,
                    (static_cast<u64>(first & 0xFF) << 32) | second,
                };
                break;
            }
            case OpcodeType::StoreStaticToAddress:
                decoded.opcode = StoreStaticToAddressOpcode{
                    (first >> 24) & 0xF,
                    (first >> 16) & 0xF,
                    ((first >> 12) & 0xF) != 0,
                    ((first >> 8) & 0xF) != 0,
                    (first >> 4) & 0xF,
                    (static_cast<u64>(next()) << 32) | next(),
                };
                break;
            case OpcodeType::ArithmeticStatic:
                decoded.opcode = ArithmeticStaticOpcode{
                    (first >> 24) & 0xF,
                    (first >> 16) & 0xF,
                    static_cast<ArithmeticType>((first >> 12) & 0xF),
                    next(),
                };
                break;
            case OpcodeType::BeginKeypressConditional:
                decoded.opcode = BeginKeypressConditionalOpcode{first & 0x0FFFFFFF};
                break;
            case OpcodeType::ArithmeticRegister: {
                ArithmeticRegisterOpcode opcode{
                    (first >> 24) & 0xF,
                    static_cast<ArithmeticType>((first >> 20) & 0xF),
                    (first >> 16) & 0xF,
                    (first >> 12) & 0xF,
                    0,
                    ((first >> 8) & 0xF) != 0,
                    {},
                };
                if (opcode.immediate)
                    opcode.value = nextValue(opcode.width);
                else
                    opcode.src2 = (first >> 4) & 0xF;
                decoded.opcode = opcode;
                break;
            }
            case OpcodeType::StoreRegisterToAddress: {
                StoreRegisterToAddressOpcode opcode{
                    (first >> 24) & 0xF,
                    (first >> 20) & 0xF,
                    (first >> 16) & 0xF,
                    ((first >> 12) & 0xF) != 0,
                    static_cast<StoreOffsetType>((first >> 8) & 0xF),
                    MemoryAccessType::MainNso,
                    (first >> 4) & 0xF,
                    0,
                };
                switch (opcode.offsetType) {
                    case StoreOffsetType::Immediate:
                        opcode.address = (static_cast<u64>(first & 0xF) << 32) | next();
                        break;
                    case StoreOffsetType::MemoryRegister:
                        opcode.memory = static_cast<MemoryAccessType>((first >> 4) & 0xF);
                        break;
                    case StoreOffsetType::MemoryImmediate:
                    case StoreOffsetType::MemoryImmediateRegister:
                        opcode.memory = static_cast<MemoryAccessType>((first >> 4) & 0xF);
                        opcode.address = (static_cast<u64>(first & 0xF) << 32) | next();
                        break;
                    case StoreOffsetType::None:
                    case StoreOffsetType::Register:
                        break;
                    default:
                        valid = false;
                        break;
                }
                decoded.opcode = opcode;
                break;
            }
            case OpcodeType::BeginRegisterConditional: {
                BeginRegisterConditionalOpcode opcode{
                    (first >> 20) & 0xF,
                    static_cast<ComparisonType>((first >> 16) & 0xF),
                    (first >> 12) & 0xF,
                    static_cast<CompareValueType>((first >> 8) & 0xF),
                    MemoryAccessType::MainNso,
                    0, 0, 0, 0, {},
                };
                switch (opcode.valueType) {
                    case CompareValueType::StaticValue:
                        opcode.value = nextValue(opcode.width);
                        break;
                    case CompareValueType::OtherRegister:
                        opcode.otherRegister = (first >> 4) & 0xF;
                        break;
                    case CompareValueType::MemoryRelative:
                        opcode.memory = static_cast<MemoryAccessType>((first >> 4) & 0xF);
                        opcode.address = (static_cast<u64>(first & 0xF) << 32) | next();
                        break;
                    case CompareValueType::MemoryOffsetRegister:
                        opcode.memory = static_cast<MemoryAccessType>((first >> 4) & 0xF);
                        opcode.offsetRegister = first & 0xF;
                        break;
                    case CompareValueType::RegisterRelative:
                        opcode.addressRegister = (first >> 4) & 0xF;
                        opcode.address = (static_cast<u64>(first & 0xF) << 32) | next();
                        break;
                    case CompareValueType::RegisterOffsetRegister:
                        opcode.addressRegister = (first >> 4) & 0xF;
                        opcode.offsetRegister = first & 0xF;
                        break;
                    default:
                        valid = false;
                        break;
                }
                decoded.opcode = opcode;
                break;
            }
            case OpcodeType::SaveRestoreRegister:
                decoded.opcode = SaveRestoreRegisterOpcode{
                    (first >> 16) & 0xF,
                    (first >> 8) & 0xF,
                    static_cast<SaveRestoreType>((first >> 4) & 0xF),
                };
                break;
            case OpcodeType::SaveRestoreRegisterMask: {
                SaveRestoreRegisterMaskOpcode opcode{static_cast<SaveRestoreType>((first >> 20) & 0xF), {}};
                for (size_t index{}; index < RegisterCount; index++)
                    opcode.mask[index] = (first & (1U << index)) != 0;
                decoded.opcode = opcode;
                break;
            }
            case OpcodeType::ReadWriteStaticRegister:
                decoded.opcode = ReadWriteStaticRegisterOpcode{(first >> 4) & 0xFF, first & 0xF};
                break;
            case OpcodeType::PauseProcess:
                decoded.opcode = PauseProcessOpcode{};
                break;
            case OpcodeType::ResumeProcess:
                decoded.opcode = ResumeProcessOpcode{};
                break;
            case OpcodeType::DebugLog: {
                DebugLogOpcode opcode{
                    (first >> 16) & 0xF,
                    (first >> 12) & 0xF,
                    static_cast<DebugValueType>((first >> 8) & 0xF),
                    MemoryAccessType::MainNso,
                    0, 0, 0, 0,
                };
                switch (opcode.valueType) {
                    case DebugValueType::RegisterValue:
                        opcode.valueRegister = (first >> 4) & 0xF;
                        break;
                    case DebugValueType::MemoryRelative:
                        opcode.memory = static_cast<MemoryAccessType>((first >> 4) & 0xF);
                        opcode.address = (static_cast<u64>(first & 0xF) << 32) | next();
                        break;
                    case DebugValueType::MemoryOffsetRegister:
                        opcode.memory = static_cast<MemoryAccessType>((first >> 4) & 0xF);
                        opcode.offsetRegister = first & 0xF;
                        break;
                    case DebugValueType::RegisterRelative:
                        opcode.addressRegister = (first >> 4) & 0xF;
                        opcode.address = (static_cast<u64>(first & 0xF) << 32) | next();
                        break;
                    case DebugValueType::RegisterOffsetRegister:
                        opcode.addressRegister = (first >> 4) & 0xF;
                        opcode.offsetRegister = first & 0xF;
                        break;
                    default:
                        valid = false;
                        break;
                }
                decoded.opcode = opcode;
                break;
            }
            default:
                valid = false;
                break;
        }

        decodeSuccess = decodeSuccess && valid;
        if (valid)
            out = decoded;
        return valid;
    }

    void CheatVm::SkipConditional(bool isIf) {
        if (!conditionDepth) {
            decodeSuccess = false;
            return;
        }

        const size_t desiredDepth{conditionDepth - 1};
        Instruction instruction{};
        while (conditionDepth > desiredDepth && DecodeNext(instruction)) {
            if (instruction.beginsConditional) {
                conditionDepth++;
            } else if (auto end = std::get_if<EndConditionalOpcode>(&instruction.opcode)) {
                if (!end->isElse)
                    conditionDepth--;
                else if (isIf && conditionDepth - 1 == desiredDepth)
                    break;
            }
        }
    }

    void CheatVm::Execute(const CheatProcessMetadata &metadata) {
        ResetState();
        const u64 keys{callbacks.KeysDown()};
        Instruction instruction{};

        while (DecodeNext(instruction)) {
            if (instruction.beginsConditional)
                conditionDepth++;

            if (auto opcode = std::get_if<StoreStaticOpcode>(&instruction.opcode)) {
                const u64 address{GetAddress(metadata, opcode->memory, opcode->address + registers[opcode->offsetRegister])};
                const u64 value{GetValue(opcode->value, opcode->width)};
                if (opcode->width == 1 || opcode->width == 2 || opcode->width == 4 || opcode->width == 8)
                    callbacks.WriteMemory(address, &value, opcode->width);
            } else if (auto opcode = std::get_if<BeginConditionalOpcode>(&instruction.opcode)) {
                u64 value{};
                if (opcode->width == 1 || opcode->width == 2 || opcode->width == 4 || opcode->width == 8)
                    callbacks.ReadMemory(GetAddress(metadata, opcode->memory, opcode->address), &value, opcode->width);
                if (!Compare(value, GetValue(opcode->value, opcode->width), opcode->comparison))
                    SkipConditional(true);
            } else if (auto opcode = std::get_if<EndConditionalOpcode>(&instruction.opcode)) {
                if (opcode->isElse)
                    SkipConditional(false);
                else if (conditionDepth)
                    conditionDepth--;
            } else if (auto opcode = std::get_if<ControlLoopOpcode>(&instruction.opcode)) {
                if (opcode->start) {
                    registers[opcode->reg] = opcode->iterations;
                    loopTops[opcode->reg] = instructionPointer;
                } else if (registers[opcode->reg] && --registers[opcode->reg]) {
                    instructionPointer = loopTops[opcode->reg];
                }
            } else if (auto opcode = std::get_if<LoadRegisterStaticOpcode>(&instruction.opcode)) {
                registers[opcode->reg] = opcode->value;
            } else if (auto opcode = std::get_if<LoadRegisterMemoryOpcode>(&instruction.opcode)) {
                const u64 address{opcode->fromRegister ? registers[opcode->reg] + opcode->address : GetAddress(metadata, opcode->memory, opcode->address)};
                registers[opcode->reg] = 0;
                if (opcode->width == 1 || opcode->width == 2 || opcode->width == 4 || opcode->width == 8)
                    callbacks.ReadMemory(address, &registers[opcode->reg], opcode->width);
            } else if (auto opcode = std::get_if<StoreStaticToAddressOpcode>(&instruction.opcode)) {
                u64 address{registers[opcode->reg]};
                if (opcode->addOffsetRegister)
                    address += registers[opcode->offsetRegister];
                if (opcode->width == 1 || opcode->width == 2 || opcode->width == 4 || opcode->width == 8)
                    callbacks.WriteMemory(address, &opcode->value, opcode->width);
                if (opcode->increment)
                    registers[opcode->reg] += opcode->width;
            } else if (auto opcode = std::get_if<ArithmeticStaticOpcode>(&instruction.opcode)) {
                auto &reg{registers[opcode->reg]};
                switch (opcode->type) {
                    case ArithmeticType::Addition: reg += opcode->value; break;
                    case ArithmeticType::Subtraction: reg -= opcode->value; break;
                    case ArithmeticType::Multiplication: reg *= opcode->value; break;
                    case ArithmeticType::LeftShift: reg = opcode->value >= 64 ? 0 : reg << opcode->value; break;
                    case ArithmeticType::RightShift: reg = opcode->value >= 64 ? 0 : reg >> opcode->value; break;
                    default: break;
                }
                reg = MaskToWidth(reg, opcode->width);
            } else if (auto opcode = std::get_if<BeginKeypressConditionalOpcode>(&instruction.opcode)) {
                if ((opcode->mask & keys) != opcode->mask)
                    SkipConditional(true);
            } else if (auto opcode = std::get_if<ArithmeticRegisterOpcode>(&instruction.opcode)) {
                const u64 left{registers[opcode->src1]};
                const u64 right{opcode->immediate ? GetValue(opcode->value, opcode->width) : registers[opcode->src2]};
                u64 result{};
                switch (opcode->type) {
                    case ArithmeticType::Addition: result = left + right; break;
                    case ArithmeticType::Subtraction: result = left - right; break;
                    case ArithmeticType::Multiplication: result = left * right; break;
                    case ArithmeticType::LeftShift: result = right >= 64 ? 0 : left << right; break;
                    case ArithmeticType::RightShift: result = right >= 64 ? 0 : left >> right; break;
                    case ArithmeticType::LogicalAnd: result = left & right; break;
                    case ArithmeticType::LogicalOr: result = left | right; break;
                    case ArithmeticType::LogicalNot: result = ~left; break;
                    case ArithmeticType::LogicalXor: result = left ^ right; break;
                    case ArithmeticType::None: result = left; break;
                }
                registers[opcode->dst] = MaskToWidth(result, opcode->width);
            } else if (auto opcode = std::get_if<StoreRegisterToAddressOpcode>(&instruction.opcode)) {
                u64 address{registers[opcode->addressRegister]};
                switch (opcode->offsetType) {
                    case StoreOffsetType::None: break;
                    case StoreOffsetType::Register: address += registers[opcode->offsetRegister]; break;
                    case StoreOffsetType::Immediate: address += opcode->address; break;
                    case StoreOffsetType::MemoryRegister: address = GetAddress(metadata, opcode->memory, registers[opcode->addressRegister]); break;
                    case StoreOffsetType::MemoryImmediate: address = GetAddress(metadata, opcode->memory, opcode->address); break;
                    case StoreOffsetType::MemoryImmediateRegister: address = GetAddress(metadata, opcode->memory, registers[opcode->addressRegister] + opcode->address); break;
                }
                const u64 value{registers[opcode->sourceRegister]};
                if (opcode->width == 1 || opcode->width == 2 || opcode->width == 4 || opcode->width == 8)
                    callbacks.WriteMemory(address, &value, opcode->width);
                if (opcode->increment)
                    registers[opcode->addressRegister] += opcode->width;
            } else if (auto opcode = std::get_if<BeginRegisterConditionalOpcode>(&instruction.opcode)) {
                const u64 left{MaskToWidth(registers[opcode->valueRegister], opcode->width)};
                u64 right{};
                if (opcode->valueType == CompareValueType::StaticValue) {
                    right = GetValue(opcode->value, opcode->width);
                } else if (opcode->valueType == CompareValueType::OtherRegister) {
                    right = MaskToWidth(registers[opcode->otherRegister], opcode->width);
                } else {
                    u64 address{};
                    switch (opcode->valueType) {
                        case CompareValueType::MemoryRelative: address = GetAddress(metadata, opcode->memory, opcode->address); break;
                        case CompareValueType::MemoryOffsetRegister: address = GetAddress(metadata, opcode->memory, registers[opcode->offsetRegister]); break;
                        case CompareValueType::RegisterRelative: address = registers[opcode->addressRegister] + opcode->address; break;
                        case CompareValueType::RegisterOffsetRegister: address = registers[opcode->addressRegister] + registers[opcode->offsetRegister]; break;
                        default: break;
                    }
                    if (opcode->width == 1 || opcode->width == 2 || opcode->width == 4 || opcode->width == 8)
                        callbacks.ReadMemory(address, &right, opcode->width);
                }
                if (!Compare(left, right, opcode->comparison))
                    SkipConditional(true);
            } else if (auto opcode = std::get_if<SaveRestoreRegisterOpcode>(&instruction.opcode)) {
                switch (opcode->type) {
                    case SaveRestoreType::ClearRegisters: registers[opcode->dst] = 0; break;
                    case SaveRestoreType::ClearSaved: savedValues[opcode->dst] = 0; break;
                    case SaveRestoreType::Save: savedValues[opcode->dst] = registers[opcode->src]; break;
                    case SaveRestoreType::Restore: registers[opcode->dst] = savedValues[opcode->src]; break;
                }
            } else if (auto opcode = std::get_if<SaveRestoreRegisterMaskOpcode>(&instruction.opcode)) {
                for (size_t index{}; index < RegisterCount; index++) {
                    if (!opcode->mask[index])
                        continue;
                    switch (opcode->type) {
                        case SaveRestoreType::ClearRegisters: registers[index] = 0; break;
                        case SaveRestoreType::ClearSaved: savedValues[index] = 0; break;
                        case SaveRestoreType::Save: savedValues[index] = registers[index]; break;
                        case SaveRestoreType::Restore: registers[index] = savedValues[index]; break;
                    }
                }
            } else if (auto opcode = std::get_if<ReadWriteStaticRegisterOpcode>(&instruction.opcode)) {
                if (opcode->staticIndex < ReadableStaticRegisters)
                    registers[opcode->reg] = staticRegisters[opcode->staticIndex];
                else
                    staticRegisters[opcode->staticIndex] = registers[opcode->reg];
            } else if (std::holds_alternative<PauseProcessOpcode>(instruction.opcode)) {
                callbacks.PauseProcess();
            } else if (std::holds_alternative<ResumeProcessOpcode>(instruction.opcode)) {
                callbacks.ResumeProcess();
            } else if (auto opcode = std::get_if<DebugLogOpcode>(&instruction.opcode)) {
                u64 value{};
                if (opcode->valueType == DebugValueType::RegisterValue) {
                    value = MaskToWidth(registers[opcode->valueRegister], opcode->width);
                } else {
                    u64 address{};
                    switch (opcode->valueType) {
                        case DebugValueType::MemoryRelative: address = GetAddress(metadata, opcode->memory, opcode->address); break;
                        case DebugValueType::MemoryOffsetRegister: address = GetAddress(metadata, opcode->memory, registers[opcode->offsetRegister]); break;
                        case DebugValueType::RegisterRelative: address = registers[opcode->addressRegister] + opcode->address; break;
                        case DebugValueType::RegisterOffsetRegister: address = registers[opcode->addressRegister] + registers[opcode->offsetRegister]; break;
                        default: break;
                    }
                    if (opcode->width == 1 || opcode->width == 2 || opcode->width == 4 || opcode->width == 8)
                        callbacks.ReadMemory(address, &value, opcode->width);
                }
                callbacks.DebugLog(static_cast<u8>(opcode->id), value);
            }
        }
    }
}
