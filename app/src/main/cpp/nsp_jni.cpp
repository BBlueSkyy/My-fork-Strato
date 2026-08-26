// SPDX-License-Identifier: MPL-2.0
// Copyright � 2025 Strato Team and Contributors (https://github.com/strato-emu/)

#include <jni.h>
#include <string>
#include <vector>
#include <memory>
#include "skyline/loader/nsp.h"
#include "skyline/vfs/os_backing.h"
#include "skyline/crypto/key_store.h"
#include "skyline/common.h"

using namespace skyline;

extern "C" {

/**
 * @brief Parse NSP metadata to determine content type and extract version/name
 */
JNIEXPORT jobject JNICALL
Java_org_stratoemu_strato_loader_NspParser_parseNspMetadata(JNIEnv *env, jclass, jint fd, jstring appFilesPath) {
    const char *appFilesPathCStr = env->GetStringUTFChars(appFilesPath, nullptr);
    std::string keyStorePath = std::string(appFilesPathCStr) + "keys/";
    env->ReleaseStringUTFChars(appFilesPath, appFilesPathCStr);

    try {
        LOGI("NSP JNI: Starting parseNspMetadata");
        auto file = std::make_shared<vfs::OsBacking>(fd);
        auto keyStore = std::make_shared<crypto::KeyStore>(keyStorePath);
        LOGI("NSP JNI: Created backing and keystore");
        auto nspLoader = std::make_shared<loader::NspLoader>(file, keyStore, std::string{}, loader::NspLoadMode::MetadataOnly);
        LOGI("NSP JNI: Created NSP loader");

        // Create result object
        jclass resultClass = env->FindClass("org/stratoemu/strato/loader/NspMetadata");
        jmethodID constructor = env->GetMethodID(resultClass, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");

        std::string name = "";
        std::string version = "";
        std::string titleId = "";
        std::string parentTitleId = "";
        int contentType = 0; // 0=Unknown, 1=Base, 2=Update, 3=DLC

        if (nspLoader->nacp) {
            LOGI("NSP JNI: Found NACP");
            // Essayer d'abord en anglais américain
            name = nspLoader->nacp->GetApplicationName(language::ApplicationLanguage::AmericanEnglish);
            
            // Si le nom est vide ou ne contient que des espaces, essayer d'autres langues
            if (name.empty() || std::all_of(name.begin(), name.end(), ::isspace)) {
                LOGI("NSP JNI: English name is empty, trying other languages");
                name = nspLoader->nacp->GetApplicationName(nspLoader->nacp->GetFirstSupportedTitleLanguage());
            }
            
            // Si toujours vide, utiliser le title ID comme fallback
            if ((name.empty() || std::all_of(name.begin(), name.end(), ::isspace)) && nspLoader->cnmt) {
                LOGI("NSP JNI: No valid name found in NACP, using title ID as fallback");
                name = "DLC " + nspLoader->cnmt->GetTitleId();
            }
            
            version = nspLoader->nacp->GetApplicationVersion();
            LOGI("NSP JNI: Name='%s', Version='%s' (length: %zu)", name.c_str(), version.c_str(), name.length());
        } else if (nspLoader->cnmt) {
            // Fallback si pas de NACP mais CNMT présent
            LOGI("NSP JNI: No NACP found, using title ID as name");
            name = "DLC " + nspLoader->cnmt->GetTitleId();
        } else {
            LOGI("NSP JNI: No NACP or CNMT found, using default name");
            name = "DLC Content";
        }

        if (nspLoader->cnmt) {
            LOGI("NSP JNI: Found CNMT");
            titleId = nspLoader->cnmt->GetTitleId();
            
            // Determine content type based on CNMT
            auto cnmtType = nspLoader->cnmt->GetContentMetaType();
            switch (cnmtType) {
                case vfs::ContentMetaType::Application:
                    contentType = 1; // Base game
                    break;
                case vfs::ContentMetaType::Patch:
                    contentType = 2; // Update
                    parentTitleId = nspLoader->cnmt->GetParentTitleId();
                    break;
                case vfs::ContentMetaType::AddOnContent:
                    contentType = 3; // DLC
                    parentTitleId = nspLoader->cnmt->GetParentTitleId();
                    break;
                default:
                    contentType = 0; // Unknown
                    break;
            }
            LOGI("NSP JNI: TitleId=%s, ParentTitleId=%s, ContentType=%d", titleId.c_str(), parentTitleId.c_str(), contentType);
        } else {
            LOGI("NSP JNI: No CNMT found");
        }

        LOGI("NSP JNI: Creating result object");
        jstring jName = env->NewStringUTF(name.c_str());
        jstring jVersion = env->NewStringUTF(version.c_str());
        jstring jTitleId = env->NewStringUTF(titleId.c_str());
        jstring jParentTitleId = env->NewStringUTF(parentTitleId.c_str());

        jobject result = env->NewObject(resultClass, constructor, jName, jVersion, jTitleId, jParentTitleId, contentType);
        LOGI("NSP JNI: parseNspMetadata completed successfully");
        return result;

    } catch (const std::exception &e) {
        LOGE("NSP JNI: Exception in parseNspMetadata: %s", e.what());
        return nullptr;
    } catch (...) {
        LOGE("NSP JNI: Unknown exception in parseNspMetadata");
        return nullptr;
    }
}

/**
 * @brief Check if an NSP file is a valid update for a given base game
 */
JNIEXPORT jboolean JNICALL
Java_org_stratoemu_strato_loader_NspParser_isValidUpdate(JNIEnv *env, jclass, jint updateFd, jstring baseTitleId, jstring appFilesPath) {
    const char *baseTitleIdCStr = env->GetStringUTFChars(baseTitleId, nullptr);
    const char *appFilesPathCStr = env->GetStringUTFChars(appFilesPath, nullptr);
    
    std::string baseTitleIdStr = baseTitleIdCStr;
    std::string keyStorePath = std::string(appFilesPathCStr) + "keys/";
    
    env->ReleaseStringUTFChars(baseTitleId, baseTitleIdCStr);
    env->ReleaseStringUTFChars(appFilesPath, appFilesPathCStr);

    try {
        auto file = std::make_shared<vfs::OsBacking>(updateFd);
        auto keyStore = std::make_shared<crypto::KeyStore>(keyStorePath);
        auto nspLoader = std::make_shared<loader::NspLoader>(file, keyStore, std::string{}, loader::NspLoadMode::MetadataOnly);

        if (!nspLoader->cnmt) {
            return JNI_FALSE;
        }

        // Check if it's a patch (update) and matches the base title ID
        auto cnmtType = nspLoader->cnmt->GetContentMetaType();
        if (cnmtType == vfs::ContentMetaType::Patch) {
            std::string parentTitleId = nspLoader->cnmt->GetParentTitleId();
            return parentTitleId == baseTitleIdStr ? JNI_TRUE : JNI_FALSE;
        }

        return JNI_FALSE;
    } catch (const std::exception &e) {
        return JNI_FALSE;
    }
}

/**
 * @brief Check if an NSP file is a valid DLC for a given base game
 */
JNIEXPORT jboolean JNICALL
Java_org_stratoemu_strato_loader_NspParser_isValidDlc(JNIEnv *env, jclass, jint dlcFd, jstring baseTitleId, jstring appFilesPath) {
    const char *baseTitleIdCStr = env->GetStringUTFChars(baseTitleId, nullptr);
    const char *appFilesPathCStr = env->GetStringUTFChars(appFilesPath, nullptr);
    
    std::string baseTitleIdStr = baseTitleIdCStr;
    std::string keyStorePath = std::string(appFilesPathCStr) + "keys/";
    
    env->ReleaseStringUTFChars(baseTitleId, baseTitleIdCStr);
    env->ReleaseStringUTFChars(appFilesPath, appFilesPathCStr);

    try {
        auto file = std::make_shared<vfs::OsBacking>(dlcFd);
        auto keyStore = std::make_shared<crypto::KeyStore>(keyStorePath);
        auto nspLoader = std::make_shared<loader::NspLoader>(file, keyStore, std::string{}, loader::NspLoadMode::MetadataOnly);

        if (!nspLoader->cnmt) {
            return JNI_FALSE;
        }

        // Check if it's DLC (AddOnContent) and matches the base title ID
        auto cnmtType = nspLoader->cnmt->GetContentMetaType();
        if (cnmtType == vfs::ContentMetaType::AddOnContent) {
            std::string parentTitleId = nspLoader->cnmt->GetParentTitleId();
            return parentTitleId == baseTitleIdStr ? JNI_TRUE : JNI_FALSE;
        }

        return JNI_FALSE;
    } catch (const std::exception &e) {
        return JNI_FALSE;
    }
}

/**
 * @brief Get DLC name from NSP file
 */
JNIEXPORT jstring JNICALL
Java_org_stratoemu_strato_loader_NspParser_getDlcName(JNIEnv *env, jclass, jint dlcFd, jstring appFilesPath) {
    const char *appFilesPathCStr = env->GetStringUTFChars(appFilesPath, nullptr);
    std::string keyStorePath = std::string(appFilesPathCStr) + "keys/";
    env->ReleaseStringUTFChars(appFilesPath, appFilesPathCStr);

    try {
        auto file = std::make_shared<vfs::OsBacking>(dlcFd);
        auto keyStore = std::make_shared<crypto::KeyStore>(keyStorePath);
        auto nspLoader = std::make_shared<loader::NspLoader>(file, keyStore, std::string{}, loader::NspLoadMode::MetadataOnly);

        if (nspLoader->nacp) {
            std::string name = nspLoader->nacp->GetApplicationName(language::ApplicationLanguage::AmericanEnglish);
            if (name.empty()) {
                name = nspLoader->nacp->GetApplicationName(nspLoader->nacp->GetFirstSupportedTitleLanguage());
            }
            return env->NewStringUTF(name.c_str());
        }

        // Fallback: use title ID if no name available
        if (nspLoader->cnmt) {
            std::string titleId = "DLC " + nspLoader->cnmt->GetTitleId();
            return env->NewStringUTF(titleId.c_str());
        }

        return env->NewStringUTF("Unknown DLC");
    } catch (const std::exception &e) {
        return env->NewStringUTF("Error reading DLC");
    }
}

/**
 * @brief Get update version from NSP file
 */
JNIEXPORT jstring JNICALL
Java_org_stratoemu_strato_loader_NspParser_getUpdateVersion(JNIEnv *env, jclass, jint updateFd, jstring appFilesPath) {
    const char *appFilesPathCStr = env->GetStringUTFChars(appFilesPath, nullptr);
    std::string keyStorePath = std::string(appFilesPathCStr) + "keys/";
    env->ReleaseStringUTFChars(appFilesPath, appFilesPathCStr);

    try {
        auto file = std::make_shared<vfs::OsBacking>(updateFd);
        auto keyStore = std::make_shared<crypto::KeyStore>(keyStorePath);
        auto nspLoader = std::make_shared<loader::NspLoader>(file, keyStore, std::string{}, loader::NspLoadMode::MetadataOnly);

        if (nspLoader->nacp) {
            std::string version = nspLoader->nacp->GetApplicationVersion();
            return env->NewStringUTF(version.c_str());
        }

        return env->NewStringUTF("Unknown Version");
    } catch (const std::exception &e) {
        return env->NewStringUTF("Error reading version");
    }
}

}
