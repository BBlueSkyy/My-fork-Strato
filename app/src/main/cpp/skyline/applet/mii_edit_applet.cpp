// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <services/am/storage/ObjIStorage.h>
#include "mii_edit_applet.h"

namespace skyline::applet {

MiiEditApplet::MiiEditApplet(const DeviceState &state,
                              service::ServiceManager &manager,
                              std::shared_ptr<kernel::type::KEvent> onAppletStateChanged,
                              std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet,
                              std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet,
                              service::applet::LibraryAppletMode appletMode)
    : IApplet{state, manager, std::move(onAppletStateChanged), std::move(onNormalDataPushFromApplet), std::move(onInteractiveDataPushFromApplet), appletMode} {}

MiiEditApplet::CharInfo MiiEditApplet::MakeDefaultMii() {
    CharInfo charInfo{}; // Zero-initialised: valid (if plain-looking) Mii, matches every field default to 0

    // "Player" in UTF-16BE, null-terminated (name field is read as big-endian by the guest)
    constexpr std::array<char, 6> name{'P', 'l', 'a', 'y', 'e', 'r'};
    for (size_t i{}; i < name.size(); i++)
        charInfo.name[i] = static_cast<u16>(static_cast<u16>(name[i]) << 8);

    charInfo.sex = 0; // Male
    charInfo.height = 64;
    charInfo.width = 64;
    charInfo.eyeSize = 4;
    charInfo.eyeThickness = 4;
    charInfo.eyePosY = 12;
    charInfo.eyebrowSize = 4;
    charInfo.eyebrowThickness = 3;
    charInfo.eyebrowPosY = 10;
    charInfo.noseSize = 4;
    charInfo.mouthSize = 4;
    charInfo.mouthThickness = 4;

    return charInfo;
}

Result MiiEditApplet::Start() {
    auto input{PopNormalInput<AppletInput>()};

    LOGD("MiiEditApplet: mode: 0x{:X} (stub, always returns a default Mii)", static_cast<u32>(input.appletMode));

    switch (input.appletMode) {
        case AppletMode::CreateMii:
        case AppletMode::EditMii: {
            AppletOutputForCharInfoEditing output{};
            output.result = 0; // Success
            output.charInfo = MakeDefaultMii();
            PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<AppletOutputForCharInfoEditing>>(state, manager, output));
            break;
        }

        default: {
            AppletOutput output{};
            output.result = 0; // Success
            output.index = 0;
            PushNormalDataAndSignal(std::make_shared<service::am::ObjIStorage<AppletOutput>>(state, manager, output));
            break;
        }
    }

    // Notify the guest that we've finished running
    onAppletStateChanged->Signal();
    return {};
}

Result MiiEditApplet::GetResult() {
    return {};
}

void MiiEditApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {}

void MiiEditApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {}

}
