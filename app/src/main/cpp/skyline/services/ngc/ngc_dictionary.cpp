// SPDX-License-Identifier: MIT
// Dictionary format and filtering semantics derived from Ryujinx (MIT),
// Copyright (c) Ryujinx Team and Contributors. See ngc_dictionary.LICENSE.
#include "ngc_dictionary.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <zlib.h>

namespace skyline::service::ngc::dictionary {
namespace {
    constexpr std::size_t MaxData = 64 * 1024 * 1024;
    struct Failure { Error error; };
    struct Reader {
        std::vector<std::uint8_t> bytes;
        std::size_t position{};
        std::uint64_t Number(unsigned size) {
            if (size > bytes.size() - position) throw Failure{Error::Malformed};
            std::uint64_t result{};
            for (unsigned i{}; i < size; ++i) result |= std::uint64_t{bytes[position++]} << (i * 8);
            return result;
        }
        void Skip(std::size_t size) {
            if (size > bytes.size() - position) throw Failure{Error::Malformed};
            position += size;
        }
        std::vector<std::uint32_t> Array() {
            auto count = Number(4);
            if (count > (bytes.size() - position) / 4) throw Failure{Error::Malformed};
            std::vector<std::uint32_t> values(count);
            for (auto &value : values) value = Number(4);
            return values;
        }
    };
    struct Bits {
        std::uint32_t length;
        std::vector<std::uint32_t> words;
        bool Has(std::size_t bit) const { return bit < length && ((words[bit / 32] >> (bit % 32)) & 1); }
    };
    Bits ReadBits(Reader &r) {
        auto length = r.Number(4);
        if (length > MaxData * 8 || (length + 31) / 32 > (r.bytes.size() - r.position) / 4) throw Failure{Error::Malformed};
        Bits bits{static_cast<std::uint32_t>(length), {}};
        bits.words.resize((length + 31) / 32);
        for (auto &word : bits.words) word = r.Number(4);
        return bits;
    }
    void SkipRank(Reader &r, std::uint32_t length) {
        if (length) r.Skip((std::size_t{length} / 256 + 1) * 4 + length / 32 + 1);
    }
    Bits ReadSet(Reader &r) { auto bits = ReadBits(r); SkipRank(r, bits.length); return bits; }
    void SkipSelect(Reader &r) {
        auto count = r.Number(4); r.Skip(count * 4);
        auto a = ReadBits(r); auto b = ReadBits(r);
        SkipRank(r, a.length); SkipRank(r, b.length);
    }
    std::uint64_t Packed(const std::vector<std::uint32_t> &words, std::size_t offset, unsigned width) {
        if (!width) return 0;
        if (width > 32 || offset / 32 >= words.size()) throw Failure{Error::Malformed};
        std::uint64_t value = words[offset / 32];
        if (offset % 32 + width > 32) {
            if (offset / 32 + 1 >= words.size()) throw Failure{Error::Malformed};
            value |= std::uint64_t{words[offset / 32 + 1]} << 32;
        }
        return (value >> (offset % 32)) & ((std::uint64_t{1} << width) - 1);
    }
    std::vector<std::int32_t> ReadCompressed(Reader &r) {
        auto ranges = r.Array(); auto fields = r.Array();
        auto count = r.Number(1);
        if (ranges.size() % 2 || count > 64 || ranges.size() > MaxData / 128) throw Failure{Error::Malformed};
        std::vector<std::int32_t> values;
        for (std::size_t i{}; i < ranges.size(); i += 2) {
            unsigned width = (ranges[i] >> 27) + 1;
            auto base = static_cast<std::int32_t>(ranges[i + 1]);
            auto start = std::size_t{ranges[i] & 0x7ffffff} * 32;
            for (unsigned j{}; j < 64; ++j) {
                auto value = Packed(fields, start + j * width, width);
                std::int64_t signedValue = value;
                if (value & (std::uint64_t{1} << (width - 1))) signedValue -= std::uint64_t{1} << width;
                auto decoded = signedValue + base;
                if (decoded < INT32_MIN || decoded > INT32_MAX) throw Failure{Error::Malformed};
                values.push_back(decoded);
            }
        }
        for (std::size_t i{}; i < count; ++i) values.push_back(static_cast<std::int32_t>(r.Number(4)));
        return values;
    }
    std::vector<std::uint64_t> ReadSparse(Reader &r) {
        auto range = r.Number(8); auto first = r.Number(8); auto last = r.Number(8);
        auto count = r.Number(4); auto width = r.Number(4); auto fields = r.Array();
        auto bits = ReadSet(r); SkipSelect(r);
        if (count > MaxData / 8 || width > 32) throw Failure{Error::Malformed};
        std::vector<std::uint64_t> values;
        values.reserve(count);
        for (std::size_t bit{}; bit < bits.length; ++bit) {
            if (!bits.Has(bit)) continue;
            if (values.size() >= count || bit < values.size()) throw Failure{Error::Malformed};
            auto high = std::uint64_t{bit - values.size()};
            if (high > (UINT64_MAX >> width)) throw Failure{Error::Malformed};
            auto value = (high << width) + Packed(fields, values.size() * width, width);
            if (!values.empty() && value <= values.back()) throw Failure{Error::Malformed};
            values.push_back(value);
        }
        if (values.size() != count || (!values.empty() && (values.front() != first || values.back() != last || range <= last))) throw Failure{Error::Malformed};
        return values;
    }
    Bits ReadBp(Reader &r, unsigned depth = 0) {
        if (depth > 32) throw Failure{Error::Malformed};
        auto bits = ReadSet(r);
        if (r.Number(1)) { ReadSparse(r); ReadBp(r, depth + 1); }
        return bits;
    }
    struct Trie {
        std::uint32_t stride{};
        std::vector<std::uint64_t> edges;
        std::vector<std::uint32_t> failure;
        std::vector<std::int32_t> lengths, suffix;
        explicit Trie(Reader r) {
            auto wordLengths = ReadCompressed(r); auto wordMap = ReadSparse(r);
            auto multiIndices = ReadCompressed(r); auto multiMap = ReadSparse(r);
            stride = r.Number(4); edges = ReadSparse(r);
            auto bp = ReadBp(r); SkipSelect(r);
            if (!stride || edges.size() >= stride || wordLengths.size() != wordMap.size() || multiIndices.size() != multiMap.size()) throw Failure{Error::Malformed};
            failure.resize(edges.size() + 1); lengths.assign(failure.size(), 0); suffix.assign(failure.size(), -1);
            std::vector<std::uint32_t> stack;
            std::size_t node{};
            for (std::size_t bit{}; bit < bp.length; ++bit) {
                if (bp.Has(bit)) {
                    if (node >= failure.size() || (node != 0 && stack.empty())) throw Failure{Error::Malformed};
                    failure[node] = stack.empty() ? 0 : stack.back();
                    stack.push_back(node++);
                } else { if (stack.empty()) throw Failure{Error::Malformed}; stack.pop_back(); }
            }
            if (!stack.empty() || node != failure.size()) throw Failure{Error::Malformed};
            for (auto edge : edges) if (edge / stride > 255 || edge % stride >= failure.size()) throw Failure{Error::Malformed};
            for (std::size_t i{}; i < wordMap.size(); ++i) {
                if (!wordMap[i] || wordMap[i] >= lengths.size() || wordLengths[i] <= 0 || wordLengths[i] > 0x800) throw Failure{Error::Malformed};
                lengths[wordMap[i]] = wordLengths[i];
            }
            for (std::size_t i{}; i < multiMap.size(); ++i) {
                if (!multiMap[i] || multiMap[i] >= suffix.size() || multiIndices[i] < 0 || std::size_t(multiIndices[i]) >= suffix.size()) throw Failure{Error::Malformed};
                suffix[multiMap[i]] = multiIndices[i];
            }
        }
        template<typename Callback> void Match(std::string_view text, Callback callback) const {
            std::uint32_t node{};
            for (std::size_t i{}; i < text.size(); ++i) {
                for (;;) {
                    auto edge = std::uint64_t{stride} * static_cast<unsigned char>(text[i]) + node;
                    auto found = std::lower_bound(edges.begin(), edges.end(), edge);
                    if (edge && found != edges.end() && *found == edge) {
                        node = static_cast<std::uint32_t>(found - edges.begin() + 1);
                        auto match = node;
                        for (std::size_t hops{};; ++hops) {
                            if (hops > suffix.size()) throw Failure{Error::Malformed};
                            if (lengths[match]) {
                                if (std::size_t(lengths[match]) > i + 1) throw Failure{Error::Malformed};
                                if (!callback(i + 1 - lengths[match], i + 1)) return;
                            }
                            if (suffix[match] < 0) break;
                            match = suffix[match];
                        }
                        break;
                    }
                    if (!node) break;
                    node = failure[node];
                }
            }
        }
        bool Contains(std::string_view text) const { bool found{}; Match(text, [&](auto, auto) { found = true; return false; }); return found; }
    };
    Trie Load(const FileReader &read, const std::string &path) {
        auto input = read(path);
        if (!input) throw Failure{Error::Unavailable};
        if (input->empty() || input->size() > MaxData) throw Failure{Error::Malformed};
        z_stream stream{};
        stream.next_in = input->data(); stream.avail_in = input->size();
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) throw Failure{Error::Malformed};
        std::vector<std::uint8_t> decoded;
        std::array<std::uint8_t, 16384> block{};
        int status;
        do {
            stream.next_out = block.data(); stream.avail_out = block.size();
            status = inflate(&stream, Z_NO_FLUSH);
            decoded.insert(decoded.end(), block.begin(), block.end() - stream.avail_out);
            if (decoded.size() > MaxData) { inflateEnd(&stream); throw Failure{Error::Malformed}; }
        } while (status == Z_OK);
        bool valid = status == Z_STREAM_END && stream.avail_in == 0;
        inflateEnd(&stream);
        if (!valid) throw Failure{Error::Malformed};
        return Trie{Reader{std::move(decoded)}};
    }
    struct Dictionaries {
        Trie partial, complete, delimited;
        Dictionaries(const FileReader &read, int region) :
            partial{Load(read, "ac_" + (region < 0 ? std::string{"common"} : std::to_string(region)) + "_not_b_nx")},
            complete{Load(read, "ac_" + (region < 0 ? std::string{"common"} : std::to_string(region)) + "_b1_nx")},
            delimited{Load(read, "ac_" + (region < 0 ? std::string{"common"} : std::to_string(region)) + "_b2_nx")} {}
    };
    Error InputError(std::string_view text) {
        bool unicode{};
        for (std::size_t i{}; i < text.size();) {
            auto c = static_cast<unsigned char>(text[i++]);
            if (c < 0x80) continue;
            unicode = true;
            unsigned count; std::uint32_t value, minimum;
            if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 31; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 15; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7; minimum = 0x10000; }
            else return Error::InvalidUtf8;
            while (count--) {
                if (i >= text.size()) return Error::InvalidUtf8;
                c = text[i++]; if ((c & 0xc0) != 0x80) return Error::InvalidUtf8;
                value = value * 64 + (c & 63);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return Error::InvalidUtf8;
        }
        return unicode ? Error::Unsupported : Error::Success;
    }
    std::string Lower(std::string_view input) {
        std::string result{input};
        for (auto &c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return result;
    }
    std::string_view BeforeNull(std::string_view text) { return text.substr(0, text.find('\0')); }
    bool Separator(unsigned char c) {
        return std::string_view{"\r\n\t\v\f .,[]!\"#$%&'()*+/:;<=>?\\@^_`{|}~-"}.find(static_cast<char>(c)) != std::string_view::npos;
    }
    void PreMask(std::string &text, std::size_t start, std::size_t end, std::uint32_t mode) {
        auto length = end - start;
        // The long-span byte/count escape encoding needs independent firmware
        // verification. Do not produce speculative output for those matches.
        if (length >= 255) throw Failure{Error::Unsupported};
        if (length == 1) text[start] = char(0xc1);
        else if (length == 2) {
            text[start] = text[start + 1] = char(mode == 0 ? 0xc1 : 0xc0);
        } else if (length > 2) {
            text[start] = 0; text[start + 1] = static_cast<char>(length);
            text[start + 2] = static_cast<char>(mode == 0 ? length : 1);
            std::fill(text.begin() + start + 3, text.begin() + end, char(0xc1));
        }
    }
    void FinishMasks(std::string &text) {
        for (std::size_t i{}; i < text.size(); ++i) {
            auto c = static_cast<unsigned char>(text[i]);
            if (c == 0xc1) text[i] = '*';
            else if (c == 0xc0 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xc0) {
                text[i++] = '*'; text[i] = 0;
            } else if (c == 0 && i + 2 < text.size()) {
                auto length = static_cast<unsigned char>(text[i + 1]);
                auto count = static_cast<unsigned char>(text[i + 2]);
                if (!length || i + length > text.size()) throw Failure{Error::Malformed};
                for (std::size_t j{}; j < length; ++j) text[i + j] = j < count ? '*' : '\0';
                i += length - 1;
            }
        }
        text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
    }
    std::uint32_t Emails(std::string &text, std::uint32_t mode) {
        enum class Step { Start, User, Domain, End };
        auto alnum = [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        auto valid = [&](unsigned char c) { return alnum(c) || c == '-' || c == '.' || c == '_'; };
        Step step{Step::Start}; std::size_t start{}; std::uint32_t count{};
        for (std::size_t i{}; i < text.size(); ++i) {
            auto c = static_cast<unsigned char>(text[i]);
            switch (step) {
                case Step::Start:
                    if (alnum(c)) { step = Step::User; start = i; } break;
                case Step::User: {
                    bool match{};
                    while (valid(c)) { match = true; if (i + 1 >= text.size()) break; c = text[++i]; }
                    step = match && c == '@' ? Step::Domain : Step::Start;
                    break;
                }
                case Step::Domain: step = alnum(c) ? Step::End : Step::Start; break;
                case Step::End: {
                    auto domain = i;
                    while (i + 1 < text.size() && valid(text[++i])) {}
                    auto addressLast = i - 1; std::size_t last{}; bool found{};
                    while (start < addressLast) {
                        c = text[addressLast];
                        if (alnum(c)) { if (!found) { found = true; last = addressLast; } }
                        else if (found) break;
                        --addressLast;
                    }
                    step = Step::Start;
                    if (domain < addressLast && c == '.') { PreMask(text, start, last + 1, mode); ++count; }
                    else i = domain - 1;
                    break;
                }
            }
        }
        FinishMasks(text);
        return count;
    }
    void MaskRegion(std::string &original, std::string &converted, const Dictionaries &dict, std::uint32_t mode, std::uint32_t &count) {
        auto mask = [&](std::size_t start, std::size_t end) {
            if (start >= end || (start > 0 && converted[start - 1] == 0) || (start > 1 && converted[start - 2] == 0)) return;
            ++count; PreMask(original, start, end, mode); PreMask(converted, start, end, mode);
        };
        auto finish = [&] { FinishMasks(original); FinishMasks(converted); };
        dict.partial.Match(converted, [&](auto start, auto end) { mask(start, end); return true; });
        finish();
        auto separatedPass = [&](bool whole) {
            std::string joined; std::vector<std::size_t> map;
            for (std::size_t i{}; i < converted.size(); ++i) {
                // This strict inequality reproduces RemoveWordSeparators in the
                // reference: the last input byte is retained even if a separator.
                if (i + 1 < converted.size() && Separator(converted[i])) continue;
                joined.push_back(converted[i]); map.push_back(i);
            }
            const auto &trie = whole ? dict.complete : dict.partial;
            trie.Match(joined, [&](auto start, auto end) {
                if (start >= map.size()) return true;
                auto a = map[start]; auto b = end < map.size() ? map[end] : converted.size();
                while (b && Separator(converted[b - 1])) --b;
                if (whole) {
                    bool left = a == 0 || Separator(converted[a - 1]);
                    bool right = b == converted.size() || converted[b] == 0 || Separator(converted[b]);
                    auto candidate = std::string(left ? "\\b" : "a") + joined.substr(start, end - start) + (right ? "\\b" : "a");
                    // The reference uses a 64-byte local buffer including NUL.
                    if (candidate.size() >= 64) throw Failure{Error::Unsupported};
                    if (!dict.delimited.Contains(candidate)) return true;
                }
                mask(a, b); return true;
            });
            finish();
        };
        separatedPass(false); separatedPass(true);
    }

}
Filter::Filter(FileReader reader) : readFile{std::move(reader)} {}
Result Filter::Check(std::string_view word, std::uint32_t regionMask, Options options) const {
    word = BeforeNull(word);
    auto error = InputError(word); if (error != Error::Success) return {error, 0};
    try {
        auto converted = "\\b" + Lower(word) + "\\b";
        std::uint32_t mask{};
        auto check = [&](int region) {
            Dictionaries dict{readFile, region};
            auto bit = region < 0 ? options.systemRegionMask : (1u << region);
            if (word.find('@') != word.npos || dict.partial.Contains(converted) || dict.delimited.Contains(converted)) mask |= bit;
        };
        check(-1);
        if (mask) mask = static_cast<std::uint16_t>(regionMask | options.systemRegionMask);
        for (int i{}; i < 16; ++i) if ((regionMask | options.systemRegionMask) & (1u << i)) check(i);
        return {Error::Success, mask};
    } catch (const Failure &failure) { return {failure.error, 0}; }
}
Result Filter::Mask(std::string &text, std::uint32_t regionMask, Options options) const {
    // Normalization/similarity require Unicode tables and the firmware's
    // similar-form assets. Unsupported is explicit and output remains intact.
    if (options.flags || options.maskMode > 1 || options.skipAtSignCheck > 1) return {Error::Unsupported, 0};
    auto input = BeforeNull(text);
    input = input.substr(0, std::min<std::size_t>(input.size(), 512));
    if (std::any_of(input.begin(), input.end(), [](unsigned char c) { return c >= 0x80; })) return {Error::Unsupported, 0};
    try {
        std::string output{input}; std::uint32_t count{};
        if (options.skipAtSignCheck == 0) count += Emails(output, options.maskMode);
        auto converted = Lower(output);
        Dictionaries common{readFile, -1}; MaskRegion(output, converted, common, options.maskMode, count);
        for (int i{}; i < 16; ++i) if ((regionMask | options.systemRegionMask) & (1u << i)) {
            Dictionaries dict{readFile, i}; MaskRegion(output, converted, dict, options.maskMode, count);
        }
        std::string result{text};
        std::copy(output.begin(), output.end(), result.begin());
        std::fill(result.begin() + output.size(), result.begin() + input.size(), '\0');
        // The reference moves a tail truncated by the 512-character limit up to
        // the compacted prefix. Bytes after a natural NUL keep their offsets.
        if (input.size() == 512 && text.size() > 512 && text[512] != 0) {
            std::copy(text.begin() + 512, text.end(), result.begin() + output.size());
        }
        text = std::move(result);
        return {Error::Success, count};
    } catch (const Failure &failure) { return {failure.error, 0}; }
}
}
