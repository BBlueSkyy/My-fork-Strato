// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "common.h"
#include <jni.h>

namespace skyline {
    struct DhcpInfo {
        i32 ipAddress;
        i32 subnet;
        i32 gateway;
        i32 dns1;
        i32 dns2;
    };

    class JniString : public std::string {
      private:
        static std::string GetJString(JNIEnv *env, jstring jString);

      public:
        JniString(JNIEnv *env, jstring jString) : std::string(GetJString(env, jString)) {}
    };

    class KtSettings {
      private:
        JNIEnv *env;
        jclass settingsClass;
        jobject settingsInstance;

      public:
        KtSettings(JNIEnv *env, jobject settingsInstance) : env(env), settingsInstance(settingsInstance), settingsClass(env->GetObjectClass(settingsInstance)) {}
        KtSettings(const KtSettings &) = delete;
        void operator=(const KtSettings &) = delete;
        KtSettings(KtSettings &&) = default;

        template<typename T>
        requires std::is_integral_v<T> || std::is_enum_v<T>
        T GetInt(const std::string_view &key) {
            return static_cast<T>(env->GetIntField(settingsInstance, env->GetFieldID(settingsClass, key.data(), "I")));
        }

        bool GetBool(const std::string_view &key) {
            return env->GetBooleanField(settingsInstance, env->GetFieldID(settingsClass, key.data(), "Z")) == JNI_TRUE;
        }

        JniString GetString(const std::string_view &key) {
            return {env, static_cast<jstring>(env->GetObjectField(settingsInstance, env->GetFieldID(settingsClass, key.data(), "Ljava/lang/String;")))};
        }
    };

    class JvmManager {
      public:
        using KeyboardHandle = jobject;
        using KeyboardConfig = std::array<u8, 0x4C8>;
        using KeyboardCloseResult = u32;
        using KeyboardTextCheckResult = u32;

        struct KeyboardUpdate {
            enum class Type : u32 {
                Changed = 0,
                Enter = 1,
                Cancel = 2,
                Closed = 3,
            };

            Type type;
            std::u16string text;
            i32 cursor;
        };

        jobject instance;
        jclass instanceClass;

        JvmManager(JNIEnv *env, jobject instance);
        ~JvmManager();

        static JNIEnv *GetEnv();

        template<typename objectType>
        objectType GetField(const char *key) {
            JNIEnv *env{GetEnv()};
            if constexpr(std::is_same<objectType, jboolean>())
                return env->GetBooleanField(instance, env->GetFieldID(instanceClass, key, "Z"));
            else if constexpr(std::is_same<objectType, jbyte>())
                return env->GetByteField(instance, env->GetFieldID(instanceClass, key, "B"));
            else if constexpr(std::is_same<objectType, jchar>())
                return env->GetCharField(instance, env->GetFieldID(instanceClass, key, "C"));
            else if constexpr(std::is_same<objectType, jshort>())
                return env->GetShortField(instance, env->GetFieldID(instanceClass, key, "S"));
            else if constexpr(std::is_same<objectType, jint>())
                return env->GetIntField(instance, env->GetFieldID(instanceClass, key, "I"));
            else if constexpr(std::is_same<objectType, jlong>())
                return env->GetLongField(instance, env->GetFieldID(instanceClass, key, "J"));
            else if constexpr(std::is_same<objectType, jfloat>())
                return env->GetFloatField(instance, env->GetFieldID(instanceClass, key, "F"));
            else if constexpr(std::is_same<objectType, jdouble>())
                return env->GetDoubleField(instance, env->GetFieldID(instanceClass, key, "D"));
            else
                throw exception("GetField: Unhandled object type");
        }

        jobject GetField(const char *key, const char *signature);
        bool CheckNull(const char *key, const char *signature);
        static bool CheckNull(jobject &object);

        void InitializeControllers();
        void VibrateDevice(jint index, const span<jlong> &timings, const span<jint> &amplitudes);
        void ClearVibrationDevice(jint index);

        KeyboardHandle ShowKeyboard(KeyboardConfig &config, std::u16string initialText);
        KeyboardHandle CloneKeyboardHandle(KeyboardHandle dialog);
        void ReleaseKeyboardHandle(KeyboardHandle dialog);
        std::pair<KeyboardCloseResult, std::u16string> WaitForSubmitOrCancel(KeyboardHandle dialog);
        KeyboardUpdate WaitForInlineKeyboardUpdate(KeyboardHandle dialog);
        void CloseKeyboard(KeyboardHandle dialog);
        KeyboardCloseResult ShowValidationResult(KeyboardHandle dialog, KeyboardTextCheckResult checkResult, std::u16string message);

        void reportCrash();
        void ShowPipelineLoadingScreen(u32 totalPipelineCount);
        void UpdatePipelineLoadingProgress(u32 progress);
        void HidePipelineLoadingScreen();
        void UpdateShaderCompilationState(bool compiling);
        i32 GetVersionCode();
        DhcpInfo GetDhcpInfo();

      private:
        jmethodID initializeControllersId;
        jmethodID vibrateDeviceId;
        jmethodID clearVibrationDeviceId;

        jclass keyboardDialogClass{};
        jmethodID showKeyboardId;
        jmethodID waitForSubmitOrCancelId;
        jmethodID waitForInlineUpdateId{};
        jmethodID cancelInlineWaitId{};
        jmethodID closeKeyboardId;
        jmethodID showValidationResultId;
        jmethodID getIntegerValueId;
        jmethodID reportCrashId;

        jmethodID showPipelineLoadingScreenId;
        jmethodID updatePipelineLoadingProgressId;
        jmethodID hidePipelineLoadingScreenId;

        jclass shaderCompilationNotifierClass{};
        jmethodID updateShaderCompilationStateId{};

        jmethodID getVersionCodeId;
        jmethodID getDhcpInfoId;
    };
}
