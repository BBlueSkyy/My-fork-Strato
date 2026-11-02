// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <iostream>
#include <fstream>
#include <iterator>
#include <services/ngc/INgcServiceForApplication.h>

using namespace skyline;
using namespace skyline::service;
using namespace skyline::service::ngc;

namespace {
    constexpr Result DataAccessError{146, 102};
    constexpr Result InvalidUtf8{146, 5};
    constexpr Result InvalidArgument{1, 14};

    template<class T> T Value(const kernel::ipc::IpcResponse &response) {
        assert(response.payload.size() == sizeof(T));
        T value;
        std::memcpy(&value, response.payload.data(), sizeof(T));
        return value;
    }

    struct Arguments {
        u32 regionMask{1};
        u32 skipAtSign{1};
        u32 maskMode{};
        u32 flags{};
        u32 systemRegionMask{};
        u32 reserved{};
    };
    static_assert(sizeof(Arguments) == 0x18);

    void Layout(kernel::ipc::IpcRequest &request, const Arguments &args) {
        size_t rawStart{8};
        if (request.header->handleDesc) {
            const auto &h{*request.handleDesc};
            rawStart += 4 + (h.sendPid ? 8 : 0) + (h.copyCount + h.moveCount) * 4;
        }
        rawStart += request.header->xNo * 8 + (request.header->aNo + request.header->bNo + request.header->wNo) * 12;
        const size_t aligned{(rawStart + 15) & ~size_t{15}};
        request.header->rawSize = (32 + sizeof(args) + (request.isDomain ? 16 : 0)) / 4;
        if (request.isDomain) {
            request.domain = reinterpret_cast<kernel::ipc::DomainHeaderRequest *>(request.wire->data() + aligned);
            *request.domain = {};
            request.domain->command = 1;
            request.domain->payloadSz = 16 + sizeof(args);
        }
        request.payload = reinterpret_cast<kernel::ipc::PayloadHeader *>(request.wire->data() + aligned + (request.isDomain ? 16 : 0));
        request.cmdArg = reinterpret_cast<u8 *>(request.payload) + 16;
        request.cmdArgSz = request.isDomain ? sizeof(args) : request.header->rawSize * 4;
        std::memcpy(request.cmdArg, &args, sizeof(args));
    }

    kernel::ipc::IpcRequest Request(Arguments &args, std::vector<u8> &input) {
        kernel::ipc::IpcRequest request;
        request.header->aNo = 1;
        Layout(request, args);
        request.inputBuf.emplace_back(input.data(), input.size());
        return request;
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    DeviceState state;
    ServiceManager manager;
    kernel::type::KSession session;
    // A real data-source callback, with no archive mounted.
    auto missing = [](std::string_view) -> std::optional<std::vector<u8>> { return std::nullopt; };
    INgcServiceForApplication service{state, manager, missing, [] { return true; }};
    kernel::ipc::IpcRequest empty;
    kernel::ipc::IpcResponse version;
    assert(!service.HandleRequest(0, session, empty, version));
    assert(Value<u32>(version) == 0);

    Arguments args;
    std::vector<u8> input{'a', 'b', 'c', 0};
    auto request{Request(args, input)};
    kernel::ipc::IpcResponse check;
    assert(service.HandleRequest(1, session, request, check).raw == DataAccessError.raw);
    assert(check.payload.empty()); // no clean mask fabricated on failure
    // Buffer vectors alone do not establish the required map-alias transport.
    auto pointerOnly{Request(args, input)};
    pointerOnly.header->aNo = 0;
    pointerOnly.header->xNo = 1;
    Layout(pointerOnly, args);
    kernel::ipc::IpcResponse pointerResponse;
    assert(service.HandleRequest(1, session, pointerOnly, pointerResponse).raw == InvalidArgument.raw);
    auto mixedPointer{Request(args, input)};
    mixedPointer.header->xNo = 1;
    Layout(mixedPointer, args);
    kernel::ipc::IpcResponse mixedResponse;
    assert(service.HandleRequest(1, session, mixedPointer, mixedResponse).raw == InvalidArgument.raw);
    auto exchangeBuffer{Request(args, input)};
    exchangeBuffer.header->wNo = 1;
    Layout(exchangeBuffer, args);
    kernel::ipc::IpcResponse exchangeResponse;
    assert(service.HandleRequest(1, session, exchangeBuffer, exchangeResponse).raw == InvalidArgument.raw);
    auto receiveList{Request(args, input)};
    receiveList.header->cFlag = kernel::ipc::BufferCFlag::SingleDescriptor;
    kernel::ipc::IpcResponse receiveResponse;
    assert(service.HandleRequest(1, session, receiveList, receiveResponse).raw == InvalidArgument.raw);
    auto extraAlias{Request(args, input)};
    extraAlias.header->aNo = 2;
    Layout(extraAlias, args);
    kernel::ipc::IpcResponse extraAliasResponse;
    assert(service.HandleRequest(1, session, extraAlias, extraAliasResponse).raw == InvalidArgument.raw);
    std::array<u8, 4> aliasOutput{};
    auto wrongMaskInput{Request(args, input)};
    wrongMaskInput.header->aNo = 0;
    wrongMaskInput.header->xNo = wrongMaskInput.header->bNo = 1;
    Layout(wrongMaskInput, args);
    wrongMaskInput.outputBuf.emplace_back(aliasOutput);
    kernel::ipc::IpcResponse wrongMaskInputResponse;
    assert(service.HandleRequest(2, session, wrongMaskInput, wrongMaskInputResponse).raw == InvalidArgument.raw);
    auto wrongMaskOutput{Request(args, input)};
    wrongMaskOutput.header->wNo = 1;
    Layout(wrongMaskOutput, args);
    wrongMaskOutput.outputBuf.emplace_back(aliasOutput);
    kernel::ipc::IpcResponse wrongMaskOutputResponse;
    assert(service.HandleRequest(2, session, wrongMaskOutput, wrongMaskOutputResponse).raw == InvalidArgument.raw);
    auto wrongMaskReceiveList{Request(args, input)};
    wrongMaskReceiveList.header->cFlag = kernel::ipc::BufferCFlag::SingleDescriptor;
    wrongMaskReceiveList.outputBuf.emplace_back(aliasOutput);
    kernel::ipc::IpcResponse wrongMaskReceiveResponse;
    assert(service.HandleRequest(2, session, wrongMaskReceiveList, wrongMaskReceiveResponse).raw == InvalidArgument.raw);

    // A raw frame that contains no arguments must not consume stale bytes
    // beyond the frame simply because production cmdArgSz includes headers.
    auto rawTruncated{Request(args, input)};
    rawTruncated.header->rawSize = 8;
    rawTruncated.cmdArgSz = 32;
    kernel::ipc::IpcResponse rawTruncatedResponse;
    assert(service.HandleRequest(1, session, rawTruncated, rawTruncatedResponse).raw == InvalidArgument.raw);
    auto rawOverrun{Request(args, input)};
    rawOverrun.header->rawSize = 100;
    rawOverrun.cmdArgSz = 400;
    kernel::ipc::IpcResponse rawOverrunResponse;
    assert(service.HandleRequest(1, session, rawOverrun, rawOverrunResponse).raw == InvalidArgument.raw);
    const auto domainRequest = [&] {
        auto q{Request(args, input)};
        q.isDomain = true;
        Layout(q, args);
        return q;
    };
    auto domain{domainRequest()};
    kernel::ipc::IpcResponse domainResponse;
    assert(service.HandleRequest(1, session, domain, domainResponse).raw == DataAccessError.raw);
    auto domainMask{domainRequest()};
    domainMask.header->bNo = 1;
    Layout(domainMask, args);
    domainMask.outputBuf.emplace_back(aliasOutput);
    kernel::ipc::IpcResponse domainMaskResponse;
    assert(service.HandleRequest(2, session, domainMask, domainMaskResponse).raw == DataAccessError.raw);
    auto underflow{domainRequest()};
    underflow.domain->payloadSz = 15;
    underflow.cmdArgSz = ~u64{};
    kernel::ipc::IpcResponse underflowResponse;
    assert(service.HandleRequest(1, session, underflow, underflowResponse).raw == InvalidArgument.raw);
    auto domainBeyondRaw{domainRequest()};
    domainBeyondRaw.header->rawSize = 12;
    kernel::ipc::IpcResponse domainBeyondRawResponse;
    assert(service.HandleRequest(1, session, domainBeyondRaw, domainBeyondRawResponse).raw == InvalidArgument.raw);
    auto objectsOverlapPadding{domainRequest()};
    objectsOverlapPadding.domain->inputCount = 1;
    kernel::ipc::IpcResponse objectsResponse;
    assert(service.HandleRequest(1, session, objectsOverlapPadding, objectsResponse).raw == InvalidArgument.raw);
    // Handle/PID and map-alias descriptors shift the raw data start and its
    // alignment. The argument limit is still derived from the actual frame.
    auto withHandles{Request(args, input)};
    withHandles.header->handleDesc = true;
    withHandles.handleDesc = reinterpret_cast<kernel::ipc::HandleDescriptor *>(withHandles.wire->data() + 8);
    *withHandles.handleDesc = {};
    withHandles.handleDesc->sendPid = true;
    withHandles.handleDesc->copyCount = withHandles.handleDesc->moveCount = 1;
    Layout(withHandles, args);
    kernel::ipc::IpcResponse handlesResponse;
    assert(service.HandleRequest(1, session, withHandles, handlesResponse).raw == DataAccessError.raw);
    withHandles.header->bNo = 1;
    Layout(withHandles, args);
    withHandles.outputBuf.emplace_back(aliasOutput);
    kernel::ipc::IpcResponse handlesMaskResponse;
    assert(service.HandleRequest(2, session, withHandles, handlesMaskResponse).raw == DataAccessError.raw);
    auto withDescriptors{Request(args, input)};
    withDescriptors.header->aNo = withDescriptors.header->bNo = 1;
    Layout(withDescriptors, args);
    std::array<u8, 4> descriptorOutput{};
    withDescriptors.outputBuf.emplace_back(descriptorOutput);
    kernel::ipc::IpcResponse descriptorsResponse;
    assert(service.HandleRequest(2, session, withDescriptors, descriptorsResponse).raw == DataAccessError.raw);
    auto excessiveDescriptors{Request(args, input)};
    excessiveDescriptors.header->xNo = excessiveDescriptors.header->aNo = excessiveDescriptors.header->bNo = 15;
    kernel::ipc::IpcResponse excessiveResponse;
    assert(service.HandleRequest(1, session, excessiveDescriptors, excessiveResponse).raw == InvalidArgument.raw);

    for (size_t size = 0; size < sizeof(args); ++size) {
        auto truncated{request};
        truncated.cmdArgSz = size;
        kernel::ipc::IpcResponse response;
        assert(service.HandleRequest(1, session, truncated, response).raw == InvalidArgument.raw);
        assert(response.payload.empty());
    }
    auto nullArguments{request};
    nullArguments.cmdArg = nullptr;
    kernel::ipc::IpcResponse nullResponse;
    assert(service.HandleRequest(1, session, nullArguments, nullResponse).raw == InvalidArgument.raw);
    auto missingBuffer{request};
    missingBuffer.inputBuf.clear();
    kernel::ipc::IpcResponse missingResponse;
    assert(service.HandleRequest(1, session, missingBuffer, missingResponse).raw == InvalidArgument.raw);
    // A descriptor containing an empty string is valid; absence of the
    // descriptor is malformed. Empty text still needs the dictionaries.
    auto emptyBuffer{request};
    emptyBuffer.inputBuf[0] = span<u8>{};
    kernel::ipc::IpcResponse emptyResponse;
    assert(service.HandleRequest(1, session, emptyBuffer, emptyResponse).raw == DataAccessError.raw);
    // Reject a cmdArg pointer outside its TLS wire frame.
    std::array<u8, sizeof(args) + 1> unalignedBytes{};
    std::memcpy(unalignedBytes.data() + 1, &args, sizeof(args));
    auto unaligned{request};
    unaligned.cmdArg = unalignedBytes.data() + 1;
    kernel::ipc::IpcResponse unalignedResponse;
    assert(service.HandleRequest(1, session, unaligned, unalignedResponse).raw == InvalidArgument.raw);

    // Check rejects malformed UTF-8 before trying dictionaries, including a
    // truncated sequence, overlong NUL, surrogate, and values beyond U+10FFFF.
    for (std::vector<u8> invalid : {std::vector<u8>{0xC2}, {0xC0, 0x80}, {0xED, 0xA0, 0x80}, {0xF4, 0x90, 0x80, 0x80}}) {
        auto invalidRequest{Request(args, invalid)};
        kernel::ipc::IpcResponse response;
        assert(service.HandleRequest(1, session, invalidRequest, response).raw == InvalidUtf8.raw);
        assert(response.payload.empty());
    }
    // Bytes following NUL are outside Check's UTF-8 string.
    std::vector<u8> terminated{'a', 0, 0xFF};
    auto terminatedRequest{Request(args, terminated)};
    kernel::ipc::IpcResponse terminatedResponse;
    assert(service.HandleRequest(1, session, terminatedRequest, terminatedResponse).raw == DataAccessError.raw);

    // Mask's IPC contract copies min(input, output) before invoking the filter.
    std::array<u8, 6> output{9, 9, 9, 9, 9, 9};
    auto maskRequest{Request(args, input)};
    maskRequest.header->bNo = 1;
    Layout(maskRequest, args);
    maskRequest.outputBuf.emplace_back(output);
    kernel::ipc::IpcResponse mask;
    assert(service.HandleRequest(2, session, maskRequest, mask).raw == DataAccessError.raw);
    assert((output == std::array<u8, 6>{'a', 'b', 'c', 0, 9, 9}));
    assert(mask.payload.empty());
    for (size_t size = 0; size < sizeof(args); ++size) {
        auto truncated{maskRequest};
        truncated.cmdArgSz = size;
        kernel::ipc::IpcResponse response;
        assert(service.HandleRequest(2, session, truncated, response).raw == InvalidArgument.raw);
    }
    std::array<u8, 2> shortOutput{};
    maskRequest.outputBuf[0] = shortOutput;
    kernel::ipc::IpcResponse shortMask;
    assert(service.HandleRequest(2, session, maskRequest, shortMask).raw == DataAccessError.raw);
    assert((shortOutput == std::array<u8, 2>{'a', 'b'}));
    kernel::ipc::IpcResponse missingOutput;
    assert(service.HandleRequest(2, session, request, missingOutput).raw == InvalidArgument.raw);

    kernel::ipc::IpcResponse reload;
    assert(!service.HandleRequest(3, session, empty, reload));
    assert(reload.payload.empty());

    // Version is the original four-byte BE value, never an arbitrary constant.
    std::vector<u8> versionData{0x01, 0x02, 0x03, 0x04};
    unsigned reloads{};
    auto available = [&](std::string_view name) -> std::optional<std::vector<u8>> {
        if (name == "version.dat") return versionData;
        return std::nullopt;
    };
    INgcServiceForApplication supplied{state, manager, available, [&] { ++reloads; return true; }};
    kernel::ipc::IpcResponse suppliedVersion;
    assert(!supplied.HandleRequest(0, session, empty, suppliedVersion));
    assert(Value<u32>(suppliedVersion) == 0x01020304);
    versionData.push_back(0xFF);
    kernel::ipc::IpcResponse longVersion;
    assert(!supplied.HandleRequest(0, session, empty, longVersion));
    assert(Value<u32>(longVersion) == 0x01020304);
    versionData = {0x11, 0x22};
    kernel::ipc::IpcResponse shortVersion;
    assert(!supplied.HandleRequest(0, session, empty, shortVersion));
    assert(Value<u32>(shortVersion) == 0);
    kernel::ipc::IpcResponse suppliedReload;
    assert(!supplied.HandleRequest(3, session, empty, suppliedReload));
    assert(reloads == 1);

    auto fixtureReader = [&](std::string_view name) -> std::optional<std::vector<u8>> {
        std::ifstream stream{std::string{argv[1]} + "/" + std::string{name}, std::ios::binary};
        if (!stream) return std::nullopt;
        return std::vector<u8>{std::istreambuf_iterator<char>{stream}, {}};
    };
    INgcServiceForApplication real{state, manager, fixtureReader, [] { return true; }};
    const auto checked = [&](std::string_view word, u32 regions, u32 systemRegions, u32 expected) {
        std::vector<u8> bytes{word.begin(), word.end()};
        args.regionMask = regions;
        args.systemRegionMask = systemRegions;
        auto q{Request(args, bytes)};
        kernel::ipc::IpcResponse r;
        assert(!real.HandleRequest(1, session, q, r));
        assert(Value<u32>(r) == expected);
    };
    checked("BAD", 1, 0, 1);
    checked("ordinary", 1, 0, 0);
    checked("regional", 2, 0, 2);
    checked("zork", 2, 1, 3);
    checked("word", 1, 0, 1);
    checked("sword", 1, 0, 0);
    checked("ab@example.com", 2, 1, 3);
    checked("zork", 0, 0, 0); // no requested/system bits exist to report
    checked("ordinary", 0xFFFF0000, 0, 0); // region selection uses low 16 bits
    args = Arguments{};
    std::vector<u8> phrase{'B', 'A', 'D', ' ', 'b', '-', 'a', '-', 'd', 0};
    std::vector<u8> filtered(phrase.size(), 0xFE);
    auto q{Request(args, phrase)};
    q.header->bNo = 1;
    Layout(q, args);
    q.outputBuf.emplace_back(filtered);
    kernel::ipc::IpcResponse r;
    assert(!real.HandleRequest(2, session, q, r));
    assert(Value<i32>(r) == 2);
    assert((filtered == std::vector<u8>{'*', '*', '*', ' ', '*', '*', '*', '*', '*', 0}));
    args.maskMode = 1;
    std::memcpy(q.cmdArg, &args, sizeof(args));
    kernel::ipc::IpcResponse compacted;
    assert(!real.HandleRequest(2, session, q, compacted));
    assert(Value<i32>(compacted) == 2);
    assert((filtered == std::vector<u8>{'*', ' ', '*', 0, 0, 0, 0, 0, 0, 0}));
    args.flags = 1;
    std::memcpy(q.cmdArg, &args, sizeof(args));
    kernel::ipc::IpcResponse unsupported;
    assert((real.HandleRequest(2, session, q, unsupported).raw == Result{10, 1}.raw));
    assert(unsupported.payload.empty());
    // Unsupported filtering still performs the required IPC copy, with no
    // arbitrary asterisks or false count written afterwards.
    assert(filtered == phrase);
    args.flags = 0;
    std::vector<u8> longInput(513, 'x');
    std::copy_n("bad", 3, longInput.begin() + 510);
    std::vector<u8> longOutput(longInput.size());
    auto longMask{Request(args, longInput)};
    longMask.header->bNo = 1;
    Layout(longMask, args);
    longMask.outputBuf.emplace_back(longOutput);
    kernel::ipc::IpcResponse longResponse;
    assert(!real.HandleRequest(2, session, longMask, longResponse));
    assert(Value<i32>(longResponse) == 0);
    assert(longOutput == longInput); // no match may cross the 512-char limit
    args.flags = 0;
    args.regionMask = 1U << 15; // selected dictionary absent, common present
    auto absentRegion{Request(args, input)};
    kernel::ipc::IpcResponse absentRegionResponse;
    assert(real.HandleRequest(1, session, absentRegion, absentRegionResponse).raw == DataAccessError.raw);
    assert(absentRegionResponse.payload.empty());

    std::cout << "NGC command registration, IPC validation, real-format dictionaries, missing data, version and reload: PASS\n";
}
