// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common/language.h>
#include "backing.h"

namespace skyline::vfs {
    /**
     * @brief The NACP class provides easy access to the data found in an NACP file
     * @url https://switchbrew.org/wiki/NACP_Format
     */
    class NACP {
      private:
        /**
         * @brief The title data of an application for one language
         */
        struct ApplicationTitle {
            std::array<char, 0x200> applicationName; //!< The name of the application
            std::array<char, 0x100> applicationPublisher; //!< The publisher of the application
        };
        static_assert(sizeof(ApplicationTitle) == 0x300);

      public:
        struct NacpData {
            struct NeighborDetectionGroupConfiguration {
                u64 groupId;
                std::array<u8, 0x10> key;
            };
            static_assert(sizeof(NeighborDetectionGroupConfiguration) == 0x18);

            struct NeighborDetectionClientConfiguration {
                NeighborDetectionGroupConfiguration sendGroupConfiguration;
                std::array<NeighborDetectionGroupConfiguration, 0x10> receivableGroupConfigurations;
            };
            static_assert(sizeof(NeighborDetectionClientConfiguration) == 0x198);

            struct JitConfiguration {
                u64 flags;
                i64 memorySize;
            };
            static_assert(sizeof(JitConfiguration) == 0x10);

            std::array<ApplicationTitle, 0x10> titleEntries; //!< Title entries for each language
            std::array<u8, 0x25> isbn;
            u8 startupUserAccount;
            u8 userAccountSwitchLock;
            u8 addonContentRegistrationType;
            u32 attributeFlag;
            u32 supportedLanguageFlag; //!< A bitmask containing the game's supported languages
            u32 parentalControlFlag;
            u8 screenshotEnabled;
            u8 videoCaptureMode;
            u8 dataLossConfirmation;
            u8 playLogPolicy;
            u64 presenceGroupId;
            std::array<u8, 0x20> ratingAge;
            std::array<char, 0x10> displayVersion; //!< The user-readable version of the application
            u64 addOnContentBaseId;
            u64 saveDataOwnerId; //!< The ID that should be used for this application's savedata
            u64 userAccountSaveDataSize;
            u64 userAccountSaveDataJournalSize;
            u64 deviceSaveDataSize;
            u64 deviceSaveDataJournalSize;
            u64 bcatDeliveryCacheStorageSize;
            char applicationErrorCodeCategory[8];
            std::array<u64, 0x8> localCommunicationId;
            u8 logoType;
            u8 logoHandling;
            u8 runtimeAddOnContentInstall;
            u8 runtimeParameterDelivery;
            std::array<u8, 0x2> reserved30F4;
            u8 crashReport;
            u8 hdcp;
            std::array<u8, 8> seedForPseudoDeviceId; //!< Seed that is combined with the device seed for generating the pseudo-device ID
            std::array<u8, 0x41> bcatPassphrase;
            u8 startupUserAccountOption;
            std::array<u8, 0x6> reservedForUserAccountSaveDataOperation;
            i64 userAccountSaveDataSizeMax;
            i64 userAccountSaveDataJournalSizeMax;
            i64 deviceSaveDataSizeMax;
            i64 deviceSaveDataJournalSizeMax;
            i64 temporaryStorageSize;
            i64 cacheStorageSize;
            i64 cacheStorageJournalSize;
            i64 cacheStorageDataAndJournalSizeMax;
            u16 cacheStorageIndexMax;
            u8 reserved318A;
            u8 runtimeUpgrade;
            u32 supportingLimitedLicenses;
            std::array<u64, 0x10> playLogQueryableApplicationId;
            u8 playLogQueryCapability;
            u8 repairFlag;
            u8 programIndex;
            u8 requiredNetworkServiceLicenseOnLaunchFlag;
            std::array<u8, 0x4> reserved3214;
            NeighborDetectionClientConfiguration neighborDetectionClientConfiguration;
            JitConfiguration jitConfiguration;
            std::array<u16, 0x20> requiredAddOnContentsSetBinaryDescriptors;
            u8 playReportPermission;
            u8 crashScreenshotForProd;
            u8 crashScreenshotForDev;
            u8 contentsAvailabilityTransitionPolicy;
            std::array<u8, 0x4> reserved3404;
            std::array<u64, 0x8> accessibleLaunchRequiredVersion;
            std::array<u8, 0xBB8> reserved3448;
        } nacpContents{};
        static_assert(sizeof(NacpData) == 0x4000);

        u32 supportedTitleLanguages{}; //!< A bitmask containing the available title entry languages and game icons

        NACP(const std::shared_ptr<vfs::Backing> &backing);

        language::ApplicationLanguage GetFirstSupportedTitleLanguage();

        language::ApplicationLanguage GetFirstSupportedLanguage();

        std::string GetApplicationName(language::ApplicationLanguage language);

        std::string GetApplicationVersion();

        std::string GetAddOnContentBaseId();

        std::string GetSaveDataOwnerId();

        std::string GetApplicationPublisher(language::ApplicationLanguage language);
    };
}
