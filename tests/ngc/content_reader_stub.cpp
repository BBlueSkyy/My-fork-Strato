// SPDX-License-Identifier: MPL-2.0
// IPC tests inject the real-format fixture reader directly. The independent
// ngc_content suite tests the production provider and mount lifecycle.
#include <services/ngc/content_reader.h>
namespace skyline::service::ngc {
    ContentReader::ContentReader(const DeviceState &) {}
    bool ContentReader::ReadFile(std::string_view, std::vector<u8> &) { return false; }
    bool ContentReader::Reload() { return true; }
}
