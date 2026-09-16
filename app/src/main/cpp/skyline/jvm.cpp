// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "jvm.h"

namespace skyline {
    std::string JniString::GetJString(JNIEnv *env, jstring jString) {
        auto utf{env->GetStringUTFChars(jString, nullptr)};
        std::string string{utf};
        env->ReleaseStringUTFChars(jString, utf);
        return string;
    }

    struct JniEnvironment {
        JNIEnv *env{};
        static inline JavaVM *vm{};
        bool attached{};

        void Initialize(JNIEnv *environment) {
            env = environment;
            if (env->GetJavaVM(&vm) < 0)
                throw exception("Cannot get JavaVM from environment");
            attached = true;
        }

        JniEnvironment() {
            if (vm && !attached) {
                vm->AttachCurrentThread(&env, nullptr);
                attached = true;
            }
        }

        ~JniEnvironment() {
            if (vm && attached)
                vm->DetachCurrentThread();
        }

        operator JNIEnv *() {
            if (!attached)
                throw exception("Not attached");
            return env;
        }

        JNIEnv *operator->() {
            if (!attached)
                throw exception("Not attached");
            return env;
        }
    };

    thread_local inline JniEnvironment env;

    JvmManager::JvmManager(JNIEnv *environ, jobject instance)
        : instance{environ->NewGlobalRef(instance)},
          instanceClass{reinterpret_cast<jclass>(environ->NewGlobalRef(environ->GetObjectClass(instance)))},
          initializeControllersId{environ->GetMethodID(instanceClass, "initializeControllers", "()V")},
          vibrateDeviceId{environ->GetMethodID(instanceClass, "vibrateDevice", "(I[J[I)V")},
          clearVibrationDeviceId{environ->GetMethodID(instanceClass, "clearVibrationDevice", "(I)V")},
          showKeyboardId{environ->GetMethodID(instanceClass, "showKeyboard", "(Ljava/nio/ByteBuffer;Ljava/lang/String;)Lorg/stratoemu/strato/applet/swkbd/SoftwareKeyboardDialog;")},
          waitForSubmitOrCancelId{environ->GetMethodID(instanceClass, "waitForSubmitOrCancel", "(Lorg/stratoemu/strato/applet/swkbd/SoftwareKeyboardDialog;)[Ljava/lang/Object;")},
          closeKeyboardId{environ->GetMethodID(instanceClass, "closeKeyboard", "(Lorg/stratoemu/strato/applet/swkbd/SoftwareKeyboardDialog;)V")},
          showValidationResultId{environ->GetMethodID(instanceClass, "showValidationResult", "(Lorg/stratoemu/strato/applet/swkbd/SoftwareKeyboardDialog;ILjava/lang/String;)I")},
          getIntegerValueId{environ->GetMethodID(environ->FindClass("java/lang/Integer"), "intValue", "()I")},
          reportCrashId{environ->GetMethodID(instanceClass, "reportCrash", "()V")},
          showPipelineLoadingScreenId{environ->GetMethodID(instanceClass, "showPipelineLoadingScreen", "(I)V")},
          updatePipelineLoadingProgressId{environ->GetMethodID(instanceClass, "updatePipelineLoadingProgress", "(I)V")},
          hidePipelineLoadingScreenId{environ->GetMethodID(instanceClass, "hidePipelineLoadingScreen", "()V")},
          getVersionCodeId{environ->GetMethodID(instanceClass, "getVersionCode", "()I")},
          getDhcpInfoId{environ->GetMethodID(instanceClass, "getDhcpInfo", "()Landroid/net/DhcpInfo;")} {
        env.Initialize(environ);

        auto localInlineKeyboardClass{environ->FindClass("org/stratoemu/strato/applet/swkbd/InlineKeyboardInputView")};
        inlineKeyboardClass = reinterpret_cast<jclass>(environ->NewGlobalRef(localInlineKeyboardClass));
        showInlineKeyboardId = environ->GetStaticMethodID(
            inlineKeyboardClass,
            "show",
            "(Landroid/app/Activity;Ljava/nio/ByteBuffer;Ljava/lang/String;)Lorg/stratoemu/strato/applet/swkbd/InlineKeyboardInputView;");
        waitForInlineUpdateId = environ->GetMethodID(inlineKeyboardClass, "waitForInlineUpdate", "()[Ljava/lang/Object;");
        closeInlineKeyboardId = environ->GetStaticMethodID(
            inlineKeyboardClass,
            "close",
            "(Landroid/app/Activity;Lorg/stratoemu/strato/applet/swkbd/InlineKeyboardInputView;)V");
        environ->DeleteLocalRef(localInlineKeyboardClass);

        auto notifierClass{environ->FindClass("org/stratoemu/strato/ShaderCompilationNotifier")};
        shaderCompilationNotifierClass = reinterpret_cast<jclass>(environ->NewGlobalRef(notifierClass));
        updateShaderCompilationStateId = environ->GetStaticMethodID(shaderCompilationNotifierClass, "update", "(Landroid/app/Activity;Z)V");
        environ->DeleteLocalRef(notifierClass);
    }

    JvmManager::~JvmManager() {
        env->DeleteGlobalRef(shaderCompilationNotifierClass);
        env->DeleteGlobalRef(inlineKeyboardClass);
        env->DeleteGlobalRef(instanceClass);
        env->DeleteGlobalRef(instance);
    }

    JNIEnv *JvmManager::GetEnv() {
        return env;
    }

    jobject JvmManager::GetField(const char *key, const char *signature) {
        return env->GetObjectField(instance, env->GetFieldID(instanceClass, key, signature));
    }

    bool JvmManager::CheckNull(const char *key, const char *signature) {
        return env->IsSameObject(env->GetObjectField(instance, env->GetFieldID(instanceClass, key, signature)), nullptr);
    }

    bool JvmManager::CheckNull(jobject &object) {
        return env->IsSameObject(object, nullptr);
    }

    void JvmManager::InitializeControllers() {
        env->CallVoidMethod(instance, initializeControllersId);
    }

    void JvmManager::VibrateDevice(jint index, const span<jlong> &timings, const span<jint> &amplitudes) {
        auto jTimings{env->NewLongArray(static_cast<jsize>(timings.size()))};
        env->SetLongArrayRegion(jTimings, 0, static_cast<jsize>(timings.size()), timings.data());
        auto jAmplitudes{env->NewIntArray(static_cast<jsize>(amplitudes.size()))};
        env->SetIntArrayRegion(jAmplitudes, 0, static_cast<jsize>(amplitudes.size()), amplitudes.data());
        env->CallVoidMethod(instance, vibrateDeviceId, index, jTimings, jAmplitudes);
        env->DeleteLocalRef(jTimings);
        env->DeleteLocalRef(jAmplitudes);
    }

    void JvmManager::ClearVibrationDevice(jint index) {
        env->CallVoidMethod(instance, clearVibrationDeviceId);
    }

    JvmManager::KeyboardHandle JvmManager::ShowKeyboard(KeyboardConfig &config, std::u16string initialText) {
        auto buffer{env->NewDirectByteBuffer(&config, sizeof(KeyboardConfig))};
        auto str{env->NewString(reinterpret_cast<const jchar *>(initialText.data()), static_cast<int>(initialText.length()))};
        jobject localKeyboardDialog{env->CallObjectMethod(instance, showKeyboardId, buffer, str)};
        env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(str);
        if (!localKeyboardDialog)
            return {};

        auto keyboardDialog{env->NewGlobalRef(localKeyboardDialog)};
        env->DeleteLocalRef(localKeyboardDialog);
        return keyboardDialog;
    }

    JvmManager::KeyboardHandle JvmManager::ShowInlineKeyboard(KeyboardConfig &config, std::u16string initialText) {
        auto buffer{env->NewDirectByteBuffer(&config, sizeof(KeyboardConfig))};
        auto str{env->NewString(reinterpret_cast<const jchar *>(initialText.data()), static_cast<int>(initialText.length()))};
        jobject localInlineView{env->CallStaticObjectMethod(inlineKeyboardClass, showInlineKeyboardId, instance, buffer, str)};
        env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(str);
        if (!localInlineView)
            return {};

        auto inlineView{env->NewGlobalRef(localInlineView)};
        env->DeleteLocalRef(localInlineView);
        return inlineView;
    }

    JvmManager::KeyboardHandle JvmManager::CloneKeyboardHandle(KeyboardHandle handle) {
        return handle ? env->NewGlobalRef(handle) : nullptr;
    }

    void JvmManager::ReleaseKeyboardHandle(KeyboardHandle handle) {
        if (handle)
            env->DeleteGlobalRef(handle);
    }

    std::pair<JvmManager::KeyboardCloseResult, std::u16string> JvmManager::WaitForSubmitOrCancel(KeyboardHandle keyboardDialog) {
        auto returnArray{reinterpret_cast<jobjectArray>(env->CallObjectMethod(instance, waitForSubmitOrCancelId, keyboardDialog))};
        auto buttonInteger{env->GetObjectArrayElement(returnArray, 0)};
        auto inputJString{reinterpret_cast<jstring>(env->GetObjectArrayElement(returnArray, 1))};
        auto stringChars{env->GetStringChars(inputJString, nullptr)};
        std::u16string input{stringChars, stringChars + env->GetStringLength(inputJString)};
        env->ReleaseStringChars(inputJString, stringChars);
        auto result{static_cast<KeyboardCloseResult>(env->CallIntMethod(buttonInteger, getIntegerValueId))};
        env->DeleteLocalRef(inputJString);
        env->DeleteLocalRef(buttonInteger);
        env->DeleteLocalRef(returnArray);
        return {result, std::move(input)};
    }

    JvmManager::KeyboardUpdate JvmManager::WaitForInlineKeyboardUpdate(KeyboardHandle inlineView) {
        auto returnArray{reinterpret_cast<jobjectArray>(env->CallObjectMethod(inlineView, waitForInlineUpdateId))};
        if (!returnArray)
            return {KeyboardUpdate::Type::Closed, {}, 0};

        auto typeInteger{env->GetObjectArrayElement(returnArray, 0)};
        auto inputJString{reinterpret_cast<jstring>(env->GetObjectArrayElement(returnArray, 1))};
        auto cursorInteger{env->GetObjectArrayElement(returnArray, 2)};

        std::u16string input;
        if (inputJString) {
            auto stringChars{env->GetStringChars(inputJString, nullptr)};
            input.assign(stringChars, stringChars + env->GetStringLength(inputJString));
            env->ReleaseStringChars(inputJString, stringChars);
        }

        KeyboardUpdate update{
            static_cast<KeyboardUpdate::Type>(env->CallIntMethod(typeInteger, getIntegerValueId)),
            std::move(input),
            static_cast<i32>(env->CallIntMethod(cursorInteger, getIntegerValueId)),
        };

        env->DeleteLocalRef(cursorInteger);
        if (inputJString)
            env->DeleteLocalRef(inputJString);
        env->DeleteLocalRef(typeInteger);
        env->DeleteLocalRef(returnArray);
        return update;
    }

    DhcpInfo JvmManager::GetDhcpInfo() {
        jobject dhcpInfo{env->CallObjectMethod(instance, getDhcpInfoId)};
        jclass dhcpInfoClass{env->GetObjectClass(dhcpInfo)};
        jfieldID ipAddressFieldId{env->GetFieldID(dhcpInfoClass, "ipAddress", "I")};
        jfieldID subnetFieldId{env->GetFieldID(dhcpInfoClass, "netmask", "I")};
        jfieldID gatewayFieldId{env->GetFieldID(dhcpInfoClass, "gateway", "I")};
        jfieldID dns1FieldId{env->GetFieldID(dhcpInfoClass, "dns1", "I")};
        jfieldID dns2FieldId{env->GetFieldID(dhcpInfoClass, "dns2", "I")};

        jint ipAddress{env->GetIntField(dhcpInfo, ipAddressFieldId)};
        jint subnet{env->GetIntField(dhcpInfo, subnetFieldId)};
        jint gateway{env->GetIntField(dhcpInfo, gatewayFieldId)};
        jint dns1{env->GetIntField(dhcpInfo, dns1FieldId)};
        jint dns2{env->GetIntField(dhcpInfo, dns2FieldId)};
        env->DeleteLocalRef(dhcpInfoClass);
        env->DeleteLocalRef(dhcpInfo);
        return DhcpInfo{ipAddress, subnet, gateway, dns1, dns2};
    }

    void JvmManager::CloseKeyboard(KeyboardHandle dialog) {
        if (!dialog)
            return;
        env->CallVoidMethod(instance, closeKeyboardId, dialog);
        env->DeleteGlobalRef(dialog);
    }

    void JvmManager::CloseInlineKeyboard(KeyboardHandle inlineView) {
        if (!inlineView)
            return;
        env->CallStaticVoidMethod(inlineKeyboardClass, closeInlineKeyboardId, instance, inlineView);
        env->DeleteGlobalRef(inlineView);
    }

    JvmManager::KeyboardCloseResult JvmManager::ShowValidationResult(KeyboardHandle dialog, KeyboardTextCheckResult checkResult, std::u16string message) {
        auto str{env->NewString(reinterpret_cast<const jchar *>(message.data()), static_cast<int>(message.length()))};
        auto result{static_cast<KeyboardCloseResult>(env->CallIntMethod(instance, showValidationResultId, dialog, checkResult, str))};
        env->DeleteLocalRef(str);
        return result;
    }

    void JvmManager::reportCrash() {
        env->CallVoidMethod(instance, reportCrashId);
    }

    void JvmManager::ShowPipelineLoadingScreen(u32 totalPipelineCount) {
        env->CallVoidMethod(instance, showPipelineLoadingScreenId, static_cast<jint>(totalPipelineCount));
    }

    void JvmManager::UpdatePipelineLoadingProgress(u32 progress) {
        env->CallVoidMethod(instance, updatePipelineLoadingProgressId, static_cast<jint>(progress));
    }

    void JvmManager::HidePipelineLoadingScreen() {
        env->CallVoidMethod(instance, hidePipelineLoadingScreenId);
    }

    void JvmManager::UpdateShaderCompilationState(bool compiling) {
        env->CallStaticVoidMethod(shaderCompilationNotifierClass, updateShaderCompilationStateId, instance, static_cast<jboolean>(compiling));
    }

    i32 JvmManager::GetVersionCode() {
        return env->CallIntMethod(instance, getVersionCodeId);
    }
}
