#include "skyline/services/ngc/ngc_dictionary.h"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iterator>
using namespace skyline::service::ngc::dictionary;
void Require(bool value, const char *message) { if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); } }
int main(int argc, char **argv) {
    Require(argc == 2, "fixture directory supplied");
    Filter filter([](std::string_view) -> std::optional<std::vector<std::uint8_t>> { return std::nullopt; });
    auto result = filter.Check("ordinary", 1, {});
    Require(result.error == Error::Unavailable, "missing dictionaries cannot report clean text");
    std::string text = "ordinary";
    result = filter.Mask(text, 1, {});
    Require(result.error == Error::Unavailable && text == "ordinary", "missing dictionary mask is transactional");
    result = filter.Check(std::string("\xc0\x80", 2), 1, {});
    Require(result.error == Error::InvalidUtf8, "overlong UTF8 fails before dictionary access");
    text = std::string("\xc0\x80", 2);
    result = filter.Mask(text, 1, {});
    Require(result.error == Error::Unsupported, "permissive nonASCII mask conversion is explicitly unsupported");
    result = filter.Check("\xc3\xa9", 1, {});
    Require(result.error == Error::Unsupported, "Unicode casefold is explicitly unsupported");
    Filter real([&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
        std::ifstream file(std::string(argv[1]) + "/" + std::string(path), std::ios::binary);
        if (!file) return std::nullopt;
        return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{file}, {}};
    });
    Options options{}; options.systemRegionMask = 1; options.skipAtSignCheck = 1;
    result = real.Check("BAD", 1, options);
    Require(result.error == Error::Success && result.value == 1, "real gzip trie matches lowercased partial word");
    result = real.Check("dbad", 1, options);
    Require(result.error == Error::Success && result.value == 1, "AC failure links find suffix matches");
    result = real.Check("WORD", 1, options);
    Require(result.error == Error::Success && result.value == 1, "check uses delimited trie with literal boundary markers");
    result = real.Check("sword", 1, options);
    Require(result.error == Error::Success && result.value == 0, "delimited word does not match inside word");
    result = real.Check("zork", 2, options);
    Require(result.error == Error::Success && result.value == 3, "common match applies to selected region union");
    result = real.Check("token69", 2, options);
    Require(result.error == Error::Success && result.value == 3, "compressed blocks decode signed word length deltas");
    result = real.Check("regional", 2, options);
    Require(result.error == Error::Success && result.value == 2, "regional match reports exact region bit");
    result = real.Check("bad\0regional", 2, options);
    Require(result.error == Error::Success && result.value == 1, "check stops at null terminator");
    text = "bad WORD b-a-d sword";
    result = real.Mask(text, 1, options);
    Require(result.error == Error::Success && result.value == 3 && text == "*** **** ***** sword", "mask applies partial, separator removed, and whole word dictionaries");
    options.maskMode = 1; text = "BAD and b a d";
    result = real.Mask(text, 1, options);
    Require(result.error == Error::Success && result.value == 2 && text.substr(0, text.find('\0')) == "* and *", "single character mask compacts all matching ranges");
    options.maskMode = 0; options.skipAtSignCheck = 0; text = "mail ab@example.com!";
    result = real.Mask(text, 1, options);
    Require(result.error == Error::Success && result.value == 1 && text == "mail **************!", "email masking uses firmware state machine");
    options.skipAtSignCheck = 1;
    text = std::string("bad\0TAIL", 8);
    result = real.Mask(text, 1, options);
    Require(result.error == Error::Success && result.value == 1 && text == std::string("***\0TAIL", 8), "mask preserves bytes after input terminator");
    options.maskMode = 1; text = std::string("bad\0TAIL", 8);
    result = real.Mask(text, 1, options);
    Require(result.error == Error::Success && result.value == 1 && text == std::string("*\0\0\0TAIL", 8), "compaction zeros processed capacity and preserves tail offsets");
    Filter corrupt([](std::string_view) -> std::optional<std::vector<std::uint8_t>> { return std::vector<std::uint8_t>{1,2,3}; });
    result = corrupt.Check("ordinary", 1, options);
    Require(result.error == Error::Malformed, "malformed gzip cannot report clean text");
    options.flags = 1; text = "bad";
    result = real.Mask(text, 1, options);
    Require(result.error == Error::Unsupported && text == "bad", "unsupported normalization cannot partially mask output");
    std::cout << "ngc dictionary tests passed\n";
}
