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

    /*
     * @brief A thread-local wrapper over JNIEnv and JavaVM which automatically handles attaching and detaching threads
     */
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
          openSoftwareKeyboardId{environ->GetMethodID(instanceClass, "openSoftwareKeyboard", "(JLjava/nio/ByteBuffer;Ljava/lang/String;Z)Z")},
          showSoftwareKeyboardTextCheckId{environ->GetMethodID(instanceClass, "showSoftwareKeyboardTextCheck", "(JILjava/lang/String;)V")},
          resumeSoftwareKeyboardId{environ->GetMethodID(instanceClass, "resumeSoftwareKeyboard", "(J)V")},
          updateSoftwareKeyboardId{environ->GetMethodID(instanceClass, "updateSoftwareKeyboard", "(JLjava/lang/String;I)V")},
          hideSoftwareKeyboardId{environ->GetMethodID(instanceClass, "hideSoftwareKeyboard", "(J)V")},
          closeSoftwareKeyboardId{environ->GetMethodID(instanceClass, "closeSoftwareKeyboard", "(J)V")},
          reportCrashId{environ->GetMethodID(instanceClass, "reportCrash", "()V")},
          showPipelineLoadingScreenId{environ->GetMethodID(instanceClass, "showPipelineLoadingScreen", "(I)V")},
          updatePipelineLoadingProgressId{environ->GetMethodID(instanceClass, "updatePipelineLoadingProgress", "(I)V")},
          hidePipelineLoadingScreenId{environ->GetMethodID(instanceClass, "hidePipelineLoadingScreen", "()V")},
          getVersionCodeId{environ->GetMethodID(instanceClass, "getVersionCode", "()I")},
          getDhcpInfoId{environ->GetMethodID(instanceClass, "getDhcpInfo", "()Landroid/net/DhcpInfo;")} {
        env.Initialize(environ);

        auto notifierClass{environ->FindClass("org/stratoemu/strato/ShaderCompilationNotifier")};
        shaderCompilationNotifierClass = reinterpret_cast<jclass>(environ->NewGlobalRef(notifierClass));
        updateShaderCompilationStateId = environ->GetStaticMethodID(shaderCompilationNotifierClass, "update", "(Landroid/app/Activity;Z)V");
        environ->DeleteLocalRef(notifierClass);
    }

    JvmManager::~JvmManager() {
        env->DeleteGlobalRef(shaderCompilationNotifierClass);
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
        env->CallVoidMethod(instance, clearVibrationDeviceId, index);
    }

    namespace {
        jstring NewJString(JNIEnv *environment, std::u16string_view text) {
            return environment->NewString(reinterpret_cast<const jchar *>(text.data()), static_cast<jsize>(text.size()));
        }
    }

    applet::swkbd::FrontendSessionId JvmManager::CreateSoftwareKeyboardSession(
        std::weak_ptr<applet::swkbd::SoftwareKeyboardFrontendCallbacks> callbacks) {
        return softwareKeyboardSessions.Register(std::move(callbacks));
    }

    bool JvmManager::ShowSoftwareKeyboard(applet::swkbd::FrontendSessionId sessionId,
                                          const applet::swkbd::FrontendKeyboardConfig &config,
                                          std::u16string_view initialText, bool inlineKeyboard) {
        auto configCopy{config};
        auto buffer{env->NewDirectByteBuffer(configCopy.data(), configCopy.size())};
        auto text{NewJString(env, initialText)};
        const bool opened{env->CallBooleanMethod(instance, openSoftwareKeyboardId, static_cast<jlong>(sessionId), buffer, text,
                                                 inlineKeyboard ? JNI_TRUE : JNI_FALSE) == JNI_TRUE};
        env->DeleteLocalRef(text);
        env->DeleteLocalRef(buffer);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return false;
        }
        return opened;
    }

    void JvmManager::ShowSoftwareKeyboardTextCheck(applet::swkbd::FrontendSessionId sessionId, u32 result,
                                                   std::u16string_view message) {
        auto text{NewJString(env, message)};
        env->CallVoidMethod(instance, showSoftwareKeyboardTextCheckId, static_cast<jlong>(sessionId),
                            static_cast<jint>(result), text);
        env->DeleteLocalRef(text);
    }

    void JvmManager::ResumeSoftwareKeyboard(applet::swkbd::FrontendSessionId sessionId) {
        env->CallVoidMethod(instance, resumeSoftwareKeyboardId, static_cast<jlong>(sessionId));
    }

    void JvmManager::UpdateSoftwareKeyboard(applet::swkbd::FrontendSessionId sessionId,
                                            std::u16string_view input, i32 cursor) {
        auto text{NewJString(env, input)};
        env->CallVoidMethod(instance, updateSoftwareKeyboardId, static_cast<jlong>(sessionId), text,
                            static_cast<jint>(cursor));
        env->DeleteLocalRef(text);
    }

    void JvmManager::HideSoftwareKeyboard(applet::swkbd::FrontendSessionId sessionId) {
        env->CallVoidMethod(instance, hideSoftwareKeyboardId, static_cast<jlong>(sessionId));
    }

    void JvmManager::CloseSoftwareKeyboardSession(applet::swkbd::FrontendSessionId sessionId) {
        softwareKeyboardSessions.Unregister(sessionId);
        env->CallVoidMethod(instance, closeSoftwareKeyboardId, static_cast<jlong>(sessionId));
    }

    bool JvmManager::DispatchSoftwareKeyboardEvent(applet::swkbd::FrontendEvent event) {
        return softwareKeyboardSessions.Dispatch(std::move(event));
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
        return DhcpInfo{ipAddress, subnet, gateway, dns1, dns2};
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
