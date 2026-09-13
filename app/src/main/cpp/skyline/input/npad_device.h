// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <kernel/types/KEvent.h>
#include "shared_mem.h"

namespace skyline::input {

    /**
     * @brief Motion sensor location
     */
    enum class MotionId {
        Left,
        Right,
        Console,
    };

    /**
     * @brief A description of a motion event
     * @note This structure corresponds to MotionSensorInput, see that for details
     */
    struct MotionSensorState {
        u64 timestamp;
        u64 deltaTimestamp;
        std::array<float,3> gyroscope;
        std::array<float,3> accelerometer;
        std::array<float,4> quaternion;
        std::array<float,9> orientationMatrix;
    };
    static_assert(sizeof(MotionSensorState) == 0x60);

    /**
     * @brief How many joycons must be attached for handheld mode to be triggered
     */
    enum class NpadHandheldActivationMode : u64 {
        Dual = 0,
        Single = 1,
        None = 2,
    };

    /**
     * @brief The orientations the Joy-Con(s) can be held in
     */
    enum class NpadJoyOrientation : i64 {
        Vertical = 0,
        Horizontal = 1,
    };

    /**
     * @url https://switchbrew.org/wiki/HID_services#NpadStyleTag
     */
    union NpadStyleSet {
        u32 raw;
        struct {
            bool proController : 1; //!< Pro Controller
            bool joyconHandheld : 1; //!< Joy-Cons in handheld mode
            bool joyconDual : 1; //!< Joy-Cons in a pair
            bool joyconLeft : 1; //!< Left Joy-Con only
            bool joyconRight : 1; //!< Right Joy-Con only
            bool gamecube : 1; //!< GameCube controller
            bool palma : 1; //!< Poké Ball Plus controller
            bool nes : 1; //!< NES controller
            bool nesHandheld : 1; //!< NES controller in handheld mode
            bool snes : 1; //!< SNES controller
            bool n64 : 1; //!< Nintendo 64 controller
            bool segaGenesis : 1; //!< Sega Genesis controller
            u32 _reserved_ : 17;
            bool systemExt : 1; //!< Generic external controller
            bool system : 1; //!< Generic system controller
        };
    };
    static_assert(sizeof(NpadStyleSet) == 0x4);

    enum class NpadAxisId {
        LX, //!< Left Stick X
        LY, //!< Left Stick Y
        RX, //!< Right Stick X
        RY, //!< Right Stick Y
    };

    /**
     * @url https://switchbrew.org/wiki/HID_services#NpadIdType
     */
    enum class NpadId : u32 {
        Player1 = 0x0,
        Player2 = 0x1,
        Player3 = 0x2,
        Player4 = 0x3,
        Player5 = 0x4,
        Player6 = 0x5,
        Player7 = 0x6,
        Player8 = 0x7,
        Unknown = 0x10,
        Handheld = 0x20,
    };

    /**
     * @brief A handle to a specific device addressed by its ID and type
     * @note This is used by both Six-Axis and Vibration
     * @url https://switchbrew.org/wiki/HID_services#SixAxisSensorHandle
     * @url https://switchbrew.org/wiki/HID_services#VibrationDeviceHandle
     */
    union __attribute__((__packed__)) NpadDeviceHandle {
        u32 raw{};
        struct {
            u8 type;
            NpadId id : 8;
            union {
                u8 deviceIndex;
                struct {
                    bool isRight : 1; //!< Right Joy-Con or right actuator
                    bool isSixAxisSingle : 1; //!< FullKey/Handheld single IMU (device index 2)
                };
            };
            u8 padding;
        };

        constexpr NpadControllerType GetType() const {
            switch (type) {
                case 3:
                    return NpadControllerType::ProController;
                case 4:
                    return NpadControllerType::Handheld;
                case 5:
                    return NpadControllerType::JoyconDual;
                case 6:
                    return NpadControllerType::JoyconLeft;
                case 7:
                    return NpadControllerType::JoyconRight;
                case 8:
                    return NpadControllerType::Gamecube;
                default:
                    return NpadControllerType::None;
            }
        }
    };
    static_assert(sizeof(NpadDeviceHandle) == 0x4);
    static_assert(alignof(NpadDeviceHandle) == 0x1);

    struct SixAxisSensorFusionParameters {
        float revisePower{0.03F};
        float reviseRange{0.4F};
    };
    static_assert(sizeof(SixAxisSensorFusionParameters) == 0x8);

    /**
     * @url https://switchbrew.org/wiki/HID_services#VibrationDeviceInfo
     */
    struct NpadVibrationDeviceInfo {
        NpadVibrationDeviceType deviceType;
        NpadVibrationDevicePosition position;
    };
    static_assert(sizeof(NpadVibrationDeviceInfo) == 0x8);

    /**
     * @brief The parameters to produce a vibration using an LRA
     * @note The vibration is broken into a frequency band with the lower and high range supplied
     * @note Amplitude is in arbitrary units from 0f to 1f
     * @note Frequency is in Hertz
     */
    struct NpadVibrationValue {
        float amplitudeLow;
        float frequencyLow;
        float amplitudeHigh;
        float frequencyHigh;

        constexpr bool operator==(const NpadVibrationValue &) const = default;
    };
    static_assert(sizeof(NpadVibrationValue) == 0x10);

    inline constexpr NpadVibrationValue DefaultNpadVibrationValue{
        .amplitudeLow = 0.0F,
        .frequencyLow = 160.0F,
        .amplitudeHigh = 0.0F,
        .frequencyHigh = 320.0F,
    };

    /**
     * @url https://switchbrew.org/wiki/HID_services#GyroscopeZeroDriftMode
     */
    enum class GyroscopeZeroDriftMode : u32 {
        Loose = 0,
        Standard = 1,
        Tight = 2,
    };

    struct SixAxisSensorConfig {
        bool enabled{};
        bool fusionEnabled{true};
        bool unalteredPassthrough{};
        bool newlyAssigned{true};
        bool atRest{true};
        SixAxisSensorFusionParameters fusion{};
        GyroscopeZeroDriftMode gyroZeroDriftMode{GyroscopeZeroDriftMode::Standard};
    };

    class NpadManager;

    /**
     * @brief An easy to use interface for a NPad which abstracts away the complicated details
     */
    class NpadDevice {
      private:
        NpadManager &manager; //!< The manager responsible for managing this NpadDevice
        NpadSection &section; //!< The section in HID shared memory for this controller
        NpadControllerInfo *controllerInfo{}; //!< The NpadControllerInfo for this controller's type
        NpadSixAxisInfo *sixAxisInfoLeft{}; //!< The NpadSixAxisInfo for the main or left side of this controller's type
        NpadSixAxisInfo *sixAxisInfoRight{}; //!< The NpadSixAxisInfo for the right side of this controller's type
        u64 globalTimestamp{}; //!< An incrementing timestamp that's common across all sections
        NpadControllerState controllerState{}, defaultState{}; //!< The current state of the controller (normal and default)
        NpadSixAxisState sixAxisStateLeft{}, sixAxisStateRight{}; //!< The current state of the sixaxis (left and right)
        std::array<SixAxisSensorConfig, 3> sixAxisConfigs{}; //!< State for left, right and single-unit handles

        /**
         * @brief Updates the headers and writes a new entry in HID Shared Memory
         * @param info The controller info of the NPad that needs to be updated
         * @param entry An entry with the state of the controller
         */
        void WriteNextEntry(NpadControllerInfo &info, NpadControllerState entry);

        /**
         * @brief Updates the headers and writes a new entry in HID Shared Memory
         * @param info The sixaxis controller info of the NPad that needs to be updated
         * @param entry An entry with the state of the controller
         */
        void WriteNextEntry(NpadSixAxisInfo &info, NpadSixAxisState entry);

        /**
         * @brief Writes on all ring lifo buffers a new empty entry in HID Shared Memory
         */
        void WriteEmptyEntries();

        /**
         * @brief Reverts all device properties to the default state
         */
        void ResetDeviceProperties();

        /**
         * @return The NpadControllerInfo for this controller based on its type
         */
        NpadControllerInfo &GetControllerInfo();

        /**
         * @return The NpadSixAxisInfo for this controller based on its type
         */
        NpadSixAxisInfo &GetSixAxisInfo(MotionId id);

      public:
        NpadId id;
        static constexpr i8 NullIndex{-1}; //!< The placeholder index value when there is no device present
        i8 index{NullIndex}; //!< The index of the device assigned to this player
        i8 partnerIndex{NullIndex}; //!< The index of a partner device, if present
        NpadVibrationValue vibrationLeft{DefaultNpadVibrationValue}; //!< Vibration for the left Joy-Con (Handheld/Pair), left LRA in a Pro-Controller or individual Joy-Cons
        std::optional<NpadVibrationValue> vibrationRight; //!< Vibration for the right Joy-Con (Handheld/Pair) or right LRA in a Pro-Controller
        std::array<std::optional<NpadControllerType>, 2> activeVibrationTypes; //!< Activated vibration handle type for each actuator
        NpadControllerType type{};
        NpadConnectionState connectionState{};
        std::shared_ptr<kernel::type::KEvent> updateEvent; //!< This event is triggered on the controller's style changing

        NpadDevice(NpadManager &manager, NpadSection &section, NpadId id);

        void SetAssignment(NpadJoyAssignment assignment) {
            section.header.assignment = assignment;
        }

        NpadJoyAssignment GetAssignment() {
            return section.header.assignment;
        }

        /**
         * @brief Connects this controller to the guest
         */
        void Connect(NpadControllerType newType);

        /**
         * @brief Disconnects this controller from the guest
         */
        void Disconnect();

        /**
         * @brief Writes the current state of the controller to HID shared memory
         */
        void UpdateControllerSharedMemory();

        /** Writes SixAxis state on its native 5 ms sampling cadence. */
        void UpdateSixAxisSharedMemory();

        SixAxisSensorConfig &GetSixAxisConfig(const NpadDeviceHandle &handle) {
            return sixAxisConfigs.at(handle.deviceIndex);
        }

        const SixAxisSensorConfig &GetSixAxisConfig(const NpadDeviceHandle &handle) const {
            return sixAxisConfigs.at(handle.deviceIndex);
        }

        /**
         * @brief Changes the state of buttons to the specified state
         * @param mask A bit-field mask of all the buttons to change
         * @param pressed If the buttons were pressed or released
         */
        void SetButtonState(NpadButton mask, bool pressed);

        /**
         * @brief Sets the value of an axis to the specified value
         * @param axis The axis to set the value of
         * @param value The value to set
         */
        void SetAxisValue(NpadAxisId axis, i32 value);

        /**
         * @brief Sets the value of a motion sensor to the specified value
         * @param motion The motion sensor to set the value of
         * @param value The value to set
         */
        void SetMotionValue(MotionId sensor, MotionSensorState *value);

        /**
         * @brief Sets the vibration for both the Joy-Cons to the specified vibration values
         */
        void Vibrate(const NpadVibrationValue &left, const NpadVibrationValue &right);

        /**
         * @brief Sets the vibration for either the left or right Joy-Con to the specified vibration value
         */
        void VibrateSingle(bool isRight, const NpadVibrationValue &value);

        void ActivateVibrationDevice(const NpadDeviceHandle &handle);
        bool IsVibrationDeviceActive(const NpadDeviceHandle &handle);
        bool IsVibrationDeviceMounted(const NpadDeviceHandle &handle);
        NpadVibrationValue GetActualVibrationValue(const NpadDeviceHandle &handle);
        void StopVibration();
    };
}
