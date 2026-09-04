// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <deque>
#include <crypto/key_store.h>
#include <common/language.h>
#include "vfs/filesystem.h"
#include "loader/loader.h"
#include "services/serviceman.h"

namespace skyline::kernel {
    /**
     * @brief The OS class manages the interaction between the various Skyline components
     */
    class OS {
      public:
        std::string nativeLibraryPath; //!< The full path to the app's native library directory
        std::string publicAppFilesPath; //!< The full path to the app's public files directory
        std::string privateAppFilesPath; //!< The full path to the app's private files directory
        std::string deviceTimeZone; //!< The timezone name (e.g. Europe/London)
        std::shared_ptr<vfs::FileSystem> assetFileSystem; //!< A filesystem to be used for accessing emulator assets (like tzdata)
        std::shared_ptr<crypto::KeyStore> keyStore;
        std::deque<std::vector<u8>> userChannel;
        DeviceState state;
        service::ServiceManager serviceManager;

      private:
        std::optional<size_t> requestedProgramIndex;

      public:
        /**
         * @param settings An instance of the Settings class
         * @param window The ANativeWindow object to draw the screen to
         */
        OS(
            std::shared_ptr<JvmManager> &jvmManager,
            std::shared_ptr<Settings> &settings,
            std::string publicAppFilesPath,
            std::string privateAppFilesPath,
            std::string deviceTimeZone,
            std::string nativeLibraryPath,
            std::shared_ptr<vfs::FileSystem> assetFileSystem
        );

        /**
         * @brief Execute a particular ROM file
         * @param romFd A FD to the ROM file to execute
         * @param dlcFds An array of FD to the DLC files
         * @param updateFd A FD to the Update file
         * @param romType The type of the ROM file
         */
        void Execute(int romFd, std::vector<int> dlcFds, int updateFd, loader::RomFormat romType, size_t programIndex = 0, i32 previousProgramIndex = -1);

        bool RequestProgramChange(u64 programIndex);

        std::optional<size_t> TakeRequestedProgramIndex();

        std::shared_ptr<loader::Loader> GetLoader(int fd, std::shared_ptr<crypto::KeyStore> keyStore, loader::RomFormat romType, size_t programIndex = 0);
    };
}
