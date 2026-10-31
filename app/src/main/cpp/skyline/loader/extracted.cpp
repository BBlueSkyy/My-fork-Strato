// SPDX-License-Identifier: MPL-2.0
#include "extracted.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <sys/types.h>
#include <unistd.h>
#include <common.h>
#include <jvm.h>

namespace skyline::loader {
    namespace {
        class DocumentBacking final : public vfs::Backing {
            std::shared_ptr<JavaExtractedSource> source;
            size_t index;

            size_t ReadImpl(span<u8> data, size_t offset) override {
                return source->Read(index, data, offset);
            }

          public:
            DocumentBacking(std::shared_ptr<JavaExtractedSource> source, size_t index)
                : Backing({true, false, false}, source->files.at(index).size),
                  source(std::move(source)), index(index) {}
        };
    }

    JavaExtractedSource::JavaExtractedSource(JNIEnv *env, jobject object) {
        if (!object)
            throw exception("Missing Android extracted directory provider");
        jclass clazz{env->GetObjectClass(object)};
        const auto pathsMethod{env->GetMethodID(clazz, "getPaths", "()[Ljava/lang/String;")};
        const auto sizesMethod{env->GetMethodID(clazz, "getSizes", "()[J")};
        const auto titleMethod{env->GetMethodID(clazz, "getTitle", "()Ljava/lang/String;")};
        openFileMethod = env->GetMethodID(clazz, "openFile", "(I)I");
        if (!pathsMethod || !sizesMethod || !titleMethod || !openFileMethod)
            throw exception("Missing Android extracted directory methods");

        auto paths{static_cast<jobjectArray>(env->CallObjectMethod(object, pathsMethod))};
        auto sizes{static_cast<jlongArray>(env->CallObjectMethod(object, sizesMethod))};
        auto name{static_cast<jstring>(env->CallObjectMethod(object, titleMethod))};
        if (env->ExceptionCheck() || !paths || !sizes || !name)
            throw exception("Cannot read extracted-game directory manifest");
        const jsize count{env->GetArrayLength(paths)};
        if (count != env->GetArrayLength(sizes) || count > 200000)
            throw exception("Invalid extracted-game manifest length");
        std::vector<jlong> lengths(static_cast<size_t>(count));
        env->GetLongArrayRegion(sizes, 0, count, lengths.data());
        if (env->ExceptionCheck())
            throw exception("Cannot read extracted-game file sizes");

        files.reserve(static_cast<size_t>(count));
        for (jsize i{}; i < count; ++i) {
            auto path{static_cast<jstring>(env->GetObjectArrayElement(paths, i))};
            if (!path || lengths[i] < 0 ||
                static_cast<unsigned long long>(lengths[i]) > std::numeric_limits<size_t>::max())
                throw exception("Invalid file in extracted-game manifest");
            std::string relative{JniString(env, path)};
            env->DeleteLocalRef(path);
            if (relative.empty() || relative.front() == '/' || relative.find("..") != std::string::npos ||
                relative.find('\\') != std::string::npos)
                throw exception("Invalid extracted-game relative path");
            if (!fileIndices.emplace(relative, files.size()).second)
                throw exception("Duplicate extracted-game document path");
            files.emplace_back(File{std::move(relative), static_cast<size_t>(lengths[i])});
        }
        title = JniString(env, name);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(paths);
        env->DeleteLocalRef(sizes);
        env->DeleteLocalRef(clazz);
        sourceObject = env->NewGlobalRef(object);
        if (!sourceObject) throw exception("Cannot retain extracted-game document provider");
    }

    JavaExtractedSource::~JavaExtractedSource() {
        for (const auto &[index, fd] : recentFiles)
            ::close(fd);
        if (sourceObject)
            JvmManager::GetEnv()->DeleteGlobalRef(sourceObject);
    }

    std::shared_ptr<vfs::Backing> JavaExtractedSource::OpenFile(const std::string &path) {
        const auto it{fileIndices.find(path)};
        if (it == fileIndices.end()) return nullptr;
        return std::make_shared<DocumentBacking>(shared_from_this(), it->second);
    }

    size_t JavaExtractedSource::Read(size_t index, span<u8> output, size_t offset) {
        if (index >= files.size() || offset > files[index].size ||
            output.size() > files[index].size - offset)
            throw exception("Extracted-game file read exceeds document extent");
        if (output.empty()) return 0;

        // Serialize only document IO; keep a small LRU to avoid exhausting
        // descriptors when a RomFS contains thousands of separate files.
        std::scoped_lock lock(mutex);
        auto it{std::find_if(recentFiles.begin(), recentFiles.end(), [index](const auto &item) {
            return item.first == index;
        })};
        int fd{};
        if (it != recentFiles.end()) {
            fd = it->second;
            recentFiles.splice(recentFiles.begin(), recentFiles, it);
        } else {
            JNIEnv *env{JvmManager::GetEnv()};
            fd = env->CallIntMethod(sourceObject, openFileMethod, static_cast<jint>(index));
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
                throw exception("Android document provider failed opening extracted file {}", files[index].path);
            }
            if (fd < 0)
                throw exception("Failed to open extracted file {}", files[index].path);
            recentFiles.emplace_front(index, fd);
            constexpr size_t MaxOpenFiles{24};
            if (recentFiles.size() > MaxOpenFiles) {
                ::close(recentFiles.back().second);
                recentFiles.pop_back();
            }
        }

        size_t total{};
        while (total < output.size()) {
            const auto result{::pread64(fd, output.data() + total, output.size() - total,
                                        static_cast<off64_t>(offset + total))};
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                throw exception("Cannot read extracted file {} at offset 0x{:X}", files[index].path, offset + total);
            total += static_cast<size_t>(result);
        }
        return total;
    }

    std::shared_ptr<vfs::Backing> ExtractedExeFs::OpenFileImpl(const std::string &path, vfs::Backing::Mode mode) {
        if (mode.write || mode.append) return nullptr;
        return source->OpenFile("exefs/" + path);
    }

    std::optional<vfs::Directory::EntryType> ExtractedExeFs::GetEntryTypeImpl(const std::string &path) {
        for (const auto &entry : source->files)
            if (entry.path == "exefs/" + path)
                return vfs::Directory::EntryType::File;
        return std::nullopt;
    }
}
