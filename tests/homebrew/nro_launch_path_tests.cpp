// SPDX-License-Identifier: MPL-2.0
#include <loader/nro_launch_path.h>
#include <iostream>
#include <stdexcept>
#include <string_view>

using skyline::loader::homebrew::ResolveGuestNroPath;

static void Check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}

int main() {
    constexpr std::string_view root{"/storage/emulated/0/Android/data/org.stratoemu.strato/files"};
    constexpr std::string_view homebrew{
        "/storage/emulated/0/Android/data/org.stratoemu.strato/files/switch/sdmc/switch/supermarioworld/smw.nro"
    };

    const auto resolved{ResolveGuestNroPath(homebrew, root)};
    Check(resolved && *resolved == "sdmc:/switch/supermarioworld/smw.nro",
          "installed homebrew failed to receive its SDMC launch path");
    Check(ResolveGuestNroPath(homebrew, std::string(root) + "/") == resolved,
          "app data directory with a trailing slash is not recognized");

    const auto withSpaces{ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc/switch/My Homebrew/main.nro", root)};
    Check(withSpaces && *withSpaces == "sdmc:/switch/My Homebrew/main.nro",
          "valid homebrew path with spaces failed");

    Check(!ResolveGuestNroPath(
        "/storage/emulated/0/Nswitch/supermarioworld/smw.nro", root),
        "outside SAF NRO incorrectly received an SDMC argv path");
    Check(!ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc-extra/switch/main.nro", root),
        "SDMC prefix collision accepted");
    Check(!ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc/switch/../main.nro", root),
        "parent traversal was accepted");
    Check(!ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc/switch/./main.nro", root),
        "dot component was accepted");
    Check(!ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc/switch//main.nro", root),
        "empty directory component was accepted");
    Check(!ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc/switch/\"foo\".nro", root),
        "unrepresentable libnx quoted argv entry was accepted");
    Check(!ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc/switch/test:foo.nro", root),
        "device separator was accepted");
    Check(!ResolveGuestNroPath(
        std::string(root) + "/switch/sdmc/", root),
        "SD card root was interpreted as an executable path");
    Check(!ResolveGuestNroPath(homebrew, ""),
          "empty application files path was accepted");

    std::cout << "PASS Homebrew ABI SDMC argv path regression tests\n";
    return 0;
}
