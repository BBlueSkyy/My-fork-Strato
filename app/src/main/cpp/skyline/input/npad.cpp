// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <input.h>
#include "npad.h"

namespace skyline::input {
    bool NpadManager::IsSixAxisHandleValid(const NpadDeviceHandle &handle) {
        if (!IsNpadIdValid(handle.id) || handle.padding != 0)
            return false;

        switch (handle.type) {
            case 3: // FullKey
            case 4: // Handheld
                return handle.deviceIndex == 2;
            case 5: // JoyDual
                return handle.deviceIndex <= 1;
            case 6: // JoyLeft
                return handle.deviceIndex == 0;
            case 7: // JoyRight
                return handle.deviceIndex == 1;
            default:
                return false;
        }
    }

    bool NpadManager::IsVibrationHandleValid(const NpadDeviceHandle &handle) {
        if (!IsNpadIdValid(handle.id) || handle.padding != 0)
            return false;

        switch (handle.type) {
            case 3: // FullKey
            case 4: // Handheld
            case 5: // JoyDual
                return handle.deviceIndex <= 1;
            case 6: // JoyLeft
            case 8: // GameCube ERM
                return handle.deviceIndex == 0;
            case 7: // JoyRight
                return handle.deviceIndex == 1;
            default:
                return false;
        }
    }

    NpadManager::NpadManager(const DeviceState &state, input::HidSharedMemory *hid) : state(state), npads
        {NpadDevice{*this, hid->npad[0], NpadId::Player1}, {*this, hid->npad[1], NpadId::Player2},
         {*this, hid->npad[2], NpadId::Player3}, {*this, hid->npad[3], NpadId::Player4},
         {*this, hid->npad[4], NpadId::Player5}, {*this, hid->npad[5], NpadId::Player6},
         {*this, hid->npad[6], NpadId::Player7}, {*this, hid->npad[7], NpadId::Player8},
         {*this, hid->npad[8], NpadId::Handheld}, {*this, hid->npad[9], NpadId::Unknown},
        } { Activate(); /* NPads are activated by default, certain homebrew is reliant on this behavior */ }

    void NpadManager::Update() {
        std::scoped_lock guard{mutex};

        if (!activated)
            return;

        for (auto &controller : controllers)
            controller.device = nullptr;

        std::vector<size_t> supportedSlots;
        supportedSlots.reserve(supportedIds.size());
        for (const auto id : supportedIds) {
            if (id != NpadId::Unknown && IsNpadIdValid(id))
                supportedSlots.push_back(NpadIdToIndex(id));
        }

        assignmentOrder.Assign(supportedSlots, [&](size_t slot, size_t source) {
            auto &device{npads.at(slot)};
            auto &controller{controllers.at(source)};
            if (controller.device)
                return false;

            NpadStyleSet style{};
            if (device.id != NpadId::Handheld) {
                if (controller.type == NpadControllerType::ProController)
                    style.proController = true;
                else if (controller.type == NpadControllerType::Gamecube)
                    style.gamecube = true;
                else if (controller.type == NpadControllerType::JoyconLeft)
                    style.joyconLeft = true;
                else if (controller.type == NpadControllerType::JoyconRight)
                    style.joyconRight = true;
                if (controller.type == NpadControllerType::JoyconDual || controller.partnerIndex != -1)
                    style.joyconDual = true;
            } else if (controller.type == NpadControllerType::Handheld) {
                style.joyconHandheld = true;
            }
            style = NpadStyleSet{.raw = style.raw & styles.raw};

            if (style.proController || style.gamecube || style.joyconHandheld || style.joyconLeft || style.joyconRight) {
                device.Connect(controller.type);
                device.index = static_cast<i8>(source);
                device.partnerIndex = -1;
                controller.device = &device;
                return true;
            }
            if (style.joyconDual && orientation == NpadJoyOrientation::Vertical && device.GetAssignment() == NpadJoyAssignment::Dual &&
                controller.partnerIndex >= 0 && static_cast<size_t>(controller.partnerIndex) < controllers.size() &&
                !controllers[static_cast<size_t>(controller.partnerIndex)].device) {
                device.Connect(NpadControllerType::JoyconDual);
                device.index = static_cast<i8>(source);
                device.partnerIndex = controller.partnerIndex;
                controller.device = &device;
                controllers[static_cast<size_t>(controller.partnerIndex)].device = &device;
                return true;
            }
            return false;
        });

        // We do this to prevent triggering the event unless there's a real change in a device's style, which would be caused if we disconnected all controllers then reconnected them
        for (auto &device : npads) {
            if (!ranges::any_of(controllers, [&](auto &controller) { return controller.device == &device; }))
                device.Disconnect();
            assignmentOrder.Observe(NpadIdToIndex(device.id), device.index);
        }
    }

    void NpadManager::Activate() {
        std::scoped_lock guard{mutex};
        if (!activated) {
            supportedIds = {NpadId::Handheld, NpadId::Player1, NpadId::Player2, NpadId::Player3, NpadId::Player4, NpadId::Player5, NpadId::Player6, NpadId::Player7, NpadId::Player8};
            styles = {.proController = true, .joyconHandheld = true, .joyconDual = true, .joyconLeft = true, .joyconRight = true};
            activated = true;

            Update();
        }
    }

    void NpadManager::Deactivate() {
        std::scoped_lock guard{mutex};
        if (activated) {
            supportedIds = {};
            styles = {};
            activated = false;

            for (auto &npad : npads)
                npad.Disconnect();

            for (auto &controller : controllers)
                controller.device = nullptr;
            assignmentOrder.Reset();
        }
    }

    void NpadManager::Disconnect(NpadId id) {
        std::scoped_lock guard{mutex};
        auto &device{at(id)};
        for (auto &controller : controllers) {
            if (controller.device == &device)
                controller.device = nullptr;
        }
        device.Disconnect();
    }

    void NpadManager::SwapAssignment(NpadId first, NpadId second) {
        std::scoped_lock guard{mutex};

        if (first == second || first == NpadId::Handheld || second == NpadId::Handheld ||
            first == NpadId::Unknown || second == NpadId::Unknown)
            return;

        auto &firstDevice{at(first)};
        auto &secondDevice{at(second)};

        struct Assignment {
            NpadControllerType type{NpadControllerType::None};
            i8 index{NpadDevice::NullIndex};
            i8 partnerIndex{NpadDevice::NullIndex};
            std::vector<size_t> controllers;
        };

        auto captureAssignment{[&](NpadDevice &device) {
            Assignment assignment{
                .type = device.type,
                .index = device.index,
                .partnerIndex = device.partnerIndex,
            };

            for (size_t index{}; index < controllers.size(); ++index) {
                if (controllers[index].device == &device) {
                    assignment.controllers.push_back(index);
                    controllers[index].device = nullptr;
                }
            }

            device.Disconnect();
            return assignment;
        }};

        auto firstAssignment{captureAssignment(firstDevice)};
        auto secondAssignment{captureAssignment(secondDevice)};

        auto applyAssignment{[&](NpadDevice &device, const Assignment &assignment) {
            if (assignment.type == NpadControllerType::None)
                return;

            device.Connect(assignment.type);
            device.index = assignment.index;
            device.partnerIndex = assignment.partnerIndex;

            for (const auto index : assignment.controllers)
                controllers.at(index).device = &device;
        }};

        applyAssignment(firstDevice, secondAssignment);
        applyAssignment(secondDevice, firstAssignment);
        assignmentOrder.Remember(NpadIdToIndex(first), firstDevice.index);
        assignmentOrder.Remember(NpadIdToIndex(second), secondDevice.index);
    }

    void NpadManager::UpdateControllerSharedMemory() {
        std::scoped_lock guard{mutex};
        for (auto &pad : npads)
            pad.UpdateControllerSharedMemory();
    }

    void NpadManager::UpdateSixAxisSharedMemory() {
        std::scoped_lock guard{mutex};
        for (auto &pad : npads)
            pad.UpdateSixAxisSharedMemory();
    }

    void NpadManager::SetVibrationPermitted(bool permitted) {
        std::scoped_lock guard{mutex};
        vibrationPermitted = permitted;
        if (!permitted) {
            for (auto &pad : npads)
                pad.StopVibration();
        }
    }
}
