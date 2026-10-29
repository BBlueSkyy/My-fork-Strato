// SPDX-License-Identifier: MPL-2.0
#include <crypto/key_store.h>
#include <fstream>
#include <iostream>
#include <unistd.h>

using skyline::crypto::KeyStore;

namespace {
    void Check(bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    // Only generated, repeated-byte synthetic keys are written to these fixtures.
    std::string Hex(char digit, size_t bytes = 16) {
        return std::string(bytes * 2, digit);
    }

    KeyStore::Key128 Repeated(skyline::u8 value) {
        KeyStore::Key128 key{};
        key.fill(value);
        return key;
    }

    struct KeyFiles {
        std::filesystem::path root;

        KeyFiles() {
            auto pattern{(std::filesystem::temp_directory_path() / "strato-key-tests-XXXXXX").string()};
            const auto path{mkdtemp(pattern.data())};
            if (!path)
                throw std::runtime_error("Cannot create key test directory");
            root = path;
        }

        ~KeyFiles() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const char *name, const std::string &text) const {
            std::ofstream output(root / name, std::ios::binary);
            output << text;
            if (!output)
                throw std::runtime_error("Cannot write synthetic fixture");
        }

        KeyStore Read() const { return KeyStore(root.string()); }
    };

    void LastProductionLineWithoutNewline() {
        KeyFiles files;
        files.Write("prod.keys", "titlekek_15=" + Hex('1'));
        Check(files.Read().titleKek[0x15] == Repeated(0x11), "Final production-key record was lost");
    }

    void LastTitleLineWithoutNewline() {
        KeyFiles files;
        files.Write("title.keys", Hex('2') + "=" + Hex('3'));
        Check(files.Read().GetTitleKey(Repeated(0x22)) == Repeated(0x33), "Final title-key record was lost");
    }

    void WhitespaceAndCrLf() {
        KeyFiles files;
        files.Write("prod.keys", " \ttitlekek_15 \t= \t" + Hex('4') + " \t\r\n");
        files.Write("title.keys", " \t" + Hex('5') + " \t= \t" + Hex('6') + " \t\r\n");
        const auto store{files.Read()};
        Check(store.titleKek[0x15] == Repeated(0x44), "Whitespace hid production key");
        // GetTitleKey is non-const in production.
        auto titleStore{files.Read()};
        Check(titleStore.GetTitleKey(Repeated(0x55)) == Repeated(0x66), "Whitespace hid title key");
    }

    void BlankAndCommentLines() {
        KeyFiles files;
        files.Write("prod.keys", "\n \t\r\n# synthetic comment\n  ; synthetic comment\n" "titlekek_15=" + Hex('7') + "\n\n# final comment");
        files.Write("title.keys", "\n# synthetic comment\n; synthetic comment\n" + Hex('8') + "=" + Hex('9') + "\n\n");
        auto store{files.Read()};
        Check(store.titleKek[0x15] == Repeated(0x77), "Comment records hid production key");
        Check(store.GetTitleKey(Repeated(0x88)) == Repeated(0x99), "Comment records hid title key");
    }

    void EmptyAndMissingFiles() {
        KeyFiles files;
        Check(!files.Read().headerKey, "Missing files populated a header key");
        files.Write("prod.keys", "");
        files.Write("title.keys", "");
        Check(!files.Read().headerKey, "Empty files populated a header key");
    }

    void IndexedRevisionBounds() {
        KeyFiles files;
        files.Write("prod.keys", "titlekek_15=" + Hex('a') + "\n"
            "key_area_key_application_1F=" + Hex('b') + "\n"
            "key_area_key_ocean_20=" + Hex('c') + "\n"
            "key_area_key_system_ff=" + Hex('d') + "\n");
        const auto store{files.Read()};
        Check(store.titleKek[0x15] == Repeated(0xaa), "Modern revision was not loaded");
        Check(store.areaKeyApplication.back() == Repeated(0xbb), "Last supported revision was not loaded");
        for (const auto &key : store.areaKeyOcean)
            Check(!key, "Out-of-range revision populated an ocean key");
        for (const auto &key : store.areaKeySystem)
            Check(!key, "Out-of-range revision populated a system key");
    }

    void MalformedRevisionRejected() {
        for (const auto suffix : {"1g", "+1", "-1", " 1", "gg"}) {
            KeyFiles files;
            files.Write("prod.keys", std::string("titlekek_") + suffix + "=" + Hex('a') + "\n");
            bool threw{};
            try { files.Read(); } catch (const std::exception &) { threw = true; }
            Check(threw, "Malformed revision was accepted");
        }
    }

    void MalformedRecordsRejected() {
        for (const auto &record : {std::string("no_separator\n"), std::string("titlekek_15=\n"), std::string("titlekek_15=") + Hex('x') + "\n"}) {
            KeyFiles files;
            files.Write("prod.keys", record);
            bool threw{};
            try { files.Read(); } catch (const std::exception &) { threw = true; }
            Check(threw, "Malformed record was accepted");
        }
    }
}

int main() {
    struct Test { const char *name; void (*run)(); };
    const Test tests[]{
        {"last production line without newline", LastProductionLineWithoutNewline},
        {"last title line without newline", LastTitleLineWithoutNewline},
        {"whitespace and CRLF", WhitespaceAndCrLf},
        {"blank and comment lines", BlankAndCommentLines},
        {"empty and missing files", EmptyAndMissingFiles},
        {"indexed revision bounds", IndexedRevisionBounds},
        {"malformed revision rejected", MalformedRevisionRejected},
        {"malformed records rejected", MalformedRecordsRejected},
    };
    size_t failed{};
    for (const auto &test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception &) {
            // Never echo key-file input or exception payloads.
            ++failed;
            std::cerr << "FAIL " << test.name << '\n';
        }
    }
    return failed ? 1 : 0;
}
