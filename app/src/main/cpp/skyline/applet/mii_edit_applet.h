// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/am/applet/IApplet.h>

namespace skyline::applet {

/**
 * @brief The Mii Edit applet is used by the guest to create/edit/select a Mii, this is a stub
 * implementation which immediately returns a fixed default Mii without showing any UI
 * @url https://switchbrew.org/wiki/MiiEdit_Applet
 */
class MiiEditApplet : public service::am::IApplet, public service::am::EnableNormalQueue {
  private:
#pragma pack(push, 1)

    /**
     * @url https://switchbrew.org/wiki/MiiEdit_Applet#AppletMode
     */
    enum class AppletMode : u32 {
        ShowMiiEdit = 0,
        AppendMii = 1,
        AppendMiiImage = 2,
        UpdateMiiImage = 3,
        CreateMii = 4,
        EditMii = 5,
    };

    /**
     * @brief nn::mii::AppletInput
     * @url https://switchbrew.org/wiki/MiiEdit_Applet#AppletInput
     */
    struct AppletInput {
        u32 version;
        AppletMode appletMode;
        u32 specialMiiKeyCode;
        std::array<u8, 0xF4> unused; //!< Covers ValidUuidArray/CharInfo (EditMii)/UsedUuid/padding, unused by the stub
    };
    static_assert(sizeof(AppletInput) == 0x100);

    /**
     * @brief nn::mii::CharInfo
     * @url https://switchbrew.github.io/libnx/structMiiCharInfo.html
     */
    struct CharInfo {
        std::array<u8, 0x10> createId; //!< nn::util::Uuid
        std::array<u16, 11> name; //!< UTF-16BE, null-terminated
        u8 unk26;
        u8 color;
        u8 sex;
        u8 height;
        u8 width;
        std::array<u8, 2> unk2B;
        u8 faceShape;
        u8 faceColor;
        u8 wrinklesStyle;
        u8 makeupStyle;
        u8 hairStyle;
        u8 hairColor;
        u8 hasHairFlipped;
        u8 eyeStyle;
        u8 eyeColor;
        u8 eyeSize;
        u8 eyeThickness;
        u8 eyeAngle;
        u8 eyePosX;
        u8 eyePosY;
        u8 eyebrowStyle;
        u8 eyebrowColor;
        u8 eyebrowSize;
        u8 eyebrowThickness;
        u8 eyebrowAngle;
        u8 eyebrowPosX;
        u8 eyebrowPosY;
        u8 noseStyle;
        u8 noseSize;
        u8 nosePos;
        u8 mouthStyle;
        u8 mouthColor;
        u8 mouthSize;
        u8 mouthThickness;
        u8 mouthPos;
        u8 facialHairColor;
        u8 beardStyle;
        u8 mustacheStyle;
        u8 mustacheSize;
        u8 mustachePos;
        u8 glassesStyle;
        u8 glassesColor;
        u8 glassesSize;
        u8 glassesPos;
        u8 hasMole;
        u8 moleSize;
        u8 molePosX;
        u8 molePosY;
        u8 unk57;
    };
    static_assert(sizeof(CharInfo) == 0x58);

    /**
     * @brief nn::mii::AppletOutput, used for every AppletMode besides CreateMii/EditMii
     */
    struct AppletOutput {
        u32 result; //!< 0 = Success, 1 = Cancel
        u32 index;
        std::array<u8, 0x18> _pad_;
    };
    static_assert(sizeof(AppletOutput) == 0x20);

    /**
     * @brief nn::mii::AppletOutputForCharInfoEditing, used for AppletMode CreateMii/EditMii
     */
    struct AppletOutputForCharInfoEditing {
        u32 result; //!< 0 = Success, 1 = Cancel
        CharInfo charInfo;
        std::array<u8, 0x24> _pad_;
    };
    static_assert(sizeof(AppletOutputForCharInfoEditing) == 0x80);

#pragma pack(pop)

    /**
     * @brief Builds a fixed, valid default Mii to hand back to the guest
     */
    static CharInfo MakeDefaultMii();

  public:
    MiiEditApplet(const DeviceState &state, service::ServiceManager &manager, std::shared_ptr<kernel::type::KEvent> onAppletStateChanged, std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet, std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet, service::applet::LibraryAppletMode appletMode);

    Result Start() override;

    Result GetResult() override;

    void PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) override;

    void PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
};

}
