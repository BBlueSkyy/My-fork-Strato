// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <jni.h>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <vfs/filesystem.h>

namespace skyline::loader {
    /**
     * A bounded FD cache for Android SAF documents. Paths/sizes are supplied by
     * the trusted DocumentProvider enumeration; files open only when read.
     */
    class JavaExtractedSource : public std::enable_shared_from_this<JavaExtractedSource> {
      public:
        struct File {
            std::string path;
            size_t size;
        };

        explicit JavaExtractedSource(JNIEnv *env, jobject source);
        ~JavaExtractedSource();

        JavaExtractedSource(const JavaExtractedSource &) = delete;
        JavaExtractedSource &operator=(const JavaExtractedSource &) = delete;

        std::vector<File> files;
        std::string title;
        std::shared_ptr<vfs::Backing> OpenFile(const std::string &path);
        size_t Read(size_t index, span<u8> data, size_t offset);

      private:
        jobject sourceObject{};
        jmethodID openFileMethod{};
        std::mutex mutex;
        std::list<std::pair<size_t, int>> recentFiles;
    };

    class ExtractedExeFs final : public vfs::FileSystem {
      public:
        explicit ExtractedExeFs(std::shared_ptr<JavaExtractedSource> source) : source(std::move(source)) {}

      protected:
        std::shared_ptr<vfs::Backing> OpenFileImpl(const std::string &path, vfs::Backing::Mode mode) override;
        std::optional<vfs::Directory::EntryType> GetEntryTypeImpl(const std::string &path) override;

      private:
        std::shared_ptr<JavaExtractedSource> source;
    };
}
