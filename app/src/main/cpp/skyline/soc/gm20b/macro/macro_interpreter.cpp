// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <atomic>
#include "macro_state.h"
#include "soc/gm20b/engines/engine.h"
#include "macro_interpreter.h"

namespace skyline::soc::gm20b::engine {
    MacroInterpreter::MacroInterpreter(span<u32> macroCode) : macroCode{macroCode} {}

    void MacroInterpreter::Execute(size_t offset, span<u32> args, MacroEngineBase *targetEngine) {
        // Reset the interpreter state
        engine = targetEngine;
        opcode = reinterpret_cast<Opcode *>(&macroCode[offset]);
        registers = {};
        argument = args.data();
        methodAddress.raw = 0;
        carryFlag = false;

        // The first argument is stored in register 1
        registers[1] = *argument++;

        diagnosticSendFailed = false;
        try {
            while (Step());
        } catch (...) {
            try {
                static std::atomic<u32> failures{};
                if (failures.fetch_add(1, std::memory_order_relaxed) < 2) {
                    const auto pcAddress{reinterpret_cast<uintptr_t>(opcode)};
                    const auto codeBegin{reinterpret_cast<uintptr_t>(macroCode.data())};
                    const bool pcValid{pcAddress >= codeBegin && pcAddress - codeBegin < macroCode.size_bytes()};
                    const size_t pc{pcValid ? (pcAddress - codeBegin) / sizeof(u32) : macroCode.size()};
                    const auto argAddress{reinterpret_cast<uintptr_t>(argument)};
                    const auto argBegin{reinterpret_cast<uintptr_t>(args.data())};
                    const bool argValid{argAddress >= argBegin && argAddress - argBegin <= args.size_bytes()};
                    LOGE("MME_DIAG failure entry={} pc={} pcValid={} argCount={} consumed={} argValid={} sendFailed={} method=0x{:X} argument=0x{:X} carry={}",
                        offset, pc, pcValid, args.size(), argValid ? (argAddress - argBegin) / sizeof(u32) : 0,
                        argValid, diagnosticSendFailed, diagnosticMethod, diagnosticArgument, carryFlag);
                    LOGE("MME_DIAG registers r0=0x{:X} r1=0x{:X} r2=0x{:X} r3=0x{:X} r4=0x{:X} r5=0x{:X} r6=0x{:X} r7=0x{:X}",
                        registers[0], registers[1], registers[2], registers[3], registers[4], registers[5], registers[6], registers[7]);
                    for (size_t i{}; i < std::min<size_t>(args.size(), 16); ++i)
                        LOGE("MME_DIAG argument index={} raw=0x{:X}", i, args[i]);
                    for (size_t i{offset}; i < macroCode.size() && i - offset < 16; ++i)
                        LOGE("MME_DIAG entryCode index={} raw=0x{:X}", i, macroCode[i]);
                    if (pcValid)
                        for (size_t i{pc > 8 ? pc - 8 : 0}; i < macroCode.size() && i <= pc + 8; ++i)
                            LOGE("MME_DIAG pcCode index={} raw=0x{:X}", i, macroCode[i]);
                }
            } catch (...) {} // A logger failure must not replace the engine exception.
            throw;
        }
    }

    __attribute__((always_inline)) bool MacroInterpreter::Step(Opcode *delayedOpcode) {
        switch (opcode->operation) {
            case Opcode::Operation::AluRegister: {
                u32 result{HandleAlu(opcode->aluOperation, registers[opcode->srcA], registers[opcode->srcB])};

                HandleAssignment(opcode->assignmentOperation, opcode->dest, result);
                break;
            }

            case Opcode::Operation::AddImmediate:
                HandleAssignment(opcode->assignmentOperation, opcode->dest,
                                 static_cast<u32>(static_cast<i32>(registers[opcode->srcA]) + opcode->immediate));
                break;

            case Opcode::Operation::BitfieldReplace: {
                u32 src{registers[opcode->srcB]};
                u32 dest{registers[opcode->srcA]};

                // Extract the source region
                src = (src >> opcode->bitfield.srcBit) & opcode->bitfield.GetMask();

                // Mask out the bits that we will replace
                dest &= ~(opcode->bitfield.GetMask() << opcode->bitfield.destBit);

                // Replace the bitfield region in the destination with the region from the source
                dest |= src << opcode->bitfield.destBit;

                HandleAssignment(opcode->assignmentOperation, opcode->dest, dest);
                break;
            }

            case Opcode::Operation::BitfieldExtractShiftLeftImmediate: {
                u32 src{registers[opcode->srcB]};
                u32 dest{registers[opcode->srcA]};

                u32 result{((src >> dest) & opcode->bitfield.GetMask()) << opcode->bitfield.destBit};

                HandleAssignment(opcode->assignmentOperation, opcode->dest, result);
                break;
            }

            case Opcode::Operation::BitfieldExtractShiftLeftRegister: {
                u32 src{registers[opcode->srcB]};
                u32 dest{registers[opcode->srcA]};

                u32 result{((src >> opcode->bitfield.srcBit) & opcode->bitfield.GetMask()) << dest};

                HandleAssignment(opcode->assignmentOperation, opcode->dest, result);
                break;
            }

            case Opcode::Operation::ReadImmediate: {
                u32 result{engine->ReadMethodFromMacro(static_cast<u32>(static_cast<i32>(registers[opcode->srcA]) + opcode->immediate))};
                HandleAssignment(opcode->assignmentOperation, opcode->dest, result);
                break;
            }

            case Opcode::Operation::Branch: {
                if (delayedOpcode != nullptr)
                    throw exception("Cannot branch while inside a delay slot");

                u32 value{registers[opcode->srcA]};
                bool branch{(opcode->branchCondition == Opcode::BranchCondition::Zero) ? (value == 0) : (value != 0)};

                if (branch) {
                    if (opcode->noDelay) {
                        opcode += opcode->immediate;
                        return true;
                    } else {
                        Opcode *targetOpcode{opcode + opcode->immediate};

                        // Step into delay slot
                        opcode++;
                        return Step(targetOpcode);
                    }
                }
                break;
            }

            default:
                throw exception("Unknown MME opcode encountered: 0x{:X}", static_cast<u8>(opcode->operation));
        }

        if (opcode->exit && (delayedOpcode == nullptr)) {
            // Exit has a delay slot
            opcode++;
            Step(opcode);
            return false;
        }

        if (delayedOpcode != nullptr)
            opcode = delayedOpcode;
        else
            opcode++;

        return true;
    }

    __attribute__((always_inline)) u32 MacroInterpreter::HandleAlu(Opcode::AluOperation operation, u32 srcA, u32 srcB) {
        switch (operation) {
            case Opcode::AluOperation::Add: {
                u64 result{static_cast<u64>(srcA) + srcB};

                carryFlag = result >> 32;
                return static_cast<u32>(result);
            }
            case Opcode::AluOperation::AddWithCarry: {
                u64 result{static_cast<u64>(srcA) + srcB + carryFlag};

                carryFlag = result >> 32;
                return static_cast<u32>(result);
            }
            case Opcode::AluOperation::Subtract: {
                u64 result{static_cast<u64>(srcA) - srcB};

                // Maxwell sets carry when the subtraction does not borrow.
                carryFlag = result < 0x100000000ULL;
                return static_cast<u32>(result);
            }
            case Opcode::AluOperation::SubtractWithBorrow: {
                u64 result{static_cast<u64>(srcA) - srcB - !carryFlag};

                // Preserve the same no-borrow carry convention for chained subtraction.
                carryFlag = result < 0x100000000ULL;
                return static_cast<u32>(result);
            }
            case Opcode::AluOperation::BitwiseXor:
                return srcA ^ srcB;
            case Opcode::AluOperation::BitwiseOr:
                return srcA | srcB;
            case Opcode::AluOperation::BitwiseAnd:
                return srcA & srcB;
            case Opcode::AluOperation::BitwiseAndNot:
                return srcA & ~srcB;
            case Opcode::AluOperation::BitwiseNand:
                return ~(srcA & srcB);
        }
    }

    __attribute__((always_inline)) void MacroInterpreter::HandleAssignment(Opcode::AssignmentOperation operation, u8 reg, u32 result) {
        switch (operation) {
            case Opcode::AssignmentOperation::IgnoreAndFetch:
                WriteRegister(reg, *argument++);
                break;
            case Opcode::AssignmentOperation::Move:
                WriteRegister(reg, result);
                break;
            case Opcode::AssignmentOperation::MoveAndSetMethod:
                WriteRegister(reg, result);
                methodAddress.raw = result;
                break;
            case Opcode::AssignmentOperation::FetchAndSend:
                WriteRegister(reg, *argument++);
                Send(result);
                break;
            case Opcode::AssignmentOperation::MoveAndSend:
                WriteRegister(reg, result);
                Send(result);
                break;
            case Opcode::AssignmentOperation::FetchAndSetMethod:
                WriteRegister(reg, *argument++);
                methodAddress.raw = result;
                break;
            case Opcode::AssignmentOperation::MoveAndSetMethodThenFetchAndSend:
                WriteRegister(reg, result);
                methodAddress.raw = result;
                Send(*argument++);
                break;
            case Opcode::AssignmentOperation::MoveAndSetMethodThenSendHigh:
                WriteRegister(reg, result);
                methodAddress.raw = result;
                Send(methodAddress.increment);
                break;
        }
    }

    __attribute__((always_inline)) void MacroInterpreter::Send(u32 pArgument) {
        try {
            engine->CallMethodFromMacro(methodAddress.address, pArgument);
        } catch (...) {
            diagnosticSendFailed = true;
            diagnosticMethod = methodAddress.address;
            diagnosticArgument = pArgument;
            throw;
        }
        methodAddress.address += methodAddress.increment;
    }

    __attribute__((always_inline)) void MacroInterpreter::WriteRegister(u8 reg, u32 value) {
        // Register 0 should always be zero so block writes to it
        if (reg == 0) [[unlikely]]
            return;

        registers[reg] = value;
    }
}
