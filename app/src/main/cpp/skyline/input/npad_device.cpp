// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cmath>
#include <jvm.h>
#include "npad_device.h"
#include "npad.h"

namespace skyline::input {
    NpadDevice::NpadDevice(NpadManager &manager, NpadSection &section, NpadId id)
        : manager(manager),
          section(section),
          id(id),
          updateEvent(std::make_shared<kernel::type::KEvent>(manager.state, false)) {
        constexpr std::size_t InitializeEntryCount{19}; //!< HW initializes the first 19 entries

        ResetDeviceProperties();
        for (std::size_t i{}; i < InitializeEntryCount; ++i) {
            WriteEmptyEntries();
        }
    }

    void NpadDevice::Connect(NpadControllerType newType) {
        if (type == newType) {
            if (type == NpadControllerType::JoyconLeft || type == NpadControllerType::JoyconRight) {
                switch (manager.orientation) {
                    case NpadJoyOrientation::Vertical:
                        section.systemProperties.abxyButtonsOriented = true;
                        section.systemProperties.slSrButtonOriented = false;
                        break;
                    case NpadJoyOrientation::Horizontal:
                        section.systemProperties.abxyButtonsOriented = false;
                        section.systemProperties.slSrButtonOriented = true;
                        break;
                }
            }
            return;
        }

        ResetDeviceProperties();
        controllerInfo = nullptr;
        sixAxisInfoLeft = nullptr;
        sixAxisInfoRight = nullptr;

        connectionState.raw = 0;
        connectionState.connected = true;

        switch (newType) {
            case NpadControllerType::ProController:
                section.header.type = NpadControllerType::ProController;
                section.deviceType.fullKey = true;

                section.systemProperties.abxyButtonsOriented = true;
                section.systemProperties.plusButtonCapability = true;
                section.systemProperties.minusButtonCapability = true;

                connectionState.wired = true;
                break;

            case NpadControllerType::Handheld:
                section.header.type = NpadControllerType::Handheld;
                section.deviceType.handheldLeft = true;
                section.deviceType.handheldRight = true;

                section.systemProperties.abxyButtonsOriented = true;
                section.systemProperties.plusButtonCapability = true;
                section.systemProperties.minusButtonCapability = true;
                section.systemProperties.directionalButtonsSupported = true;

                connectionState.wired = true;
                connectionState.leftJoyconConnected = true;
                connectionState.leftJoyconWired = true;
                connectionState.rightJoyconConnected = true;
                connectionState.rightJoyconWired = true;
                break;

            case NpadControllerType::JoyconDual:
                section.header.type = NpadControllerType::JoyconDual;
                section.deviceType.joyconLeft = true;
                section.deviceType.joyconRight = true;
                section.header.assignment = NpadJoyAssignment::Dual;

                section.systemProperties.abxyButtonsOriented = true;
                section.systemProperties.plusButtonCapability = true;
                section.systemProperties.minusButtonCapability = true;
                section.systemProperties.directionalButtonsSupported = true;

                connectionState.leftJoyconConnected = true;
                connectionState.rightJoyconConnected = true;
                break;

            case NpadControllerType::JoyconLeft:
                section.header.type = NpadControllerType::JoyconLeft;
                section.deviceType.joyconLeft = true;
                section.header.assignment = NpadJoyAssignment::Single;

                if (manager.orientation == NpadJoyOrientation::Vertical)
                    section.systemProperties.abxyButtonsOriented = true;
                else if (manager.orientation == NpadJoyOrientation::Horizontal)
                    section.systemProperties.slSrButtonOriented = true;

                section.systemProperties.minusButtonCapability = true;
                section.systemProperties.directionalButtonsSupported = true;

                connectionState.leftJoyconConnected = true;
                break;

            case NpadControllerType::JoyconRight:
                section.header.type = NpadControllerType::JoyconRight;
                section.deviceType.joyconRight = true;
                section.header.assignment = NpadJoyAssignment::Single;

                if (manager.orientation == NpadJoyOrientation::Vertical)
                    section.systemProperties.abxyButtonsOriented = true;
                else if (manager.orientation == NpadJoyOrientation::Horizontal)
                    section.systemProperties.slSrButtonOriented = true;

                section.systemProperties.slSrButtonOriented = true;
                section.systemProperties.plusButtonCapability = true;

                connectionState.rightJoyconConnected = true;
                break;

            case NpadControllerType::Gamecube:
                section.header.type = NpadControllerType::Gamecube;
                section.deviceType.fullKey = true;
                section.systemProperties.abxyButtonsOriented = true;
                section.systemProperties.plusButtonCapability = true;
                section.systemProperties.directionalButtonsSupported = true;
                connectionState.wired = true;
                break;

            default:
                throw exception("Unsupported controller type: {}", newType);
        }

        switch (newType) {
            case NpadControllerType::ProController:
            case NpadControllerType::JoyconLeft:
            case NpadControllerType::JoyconRight:
                section.header.singleColorStatus = NpadColorReadStatus::Success;
                if (newType == NpadControllerType::ProController)
                    section.header.singleColor = {0xFF2D2D2D, 0xFFE6E6E6}; // Normal Pro-Controller
                else
                    section.header.singleColor = {0x4655F5, 0x00000A}; // Blue Joy-Con (https://switchbrew.org/wiki/Joy-Con#Colors)
                break;

            case NpadControllerType::Handheld:
            case NpadControllerType::JoyconDual:
                section.header.dualColorStatus = NpadColorReadStatus::Success;
                section.header.leftColor = {0x4655F5, 0x00000A};
                section.header.rightColor = {0x4655F5, 0x00000A};

                section.header.singleColorStatus = NpadColorReadStatus::Success; // Single color is also written for dual controllers
                section.header.singleColor = section.header.leftColor;           // and is set to the color of the left JC
                break;
            case NpadControllerType::Gamecube:
            case NpadControllerType::None:
                break;
        }

        section.singleBatteryLevel = NpadBatteryLevel::Full;
        section.leftBatteryLevel = NpadBatteryLevel::Full;
        section.rightBatteryLevel = NpadBatteryLevel::Full;

        type = newType;
        controllerInfo = &GetControllerInfo();

        if (type != NpadControllerType::Gamecube) {
            sixAxisInfoLeft = &GetSixAxisInfo(MotionId::Left);
            if (type == NpadControllerType::JoyconDual)
                sixAxisInfoRight = &GetSixAxisInfo(MotionId::Right);
        }

        UpdateControllerSharedMemory();
        UpdateSixAxisSharedMemory();
        updateEvent->Signal();
    }

    void NpadDevice::Disconnect() {
        if (type == NpadControllerType::None)
            return;

        ResetDeviceProperties();
        connectionState = {};
        controllerState = {};
        defaultState = {};
        tracedControllerState = {};
        tracedDefaultState = {};
        traceStateInitialized = false;
        sixAxisStateLeft = {};
        sixAxisStateRight = {};

        index = -1;
        partnerIndex = -1;

        type = NpadControllerType::None;
        controllerInfo = nullptr;
        sixAxisInfoLeft = nullptr;
        sixAxisInfoRight = nullptr;

        updateEvent->Signal();
        WriteEmptyEntries();
    }

    NpadControllerInfo &NpadDevice::GetControllerInfo() {
        switch (type) {
            case NpadControllerType::ProController:
            case NpadControllerType::Gamecube:
                return section.fullKeyController;
            case NpadControllerType::Handheld:
                return section.handheldController;
            case NpadControllerType::JoyconDual:
                return section.dualController;
            case NpadControllerType::JoyconLeft:
                return section.leftController;
            case NpadControllerType::JoyconRight:
                return section.rightController;
            default:
                throw exception("Cannot find corresponding section for ControllerType: {}", type);
        }
    }

    NpadSixAxisInfo &NpadDevice::GetSixAxisInfo(MotionId id) {
        switch (type) {
            case NpadControllerType::ProController:
                return section.fullKeySixAxis;
            case NpadControllerType::Handheld:
                return section.handheldSixAxis;
            case NpadControllerType::JoyconDual:
                if (id == MotionId::Right)
                    return section.dualRightSixAxis;
                return section.dualLeftSixAxis;
            case NpadControllerType::JoyconLeft:
                return section.leftSixAxis;
            case NpadControllerType::JoyconRight:
                return section.rightSixAxis;
            default:
                throw exception("Cannot find corresponding section for ControllerType: {}", type);
        }
    }

    void NpadDevice::WriteNextEntry(NpadControllerInfo &info, NpadControllerState entry) {
        auto &lastEntry{info.state.at(info.header.currentEntry)};

        info.header.timestamp = util::GetTimeTicks();
        info.header.entryCount = constant::HidEntryCount;
        info.header.maxEntry = std::min<u64>(info.header.maxEntry + 1, constant::HidEntryCount - 1);
        info.header.currentEntry = (info.header.currentEntry + 1) % constant::HidEntryCount;

        auto &nextEntry{info.state.at(info.header.currentEntry)};

        const auto nextSamplingNumber{lastEntry.localTimestamp + 1};
        const auto completedMarker{nextSamplingNumber << 1};
        __atomic_store_n(&nextEntry.globalTimestamp, completedMarker | 1, __ATOMIC_RELAXED);
        nextEntry.localTimestamp = nextSamplingNumber;
        nextEntry.buttons = entry.buttons;
        nextEntry.leftX = entry.leftX;
        nextEntry.leftY = entry.leftY;
        nextEntry.rightX = entry.rightX;
        nextEntry.rightY = entry.rightY;
        nextEntry.status.raw = connectionState.raw;
        __atomic_store_n(&nextEntry.globalTimestamp, completedMarker, __ATOMIC_RELEASE);
    }

    void NpadDevice::WriteNextEntry(NpadSixAxisInfo &info, NpadSixAxisState entry) {
        auto &lastEntry{info.state.at(info.header.currentEntry)};

        info.header.timestamp = util::GetTimeTicks();
        info.header.entryCount = constant::HidEntryCount;
        info.header.maxEntry = std::min<u64>(info.header.maxEntry + 1, constant::HidEntryCount - 1);
        info.header.currentEntry = (info.header.currentEntry + 1) % constant::HidEntryCount;

        auto &nextEntry{info.state.at(info.header.currentEntry)};

        const auto nextSamplingNumber{lastEntry.localTimestamp + 1};
        const auto completedMarker{nextSamplingNumber << 1};
        __atomic_store_n(&nextEntry.globalTimestamp, completedMarker | 1, __ATOMIC_RELAXED);
        nextEntry.deltaTimestamp = entry.deltaTimestamp;
        nextEntry.localTimestamp = nextSamplingNumber;
        nextEntry.accelerometer = entry.accelerometer;
        nextEntry.gyroscope = entry.gyroscope;
        nextEntry.rotation = entry.rotation;
        nextEntry.orientation = entry.orientation;
        nextEntry.attribute = entry.attribute;
        __atomic_store_n(&nextEntry.globalTimestamp, completedMarker, __ATOMIC_RELEASE);
    }

    void NpadDevice::WriteEmptyEntries() {
        NpadControllerState emptyEntry{};

        WriteNextEntry(section.fullKeyController, emptyEntry);
        WriteNextEntry(section.handheldController, emptyEntry);
        WriteNextEntry(section.leftController, emptyEntry);
        WriteNextEntry(section.rightController, emptyEntry);
        WriteNextEntry(section.palmaController, emptyEntry);
        WriteNextEntry(section.dualController, emptyEntry);
        WriteNextEntry(section.defaultController, emptyEntry);

        globalTimestamp++;
    }

    void NpadDevice::ResetDeviceProperties() {
        // Don't reset assignment mode or ring lifo entries these values are persistent
        section.header.type = NpadControllerType::None;
        section.header.singleColor = {};
        section.header.leftColor = {};
        section.header.rightColor = {};
        section.header.singleColorStatus =  NpadColorReadStatus::Disconnected;
        section.header.dualColorStatus = NpadColorReadStatus::Disconnected;
        section.deviceType.raw = 0;
        section.buttonProperties.raw = 0;
        section.systemProperties.raw = 0;
        section.singleBatteryLevel = NpadBatteryLevel::Empty;
        section.leftBatteryLevel = NpadBatteryLevel::Empty;
        section.rightBatteryLevel = NpadBatteryLevel::Empty;
    }

    void NpadDevice::UpdateControllerSharedMemory() {
        if (!connectionState.connected)
            return;

        const auto stateChanged = [](const NpadControllerState &lhs, const NpadControllerState &rhs) {
            return lhs.buttons.raw != rhs.buttons.raw ||
                   lhs.leftX != rhs.leftX || lhs.leftY != rhs.leftY ||
                   lhs.rightX != rhs.rightX || lhs.rightY != rhs.rightY ||
                   lhs.status.raw != rhs.status.raw;
        };
        const bool shouldTrace{
            !traceStateInitialized ||
            stateChanged(controllerState, tracedControllerState) ||
            stateChanged(defaultState, tracedDefaultState)};

        if (controllerInfo)
            WriteNextEntry(*controllerInfo, controllerState);
        WriteNextEntry(section.defaultController, defaultState);

        if (shouldTrace) {
            if (controllerInfo) {
                const auto &activeEntry{controllerInfo->state.at(controllerInfo->header.currentEntry)};
                LOGI("DSR-NPAD active id=0x{:X} type=0x{:X} tail={} count={} sample={} marker={} buttons=0x{:016X} LX={} LY={} RX={} RY={} status=0x{:X}",
                     static_cast<u32>(id),
                     static_cast<u32>(type),
                     controllerInfo->header.currentEntry,
                     controllerInfo->header.maxEntry,
                     activeEntry.localTimestamp,
                     activeEntry.globalTimestamp,
                     activeEntry.buttons.raw,
                     activeEntry.leftX,
                     activeEntry.leftY,
                     activeEntry.rightX,
                     activeEntry.rightY,
                     activeEntry.status.raw);
            }

            const auto &defaultEntry{section.defaultController.state.at(section.defaultController.header.currentEntry)};
            LOGI("DSR-NPAD systemExt id=0x{:X} type=0x{:X} tail={} count={} sample={} marker={} buttons=0x{:016X} LX={} LY={} RX={} RY={} status=0x{:X}",
                 static_cast<u32>(id),
                 static_cast<u32>(type),
                 section.defaultController.header.currentEntry,
                 section.defaultController.header.maxEntry,
                 defaultEntry.localTimestamp,
                 defaultEntry.globalTimestamp,
                 defaultEntry.buttons.raw,
                 defaultEntry.leftX,
                 defaultEntry.leftY,
                 defaultEntry.rightX,
                 defaultEntry.rightY,
                 defaultEntry.status.raw);

            tracedControllerState = controllerState;
            tracedDefaultState = defaultState;
            traceStateInitialized = true;
        }

        globalTimestamp++;
    }

    void NpadDevice::UpdateSixAxisSharedMemory() {
        if (!connectionState.connected)
            return;

        const auto makeEntry = [](const NpadSixAxisState &state, const SixAxisSensorConfig &config) {
            if (config.enabled)
                return state;

            NpadSixAxisState entry{};
            entry.deltaTimestamp = 5'000'000;
            entry.accelerometer.z = -1.0F;
            entry.orientation[0].x = 1.0F;
            entry.orientation[1].y = 1.0F;
            entry.orientation[2].z = 1.0F;
            entry.attribute.isConnected = true;
            return entry;
        };

        if (sixAxisInfoLeft) {
            const size_t configIndex{type == NpadControllerType::ProController || type == NpadControllerType::Handheld ? 2U : 0U};
            WriteNextEntry(*sixAxisInfoLeft, makeEntry(sixAxisStateLeft, sixAxisConfigs[configIndex]));
        }
        if (sixAxisInfoRight)
            WriteNextEntry(*sixAxisInfoRight, makeEntry(sixAxisStateRight, sixAxisConfigs[1]));
    }

    void NpadDevice::SetButtonState(NpadButton mask, bool pressed) {
        std::scoped_lock lock{manager.mutex};
        if (pressed)
            controllerState.buttons.raw |= mask.raw;
        else
            controllerState.buttons.raw &= ~mask.raw;

        if (manager.orientation == NpadJoyOrientation::Horizontal && (type == NpadControllerType::JoyconLeft || type == NpadControllerType::JoyconRight)) {
            NpadButton orientedMask{};

            if (mask.dpadUp)
                orientedMask.dpadLeft = true;
            if (mask.dpadDown)
                orientedMask.dpadRight = true;
            if (mask.dpadLeft)
                orientedMask.dpadDown = true;
            if (mask.dpadRight)
                orientedMask.dpadUp = true;

            if (mask.leftSl || mask.rightSl)
                orientedMask.l = true;
            if (mask.leftSr || mask.rightSr)
                orientedMask.r = true;

            orientedMask.a = mask.a;
            orientedMask.b = mask.b;
            orientedMask.x = mask.x;
            orientedMask.y = mask.y;
            orientedMask.leftStick = mask.leftStick;
            orientedMask.rightStick = mask.rightStick;
            orientedMask.plus = mask.plus;
            orientedMask.minus = mask.minus;
            orientedMask.leftSl = mask.leftSl;
            orientedMask.leftSr = mask.leftSr;
            orientedMask.rightSl = mask.rightSl;
            orientedMask.rightSr = mask.rightSr;

            mask = orientedMask;
        }

        if (pressed)
            defaultState.buttons.raw |= mask.raw;
        else
            defaultState.buttons.raw &= ~mask.raw;
    }

    void NpadDevice::SetAxisValue(NpadAxisId axis, i32 value) {
        std::scoped_lock lock{manager.mutex};
        const auto previousControllerState{controllerState};
        const auto previousDefaultState{defaultState};
        constexpr i16 threshold{std::numeric_limits<i16>::max() / 2}; // A 50% deadzone for the stick buttons

        if (manager.orientation == NpadJoyOrientation::Vertical || (type != NpadControllerType::JoyconLeft && type != NpadControllerType::JoyconRight)) {
            switch (axis) {
                case NpadAxisId::LX:
                    controllerState.leftX = value;
                    defaultState.leftX = value;

                    controllerState.buttons.leftStickLeft = controllerState.leftX <= -threshold;
                    defaultState.buttons.leftStickLeft = controllerState.buttons.leftStickLeft;

                    controllerState.buttons.leftStickRight = controllerState.leftX >= threshold;
                    defaultState.buttons.leftStickRight = controllerState.buttons.leftStickRight;
                    break;
                case NpadAxisId::LY:
                    controllerState.leftY = value;
                    defaultState.leftY = value;

                    controllerState.buttons.leftStickUp = controllerState.leftY >= threshold;
                    defaultState.buttons.leftStickUp = controllerState.buttons.leftStickUp;

                    controllerState.buttons.leftStickDown = controllerState.leftY <= -threshold;
                    defaultState.buttons.leftStickDown = controllerState.buttons.leftStickDown;
                    break;
                case NpadAxisId::RX:
                    controllerState.rightX = value;
                    defaultState.rightX = value;

                    controllerState.buttons.rightStickLeft = controllerState.rightX <= -threshold;
                    defaultState.buttons.rightStickLeft = controllerState.buttons.rightStickLeft;

                    controllerState.buttons.rightStickRight = controllerState.rightX >= threshold;
                    defaultState.buttons.rightStickRight = controllerState.buttons.rightStickRight;
                    break;
                case NpadAxisId::RY:
                    controllerState.rightY = value;
                    defaultState.rightY = value;

                    controllerState.buttons.rightStickUp = controllerState.rightY >= threshold;
                    defaultState.buttons.rightStickUp = controllerState.buttons.rightStickUp;

                    controllerState.buttons.rightStickDown = controllerState.rightY <= -threshold;
                    defaultState.buttons.rightStickDown = controllerState.buttons.rightStickDown;
                    break;
            }
        } else {
            switch (axis) {
                case NpadAxisId::LX:
                    controllerState.leftY = value;
                    controllerState.buttons.leftStickUp = controllerState.leftY >= threshold;
                    controllerState.buttons.leftStickDown = controllerState.leftY <= -threshold;

                    defaultState.leftX = value;
                    defaultState.buttons.leftStickLeft = defaultState.leftX <= -threshold;
                    defaultState.buttons.leftStickRight = defaultState.leftX >= threshold;
                    break;
                case NpadAxisId::LY:
                    controllerState.leftX = -value;
                    controllerState.buttons.leftStickLeft = controllerState.leftX <= -threshold;
                    controllerState.buttons.leftStickRight = controllerState.leftX >= threshold;

                    defaultState.leftY = value;
                    defaultState.buttons.leftStickUp = defaultState.leftY >= threshold;
                    defaultState.buttons.leftStickDown = defaultState.leftY <= -threshold;
                    break;
                case NpadAxisId::RX:
                    controllerState.rightY = value;
                    controllerState.buttons.rightStickUp = controllerState.rightY >= threshold;
                    controllerState.buttons.rightStickDown = controllerState.rightY <= -threshold;

                    defaultState.rightX = value;
                    defaultState.buttons.rightStickLeft = defaultState.rightX <= -threshold;
                    defaultState.buttons.rightStickRight = defaultState.rightX >= threshold;
                    break;
                case NpadAxisId::RY:
                    controllerState.rightX = -value;
                    controllerState.buttons.rightStickLeft = controllerState.rightX <= -threshold;
                    controllerState.buttons.rightStickRight = controllerState.rightX >= threshold;

                    defaultState.rightY = value;
                    defaultState.buttons.rightStickUp = defaultState.rightY >= threshold;
                    defaultState.buttons.rightStickDown = defaultState.rightY <= -threshold;
                    break;
            }
        }

        LOGI("DSR-AXIS id=0x{:X} type=0x{:X} axis={} value={} beforeL=({}, {}) afterL=({}, {}) beforeButtons=0x{:016X} afterButtons=0x{:016X} systemExtL=({}, {}) systemExtButtons=0x{:016X}",
             static_cast<u32>(id),
             static_cast<u32>(type),
             static_cast<u32>(axis),
             value,
             previousControllerState.leftX,
             previousControllerState.leftY,
             controllerState.leftX,
             controllerState.leftY,
             previousControllerState.buttons.raw,
             controllerState.buttons.raw,
             defaultState.leftX,
             defaultState.leftY,
             defaultState.buttons.raw);
    }

    void NpadDevice::SetMotionValue(MotionId sensor, MotionSensorState *value) {
        std::scoped_lock lock{manager.mutex};
        if (!connectionState.connected)
            return;

        NpadSixAxisState *sixAxisState{sensor == MotionId::Right? &sixAxisStateRight : &sixAxisStateLeft};

        sixAxisState->accelerometer.x = value->accelerometer[0];
        sixAxisState->accelerometer.y = value->accelerometer[1];
        sixAxisState->accelerometer.z = value->accelerometer[2];

        sixAxisState->gyroscope.x = value->gyroscope[0];
        sixAxisState->gyroscope.y = value->gyroscope[1];
        sixAxisState->gyroscope.z = value->gyroscope[2];

        float deltaTime{static_cast<float>(value->deltaTimestamp) / 1000000000.0f};
        sixAxisState->rotation.x += value->gyroscope[0] * deltaTime;
        sixAxisState->rotation.y += value->gyroscope[1] * deltaTime;
        sixAxisState->rotation.z += value->gyroscope[2] * deltaTime;

        sixAxisState->orientation[0].x = value->orientationMatrix[0];
        sixAxisState->orientation[0].y = value->orientationMatrix[1];
        sixAxisState->orientation[0].z = value->orientationMatrix[2];
        sixAxisState->orientation[1].x = value->orientationMatrix[3];
        sixAxisState->orientation[1].y = value->orientationMatrix[4];
        sixAxisState->orientation[1].z = value->orientationMatrix[5];
        sixAxisState->orientation[2].x = value->orientationMatrix[6];
        sixAxisState->orientation[2].y = value->orientationMatrix[7];
        sixAxisState->orientation[2].z = value->orientationMatrix[8];

        sixAxisState->deltaTimestamp = value->deltaTimestamp;
        sixAxisState->attribute.isConnected = true;

        const auto square = [](float component) { return component * component; };
        const float accelerationMagnitudeSquared{square(value->accelerometer[0]) + square(value->accelerometer[1]) + square(value->accelerometer[2])};
        const float angularVelocitySquared{square(value->gyroscope[0]) + square(value->gyroscope[1]) + square(value->gyroscope[2])};
        const size_t configIndex{sensor == MotionId::Right ? 1U : (type == NpadControllerType::ProController || type == NpadControllerType::Handheld ? 2U : 0U)};
        sixAxisConfigs[configIndex].atRest = std::abs(accelerationMagnitudeSquared - 1.0F) < 0.1F && angularVelocitySquared < 0.0025F;
    }

    constexpr jlong MsInSecond{1000}; //!< The amount of milliseconds in a single second of time
    constexpr jint AmplitudeMax{std::numeric_limits<u8>::max()}; //!< The maximum amplitude for Android Vibration APIs

    struct VibrationInfo {
        jlong period;
        jint amplitude;
        jlong start; //!< The timestamp to (re)start the vibration at
        jlong end; //!< The timestamp to end the vibration at

        static bool IsValid(float frequency, float amplitude) {
            return std::isfinite(frequency) && frequency > 0.0F && std::isfinite(amplitude) && amplitude > 0.0F;
        }

        VibrationInfo(float frequency, float amplitude)
            : period(IsValid(frequency, amplitude) ? std::max<jlong>(1, static_cast<jlong>(std::lround(MsInSecond / frequency))) : 1),
              amplitude(IsValid(frequency, amplitude) ? std::clamp(static_cast<jint>(std::lround(amplitude)), 0, AmplitudeMax) : 0),
              start(0), end(period) {}
    };

    template<size_t Size>
    void VibrateDevice(const std::shared_ptr<JvmManager> &jvm, i8 deviceIndex, std::array<VibrationInfo, Size> vibrations) {
        jint totalAmplitude{};
        for (const auto &vibration : vibrations)
            totalAmplitude += vibration.amplitude;
        if (totalAmplitude == 0) {
            jvm->ClearVibrationDevice(deviceIndex); // If a null vibration was submitted then we just clear vibrations on the device
            return;
        }

        // We output an approximation of the combined + linearized vibration data into these arrays, larger arrays would allow for more accurate reproduction of data
        std::array<jlong, 50> timings;
        std::array<jint, 50> amplitudes;

        // We are essentially unrolling the bands into a linear sequence, due to the data not being always linearizable there will be inaccuracies at the ends unless there's a pattern that's repeatable which will happen when all band's frequencies are factors of each other
        jint currentAmplitude{}; //!< The accumulated amplitude from adding up and subtracting the amplitude of individual bands
        jlong currentTime{}; //!< The accumulated time passed by adding up all the periods prior to the current vibration cycle
        size_t index{};
        for (; index < timings.size(); index++) {
            jlong cyclePeriod{}; //!< The length of this cycle, calculated as the largest period with the same amplitude
            size_t bandStartCount{}; //!< The amount of bands that start their vibration cycles in this time slot

            for (size_t vibrationIndex{}; vibrationIndex < vibrations.size(); vibrationIndex++) {
                // Iterate over every band to calculate the amplitude for this time slot
                VibrationInfo &vibration{vibrations[vibrationIndex]};
                if (currentTime <= vibration.start) {
                    // If the time to start has arrived then start the vibration
                    vibration.end = vibration.start + vibration.period;
                    currentAmplitude += vibration.amplitude;
                    auto vibrationPeriodLeft{vibration.end - currentTime};
                    cyclePeriod = cyclePeriod ? std::min(vibrationPeriodLeft, cyclePeriod) : vibrationPeriodLeft;

                    bandStartCount++;
                } else if (currentTime <= vibration.end) {
                    // If the time to end the vibration has arrived then end it
                    vibration.start = vibration.end + vibration.period;
                    currentAmplitude -= vibration.amplitude;
                    auto vibrationPeriodLeft{vibration.start - currentTime};
                    cyclePeriod = cyclePeriod ? std::min(vibrationPeriodLeft, cyclePeriod) : vibrationPeriodLeft;
                }
            }

            if (index && bandStartCount == vibrations.size())
                break; // If all bands start again at this point then we can end the pattern here and just loop over the pattern

            currentTime += cyclePeriod;
            timings[index] = cyclePeriod;

            amplitudes[index] = std::min(currentAmplitude, AmplitudeMax);
        }

        jvm->VibrateDevice(deviceIndex, span(timings.begin(), timings.begin() + index), span(amplitudes.begin(), amplitudes.begin() + index));
    }

    void VibrateDevice(const std::shared_ptr<JvmManager> &jvm, i8 index, const NpadVibrationValue &value) {
        std::array<VibrationInfo, 2> vibrations{
            VibrationInfo{value.frequencyLow, value.amplitudeLow * (AmplitudeMax / 2)},
            VibrationInfo{value.frequencyHigh, value.amplitudeHigh * (AmplitudeMax / 2)},
        };
        VibrateDevice(jvm, index, vibrations);
    }

    void NpadDevice::Vibrate(const NpadVibrationValue &left, const NpadVibrationValue &right) {
        std::scoped_lock lock{manager.mutex};
        if (vibrationLeft == left && vibrationRight && (*vibrationRight) == right)
            return;

        vibrationLeft = left;
        vibrationRight = right;

        if (partnerIndex == NpadDevice::NullIndex) {
            std::array<VibrationInfo, 4> vibrations{
                VibrationInfo{left.frequencyLow, left.amplitudeLow * (AmplitudeMax / 4)},
                VibrationInfo{left.frequencyHigh, left.amplitudeHigh * (AmplitudeMax / 4)},
                VibrationInfo{right.frequencyLow, right.amplitudeLow * (AmplitudeMax / 4)},
                VibrationInfo{right.frequencyHigh, right.amplitudeHigh * (AmplitudeMax / 4)},
            };
            VibrateDevice(manager.state.jvm, index, vibrations);
        } else {
            VibrateDevice(manager.state.jvm, index, left);
            VibrateDevice(manager.state.jvm, partnerIndex, right);
        }
    }

    void NpadDevice::VibrateSingle(bool isRight, const NpadVibrationValue &value) {
        std::scoped_lock lock{manager.mutex};
        if (vibrationRight) {
            const auto left{isRight ? vibrationLeft : value};
            const auto right{isRight ? value : *vibrationRight};
            Vibrate(left, right);
        } else {
            if (!isRight && vibrationLeft == value)
                return;

            if (isRight)
                vibrationRight = value;
            else
                vibrationLeft = value;
            VibrateDevice(manager.state.jvm, index, value);
        }
    }

    void NpadDevice::ActivateVibrationDevice(const NpadDeviceHandle &handle) {
        std::scoped_lock lock{manager.mutex};
        if (handle.deviceIndex >= activeVibrationTypes.size())
            return;
        if (activeVibrationTypes[handle.deviceIndex] == handle.GetType())
            return;

        activeVibrationTypes[handle.deviceIndex] = handle.GetType();
        if (handle.isRight)
            vibrationRight = DefaultNpadVibrationValue;
        else
            vibrationLeft = DefaultNpadVibrationValue;
    }

    bool NpadDevice::IsVibrationDeviceActive(const NpadDeviceHandle &handle) {
        std::scoped_lock lock{manager.mutex};
        return handle.deviceIndex < activeVibrationTypes.size() && activeVibrationTypes[handle.deviceIndex] == handle.GetType();
    }

    bool NpadDevice::IsVibrationDeviceMounted(const NpadDeviceHandle &handle) {
        std::scoped_lock lock{manager.mutex};
        return IsVibrationDeviceActive(handle) && connectionState.connected && type == handle.GetType();
    }

    NpadVibrationValue NpadDevice::GetActualVibrationValue(const NpadDeviceHandle &handle) {
        std::scoped_lock lock{manager.mutex};
        if (!IsVibrationDeviceMounted(handle))
            return DefaultNpadVibrationValue;

        return handle.isRight ? vibrationRight.value_or(DefaultNpadVibrationValue) : vibrationLeft;
    }

    void NpadDevice::StopVibration() {
        std::scoped_lock lock{manager.mutex};
        if (!connectionState.connected || index == NullIndex) {
            vibrationLeft = DefaultNpadVibrationValue;
            if (vibrationRight)
                vibrationRight = DefaultNpadVibrationValue;
            return;
        }

        if (activeVibrationTypes[0])
            VibrateSingle(false, DefaultNpadVibrationValue);
        if (activeVibrationTypes[1])
            VibrateSingle(true, DefaultNpadVibrationValue);
    }
}
