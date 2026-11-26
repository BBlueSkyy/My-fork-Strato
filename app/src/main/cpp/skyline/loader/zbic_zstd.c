// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors
//
// ZBIC uses the Zstandard 1.5.7 frame format with a different magic and
// Binary Interpolative Coding for FSE normalized-count tables.  This
// translation unit builds the pinned upstream Zstandard decompressor and
// replaces only the NCount reader used by decompression.  The Zstandard
// sources remain in the upstream dual BSD/GPL licensed submodule; Strato
// relies on the BSD license option.
//
// Format references:
//   https://switchbrew.org/wiki/22.0.0
//   Atmosphere-NX/Atmosphere loader ZBIC support
//   DarkMatterCore/nxdumptool ZBIC support
//
// The BIC traversal is generated algorithmically rather than copying the
// precomputed table used by other implementations.

#define DEBUGLEVEL 0
#define MEM_MODULE
#undef XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#undef XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_TRACE 0
#define ZSTD_DISABLE_ASM 1

#define ZSTD_DEPS_NEED_MALLOC
#include <common/zstd_deps.h>
#include <zstd.h>

#undef ZSTD_MAGICNUMBER
#define ZSTD_MAGICNUMBER 0x4349425A

#include <common/debug.c>
#include <common/entropy_common.c>
#include <common/error_private.c>

typedef struct {
    U32 middle;
    U32 first;
    U32 last;
} ZBIC_BicNode;

static U64 ZBIC_pull(const BYTE *header, size_t *remaining, U64 value) {
    while (*remaining != 0) {
        if ((value >> 32) >= 0x100)
            break;

        --(*remaining);
        value = (U64)header[*remaining + 1] | (value << 8);
    }

    return value;
}

static void ZBIC_buildTraversal(U32 first, U32 last, ZBIC_BicNode *nodes, size_t *count) {
    if (last - first <= 1)
        return;

    const U32 middle = (first + last) / 2;
    nodes[(*count)++] = (ZBIC_BicNode){middle, first, last};

    /* Nintendo's traversal visits the right subtree first. */
    ZBIC_buildTraversal(middle, last, nodes, count);
    ZBIC_buildTraversal(first, middle, nodes, count);
}

static size_t ZBIC_readNCount(short *normalizedCounter, unsigned *maxSVPtr, unsigned *tableLogPtr,
                              const void *headerBuffer, size_t hbSize) {
    const BYTE *header = (const BYTE *)headerBuffer;
    if (hbSize == 0)
        return ERROR(srcSize_wrong);

    const U32 rawDataSize = header[0] & 0x7F;
    const U32 useLowProb = header[0] >> 7;
    if (rawDataSize >= hbSize)
        return ERROR(srcSize_wrong);

    const size_t dataSize = (size_t)rawDataSize + 1;
    size_t remainingData = rawDataSize;
    const unsigned maxSymbolValue = *maxSVPtr;

    U64 value = ZBIC_pull(header, &remainingData, 0);
    U64 encodedCharTable = value / 0x34;
    const U32 charNum = (U32)(value % 0x34) + 1;
    encodedCharTable = ZBIC_pull(header, &remainingData, encodedCharTable);

    if (charNum > maxSymbolValue)
        return ERROR(maxSymbolValue_tooSmall);

    U64 charTable = encodedCharTable >> 3;
    const U32 tableLog = (U32)(encodedCharTable & 7) + FSE_MIN_TABLELOG;
    if (tableLog > FSE_TABLELOG_ABSOLUTE_MAX)
        return ERROR(tableLog_tooLarge);

    charTable = ZBIC_pull(header, &remainingData, charTable);

    const int tableSize = 1 << tableLog;
    U64 accumulator = charTable / (U64)tableSize;
    accumulator = ZBIC_pull(header, &remainingData, accumulator);

    U64 charLast = (charTable % (U64)tableSize) + 1;
    if (useLowProb)
        charLast = charNum + (charTable % (U64)tableSize) + 2;

    U32 nextPow2 = charNum;
    nextPow2 |= nextPow2 >> 1;
    nextPow2 |= nextPow2 >> 2;
    nextPow2 |= nextPow2 >> 4;
    nextPow2 |= nextPow2 >> 8;
    nextPow2 |= nextPow2 >> 16;
    ++nextPow2;

    if (nextPow2 > 0xFF)
        return ERROR(corruption_detected);

    U64 counters[257] = {0};
    counters[nextPow2] = charLast;

    if (nextPow2 != 0xFF) {
        ZBIC_BicNode nodes[256];
        size_t nodeCount = 0;
        ZBIC_buildTraversal(0, nextPow2, nodes, &nodeCount);
        nodes[nodeCount++] = (ZBIC_BicNode){0, 0, 1};

        if (nodeCount != nextPow2)
            return ERROR(corruption_detected);

        for (size_t n = 0; n < nodeCount; ++n) {
            const ZBIC_BicNode node = nodes[n];
            const U64 first = counters[node.first];
            const U64 last = counters[node.last];

            if (first == last) {
                for (U32 index = node.first + 1; index < node.last; ++index)
                    counters[index] = first;
                continue;
            }

            if (last < first)
                return ERROR(corruption_detected);

            const U64 denominator = last - first + 1;
            const U64 entry = accumulator % denominator;
            accumulator /= denominator;
            accumulator = ZBIC_pull(header, &remainingData, accumulator);
            counters[node.middle] = entry + first;
        }
    }

    ZSTD_memset(normalizedCounter, 0, (maxSymbolValue + 1) * sizeof(*normalizedCounter));

    if (charNum != 0xFF) {
        int previous = 0;
        int remaining = tableSize;

        for (U32 symbol = 0; symbol <= charNum; ++symbol) {
            const short current = (short)(counters[symbol + 1] & 0xFFFF);
            const int distance = (int)current - previous;
            const int count = distance - (int)useLowProb;
            previous += distance;

            normalizedCounter[symbol] = (short)count;
            remaining -= count < 0 ? -count : count;
        }

        if (remaining != 0)
            return ERROR(corruption_detected);
    }

    if (remainingData != 0)
        return ERROR(corruption_detected);

    *maxSVPtr = charNum;
    *tableLogPtr = tableLog;
    return dataSize;
}

static size_t ZBIC_readNCount_bmi2(short *normalizedCounter, unsigned *maxSVPtr, unsigned *tableLogPtr,
                                   const void *headerBuffer, size_t hbSize, int bmi2) {
    (void)bmi2;
    return ZBIC_readNCount(normalizedCounter, maxSVPtr, tableLogPtr, headerBuffer, hbSize);
}

/*
 * Huffman weights use FSE_decompress_wksp(), so redirect its NCount reader.
 */
#define FSE_readNCount_bmi2 ZBIC_readNCount_bmi2
#include <common/fse_decompress.c>
#undef FSE_readNCount_bmi2

#include <common/zstd_common.c>
#include <decompress/huf_decompress.c>
#include <decompress/zstd_ddict.c>
#include <decompress/zstd_decompress.c>

/*
 * Sequence tables call FSE_readNCount() directly.
 */
#define FSE_readNCount ZBIC_readNCount
#include <decompress/zstd_decompress_block.c>
#undef FSE_readNCount
