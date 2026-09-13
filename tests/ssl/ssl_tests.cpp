// SPDX-License-Identifier: MPL-2.0

#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <services/ssl/ISslConnection.h>
#include <services/ssl/ISslContext.h>
#include <services/ssl/ISslService.h>

using namespace skyline;
using namespace skyline::service;
using namespace skyline::service::ssl;

namespace {
    std::vector<u8> ReadFile(const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        assert(stream);
        const auto size{stream.tellg()};
        assert(size >= 0);
        std::vector<u8> data(static_cast<size_t>(size));
        stream.seekg(0);
        if (!data.empty())
            stream.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        assert(stream);
        return data;
    }

    template<typename T>
    T ResponseValue(const kernel::ipc::IpcResponse &response, size_t offset = 0) {
        assert(response.payload.size() >= offset + sizeof(T));
        T value{};
        std::memcpy(&value, response.payload.data() + offset, sizeof(value));
        return value;
    }

    template<typename T>
    kernel::ipc::IpcRequest ArgumentRequest(T &value) {
        kernel::ipc::IpcRequest request;
        request.cmdArg = reinterpret_cast<u8 *>(&value);
        request.cmdArgSz = sizeof(value);
        return request;
    }

    struct LocalTlsServer {
        int socket;
        std::filesystem::path certificate;
        std::filesystem::path key;
        std::thread thread;
        bool succeeded{};

        static int Send(void *context, const unsigned char *data, size_t size) {
            const int fd{*static_cast<int *>(context)};
            const ssize_t result{::send(fd, data, size, MSG_NOSIGNAL)};
            if (result >= 0)
                return static_cast<int>(result);
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return MBEDTLS_ERR_SSL_WANT_WRITE;
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }

        static int Receive(void *context, unsigned char *data, size_t size) {
            const int fd{*static_cast<int *>(context)};
            const ssize_t result{::recv(fd, data, size, 0)};
            if (result >= 0)
                return static_cast<int>(result);
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return MBEDTLS_ERR_SSL_WANT_READ;
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }

        void Start() {
            thread = std::thread([this] {
                mbedtls_ssl_context ssl{};
                mbedtls_ssl_config config{};
                mbedtls_entropy_context entropy{};
                mbedtls_ctr_drbg_context random{};
                mbedtls_x509_crt cert{};
                mbedtls_pk_context privateKey{};
                mbedtls_ssl_init(&ssl);
                mbedtls_ssl_config_init(&config);
                mbedtls_entropy_init(&entropy);
                mbedtls_ctr_drbg_init(&random);
                mbedtls_x509_crt_init(&cert);
                mbedtls_pk_init(&privateKey);

                static constexpr char Personalization[]{"strato-local-tls-server"};
                bool ok{mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
                                              reinterpret_cast<const unsigned char *>(Personalization),
                                              sizeof(Personalization) - 1) == 0};
                ok = ok && mbedtls_x509_crt_parse_file(&cert, certificate.c_str()) == 0;
                ok = ok && mbedtls_pk_parse_keyfile(&privateKey, key.c_str(), nullptr) == 0;
                ok = ok && mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                       MBEDTLS_SSL_PRESET_DEFAULT) == 0;
                if (ok) {
                    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
                    ok = mbedtls_ssl_conf_own_cert(&config, &cert, &privateKey) == 0;
                }
                ok = ok && mbedtls_ssl_setup(&ssl, &config) == 0;
                if (ok) {
                    mbedtls_ssl_set_bio(&ssl, &socket, Send, Receive, nullptr);
                    ok = mbedtls_ssl_handshake(&ssl) == 0;
                }

                std::array<u8, 4> input{};
                size_t received{};
                while (ok && received < input.size()) {
                    const int count{mbedtls_ssl_read(&ssl, input.data() + received, input.size() - received)};
                    if (count <= 0)
                        ok = false;
                    else
                        received += static_cast<size_t>(count);
                }
                ok = ok && std::string_view(reinterpret_cast<const char *>(input.data()), input.size()) == "ping";

                constexpr std::string_view Reply{"pong"};
                size_t written{};
                while (ok && written < Reply.size()) {
                    const int count{mbedtls_ssl_write(&ssl, reinterpret_cast<const u8 *>(Reply.data()) + written,
                                                      Reply.size() - written)};
                    if (count <= 0)
                        ok = false;
                    else
                        written += static_cast<size_t>(count);
                }
                succeeded = ok;
                if (ok)
                    mbedtls_ssl_close_notify(&ssl);

                mbedtls_pk_free(&privateKey);
                mbedtls_x509_crt_free(&cert);
                mbedtls_ctr_drbg_free(&random);
                mbedtls_entropy_free(&entropy);
                mbedtls_ssl_config_free(&config);
                mbedtls_ssl_free(&ssl);
                ::close(socket);
                socket = -1;
            });
        }

        void Join() {
            if (thread.joinable())
                thread.join();
        }

        ~LocalTlsServer() {
            Join();
            if (socket >= 0)
                ::close(socket);
        }
    };

    void WriteCertificateStore(const std::filesystem::path &path, const std::vector<u8> &certificate) {
        std::filesystem::create_directories(path.parent_path());
        CertStoreHeader header{CertStoreMagic, 1};
        CertStoreEntry entry{42, 1, static_cast<u32>(certificate.size()), sizeof(CertStoreEntry)};
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char *>(&entry), sizeof(entry));
        stream.write(reinterpret_cast<const char *>(certificate.data()), static_cast<std::streamsize>(certificate.size()));
        assert(stream);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::filesystem::path repository{argv[1]};
    const auto dataDirectory{repository / "app/libraries/mbedtls/tests/data_files"};
    const auto serverPem{ReadFile(dataDirectory / "server1.crt")};
    const auto serverDer{ReadFile(dataDirectory / "server1.crt.der")};
    const auto serverKey{ReadFile(dataDirectory / "server1.key")};
    const auto caPem{ReadFile(dataDirectory / "test-ca.crt")};
    const auto caDer{ReadFile(dataDirectory / "test-ca.crt.der")};

    char temporary[] = "/tmp/strato-ssl-XXXXXX";
    const char *directory{mkdtemp(temporary)};
    assert(directory);
    HostOS os{std::string(directory) + "/public", std::string(directory) + "/private"};
    DeviceState device{&os};
    WriteCertificateStore(std::filesystem::path(os.privateAppFilesPath) / "ssl/ssl_TrustedCerts.bdf", caDer);

    SslContextState pkiState{{SslVersionAuto | (3U << 24)}, 3, ServicePermission::User, false};
    auto pemId{pkiState.ImportServerPki(CertificateFormat::Pem, serverPem)};
    auto derId{pkiState.ImportServerPki(CertificateFormat::Der, serverDer)};
    auto duplicateId{pkiState.ImportServerPki(CertificateFormat::Pem, serverPem)};
    assert(pemId && derId && duplicateId && *pemId != *derId && *derId != *duplicateId);
    const std::array<u8, 3> invalidCertificate{1, 2, 3};
    assert(pkiState.ImportServerPki(CertificateFormat::Der, invalidCertificate).result.raw == result::BadCertificate.raw);
    assert(!pkiState.RemoveServerPki(*pemId));
    assert(pkiState.RemoveServerPki(*pemId).raw == result::PkiNotFound.raw);
    auto clientId{pkiState.ImportClientCertKeyPki(CertificateFormat::Pem, serverPem, serverKey)};
    assert(clientId);
    assert(pkiState.ImportClientCertKeyPki(CertificateFormat::Pem, serverPem, serverKey).result.raw ==
           result::ClientPkiAlreadyRegistered.raw);
    assert(!pkiState.RemoveClientPki(*clientId));
    assert(ValidateSslVersion({SslVersionTls13}).raw == result::AlertProtocolVersion.raw);

    auto sharedState{std::make_shared<SslSharedState>(device)};
    ServiceManager manager;
    kernel::type::KSession session;
    ISslService userService{device, manager, sharedState, ServicePermission::User};
    u32 interfaceVersion{3};
    auto versionRequest{ArgumentRequest(interfaceVersion)};
    kernel::ipc::IpcResponse versionResponse;
    assert(!userService.SetInterfaceVersion(session, versionRequest, versionResponse));

    u32 debugOption{static_cast<u32>(DebugOption::AllowDisableVerifyOption)};
    u8 debugEnabled{1};
    auto setDebugRequest{ArgumentRequest(debugOption)};
    setDebugRequest.inputBuf.emplace_back(&debugEnabled, sizeof(debugEnabled));
    kernel::ipc::IpcResponse setDebugResponse;
    assert(!userService.SetDebugOption(session, setDebugRequest, setDebugResponse));
    u8 debugValue{};
    auto getDebugRequest{ArgumentRequest(debugOption)};
    getDebugRequest.outputBuf.emplace_back(&debugValue, sizeof(debugValue));
    kernel::ipc::IpcResponse getDebugResponse;
    assert(!userService.GetDebugOption(session, getDebugRequest, getDebugResponse));
    assert(debugValue == 1 && getDebugResponse.payload.empty());

    std::array<u8, 0x10> contextArguments{};
    const u32 contextVersion{SslVersionAuto | (3U << 24)};
    std::memcpy(contextArguments.data(), &contextVersion, sizeof(contextVersion));
    kernel::ipc::IpcRequest contextRequest;
    contextRequest.cmdArg = contextArguments.data();
    contextRequest.cmdArgSz = contextArguments.size();
    kernel::ipc::IpcResponse contextResponse;
    assert(!userService.CreateContext(session, contextRequest, contextResponse));
    assert(sharedState->contextCount.load() == 1 && manager.registered.size() == 1);

    auto contextService{std::dynamic_pointer_cast<ISslContext>(manager.registered.back())};
    assert(contextService);
    kernel::ipc::IpcRequest connectionRequest;
    kernel::ipc::IpcResponse connectionResponse;
    assert(!contextService->CreateConnection(session, connectionRequest, connectionResponse));
    assert(manager.registered.size() == 2);
    auto lifecycleConnection{std::dynamic_pointer_cast<ISslConnection>(manager.registered.back())};
    assert(lifecycleConnection);
    i32 invalidSocket{-1};
    auto invalidSocketRequest{ArgumentRequest(invalidSocket)};
    kernel::ipc::IpcResponse invalidSocketResponse;
    assert(lifecycleConnection->SetSocketDescriptor(session, invalidSocketRequest, invalidSocketResponse).raw == result::InvalidSocket.raw);
    kernel::ipc::IpcRequest noSocketPendingRequest;
    kernel::ipc::IpcResponse noSocketPendingResponse;
    assert(lifecycleConnection->Pending(session, noSocketPendingRequest, noSocketPendingResponse).raw == result::NoSocket.raw);
    lifecycleConnection.reset();
    manager.registered.pop_back();
    manager.failRegistration = true;
    bool registrationFailed{};
    try {
        kernel::ipc::IpcResponse failedConnectionResponse;
        contextService->CreateConnection(session, connectionRequest, failedConnectionResponse);
    } catch (const std::runtime_error &) {
        registrationFailed = true;
    }
    manager.failRegistration = false;
    kernel::ipc::IpcResponse connectionCountResponse;
    assert(registrationFailed && !contextService->GetConnectionCount(session, connectionRequest, connectionCountResponse));
    assert(ResponseValue<u32>(connectionCountResponse) == 0);

    std::array<u8, 1> fakePkcs12{1};
    kernel::ipc::IpcRequest missingPasswordBufferRequest;
    missingPasswordBufferRequest.inputBuf.emplace_back(fakePkcs12);
    kernel::ipc::IpcResponse missingPasswordBufferResponse;
    assert(contextService->ImportClientPki(session, missingPasswordBufferRequest, missingPasswordBufferResponse).raw ==
           result::InvalidCertificateBufferSize.raw);
    missingPasswordBufferRequest.inputBuf.emplace_back(span<u8>{});
    assert(contextService->ImportClientPki(session, missingPasswordBufferRequest, missingPasswordBufferResponse).raw ==
           result::UnsupportedCertificate.raw);

    contextService.reset();
    manager.registered.clear();
    assert(sharedState->contextCount.load() == 0);

    std::array<i32, 1> certificateIds{-1};
    std::vector<u8> certificateOutput(2 * sizeof(BuiltInCertificateInfo) + caDer.size() + 4);
    kernel::ipc::IpcRequest certificatesRequest;
    certificatesRequest.inputBuf.emplace_back(reinterpret_cast<u8 *>(certificateIds.data()), sizeof(certificateIds));
    certificatesRequest.outputBuf.emplace_back(certificateOutput);
    kernel::ipc::IpcResponse certificatesResponse;
    assert(!userService.GetCertificates(session, certificatesRequest, certificatesResponse));
    assert(ResponseValue<u32>(certificatesResponse) == 1);
    BuiltInCertificateInfo info{};
    BuiltInCertificateInfo terminator{};
    std::memcpy(&info, certificateOutput.data(), sizeof(info));
    std::memcpy(&terminator, certificateOutput.data() + sizeof(info), sizeof(terminator));
    assert(info.certificateId == 42 && info.certificateSize == caDer.size());
    assert(terminator.certificateId == -1 && terminator.status == -1);

    std::array<i32, 1> specificCertificateId{42};
    kernel::ipc::IpcRequest specificSizeRequest;
    specificSizeRequest.inputBuf.emplace_back(reinterpret_cast<u8 *>(specificCertificateId.data()), sizeof(specificCertificateId));
    kernel::ipc::IpcResponse specificSizeResponse;
    assert(!userService.GetCertificateBufSize(session, specificSizeRequest, specificSizeResponse));
    const u32 specificSize{ResponseValue<u32>(specificSizeResponse)};
    assert(specificSize == sizeof(BuiltInCertificateInfo) + caDer.size());
    std::vector<u8> specificCertificateOutput(specificSize);
    kernel::ipc::IpcRequest specificCertificatesRequest;
    specificCertificatesRequest.inputBuf.emplace_back(reinterpret_cast<u8 *>(specificCertificateId.data()), sizeof(specificCertificateId));
    specificCertificatesRequest.outputBuf.emplace_back(specificCertificateOutput);
    kernel::ipc::IpcResponse specificCertificatesResponse;
    assert(!userService.GetCertificates(session, specificCertificatesRequest, specificCertificatesResponse));
    assert(ResponseValue<u32>(specificCertificatesResponse) == 1);

    userService.OnSessionClosed(session);
    debugValue = 0;
    assert(userService.GetDebugOption(session, getDebugRequest, getDebugResponse).raw == result::InvalidOption.raw);

    kernel::ipc::IpcResponse deniedResponse;
    assert(userService.CreateContextForSystem(session, contextRequest, deniedResponse).raw == result::AccessDenied.raw);
    ISslService systemService{device, manager, sharedState, ServicePermission::System};
    auto systemVersionRequest{ArgumentRequest(interfaceVersion)};
    kernel::ipc::IpcResponse systemVersionResponse;
    assert(!systemService.SetInterfaceVersion(session, systemVersionRequest, systemVersionResponse));
    kernel::ipc::IpcResponse systemContextResponse;
    assert(!systemService.CreateContextForSystem(session, contextRequest, systemContextResponse));
    manager.registered.clear();
    assert(sharedState->contextCount.load() == 0);

    auto tlsContext{std::make_shared<SslContextState>(SslVersion{SslVersionAuto | (3U << 24)}, 3,
                                                     ServicePermission::User, false)};
    assert(tlsContext->ImportServerPki(CertificateFormat::Pem, caPem));
    tlsContext->connectionCount.store(1);
    auto tlsConnection{std::make_shared<ISslConnection>(device, manager, tlsContext, sharedState->sessionCache)};

    std::array<u8, 8> optionArguments{};
    optionArguments[0] = 1;
    const u32 chainOption{static_cast<u32>(OptionType::GetServerCertChain)};
    std::memcpy(optionArguments.data() + 4, &chainOption, sizeof(chainOption));
    kernel::ipc::IpcRequest optionRequest;
    optionRequest.cmdArg = optionArguments.data();
    optionRequest.cmdArgSz = optionArguments.size();
    kernel::ipc::IpcResponse optionResponse;
    assert(!tlsConnection->SetOption(session, optionRequest, optionResponse));

    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    i32 socketArgument{sockets[0]};
    auto socketRequest{ArgumentRequest(socketArgument)};
    kernel::ipc::IpcResponse socketResponse;
    assert(!tlsConnection->SetSocketDescriptor(session, socketRequest, socketResponse));
    assert(ResponseValue<i32>(socketResponse) == sockets[0]);
    kernel::ipc::IpcRequest getSocketRequest;
    kernel::ipc::IpcResponse getSocketResponse;
    assert(!tlsConnection->GetSocketDescriptor(session, getSocketRequest, getSocketResponse));
    const int returnedSocket{ResponseValue<i32>(getSocketResponse)};
    assert(::fcntl(returnedSocket, F_GETFD) >= 0);
    ::close(returnedSocket);
    ::close(sockets[0]);
    sockets[0] = -1;

    std::string hostname{"localhost"};
    hostname.push_back('\0');
    kernel::ipc::IpcRequest hostnameRequest;
    hostnameRequest.inputBuf.emplace_back(reinterpret_cast<u8 *>(hostname.data()), hostname.size());
    kernel::ipc::IpcResponse hostnameResponse;
    assert(!tlsConnection->SetHostName(session, hostnameRequest, hostnameResponse));
    u32 verifyOption{VerifyPeerCa | VerifyHostName | VerifyDate};
    auto verifyRequest{ArgumentRequest(verifyOption)};
    kernel::ipc::IpcResponse verifyResponse;
    assert(!tlsConnection->SetVerifyOption(session, verifyRequest, verifyResponse));
    u32 nonBlocking{static_cast<u32>(IoMode::NonBlocking)};
    auto nonBlockingRequest{ArgumentRequest(nonBlocking)};
    kernel::ipc::IpcResponse nonBlockingResponse;
    assert(!tlsConnection->SetIoMode(session, nonBlockingRequest, nonBlockingResponse));
    kernel::ipc::IpcRequest handshakeRequest;
    kernel::ipc::IpcResponse handshakeResponse;
    assert(tlsConnection->DoHandshake(session, handshakeRequest, handshakeResponse).raw == result::WouldBlock.raw);

    LocalTlsServer server{sockets[1], dataDirectory / "server2-sha256.crt", dataDirectory / "server2.key", {}, false};
    server.Start();
    u32 blocking{static_cast<u32>(IoMode::Blocking)};
    auto blockingRequest{ArgumentRequest(blocking)};
    kernel::ipc::IpcResponse blockingResponse;
    assert(!tlsConnection->SetIoMode(session, blockingRequest, blockingResponse));
    std::vector<u8> serverCertificate(4096);
    kernel::ipc::IpcRequest handshakeCertificateRequest;
    handshakeCertificateRequest.outputBuf.emplace_back(serverCertificate);
    kernel::ipc::IpcResponse handshakeCertificateResponse;
    assert(!tlsConnection->DoHandshakeGetServerCert(session, handshakeCertificateRequest, handshakeCertificateResponse));
    assert(ResponseValue<u32>(handshakeCertificateResponse) > sizeof(ServerCertificateChainHeader));
    assert(ResponseValue<u32>(handshakeCertificateResponse, sizeof(u32)) == 1);

    std::array<u8, 4> ping{'p', 'i', 'n', 'g'};
    kernel::ipc::IpcRequest writeRequest;
    writeRequest.inputBuf.emplace_back(ping);
    kernel::ipc::IpcResponse writeResponse;
    assert(!tlsConnection->Write(session, writeRequest, writeResponse));
    assert(ResponseValue<u32>(writeResponse) == ping.size());

    std::array<u8, 4> peeked{};
    kernel::ipc::IpcRequest peekRequest;
    peekRequest.outputBuf.emplace_back(peeked);
    kernel::ipc::IpcResponse peekResponse;
    assert(!tlsConnection->Peek(session, peekRequest, peekResponse));
    assert(std::string_view(reinterpret_cast<const char *>(peeked.data()), peeked.size()) == "pong");
    kernel::ipc::IpcRequest pendingRequest;
    kernel::ipc::IpcResponse pendingResponse;
    assert(!tlsConnection->Pending(session, pendingRequest, pendingResponse));
    assert(ResponseValue<i32>(pendingResponse) == 4);
    std::array<u32, 2> pollArguments{PollRead, 0};
    kernel::ipc::IpcRequest pollRequest;
    pollRequest.cmdArg = reinterpret_cast<u8 *>(pollArguments.data());
    pollRequest.cmdArgSz = sizeof(pollArguments);
    kernel::ipc::IpcResponse pollResponse;
    assert(!tlsConnection->Poll(session, pollRequest, pollResponse));
    assert(ResponseValue<u32>(pollResponse) & PollRead);
    std::array<u8, 4> reply{};
    kernel::ipc::IpcRequest readRequest;
    readRequest.outputBuf.emplace_back(reply);
    kernel::ipc::IpcResponse readResponse;
    assert(!tlsConnection->Read(session, readRequest, readResponse));
    assert(reply == peeked && ResponseValue<u32>(readResponse) == reply.size());
    server.Join();
    assert(server.succeeded);
    tlsConnection.reset();
    assert(tlsContext->connectionCount.load() == 0);

    auto ownershipContext{std::make_shared<SslContextState>(SslVersion{SslVersionAuto}, 3,
                                                            ServicePermission::User, false)};
    ownershipContext->connectionCount.store(1);
    auto ownershipConnection{std::make_shared<ISslConnection>(device, manager, ownershipContext, sharedState->sessionCache)};
    std::array<u8, 8> ownershipOptionArguments{};
    ownershipOptionArguments[0] = 1;
    const u32 ownershipOption{static_cast<u32>(OptionType::DoNotCloseSocket)};
    std::memcpy(ownershipOptionArguments.data() + sizeof(u32), &ownershipOption, sizeof(ownershipOption));
    kernel::ipc::IpcRequest ownershipOptionRequest;
    ownershipOptionRequest.cmdArg = ownershipOptionArguments.data();
    ownershipOptionRequest.cmdArgSz = ownershipOptionArguments.size();
    kernel::ipc::IpcResponse ownershipOptionResponse;
    assert(!ownershipConnection->SetOption(session, ownershipOptionRequest, ownershipOptionResponse));
    int ownershipSockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, ownershipSockets) == 0);
    i32 ownershipSocketArgument{ownershipSockets[0]};
    auto ownershipSocketRequest{ArgumentRequest(ownershipSocketArgument)};
    kernel::ipc::IpcResponse ownershipSocketResponse;
    assert(!ownershipConnection->SetSocketDescriptor(session, ownershipSocketRequest, ownershipSocketResponse));
    assert(ResponseValue<i32>(ownershipSocketResponse) == -1);
    kernel::ipc::IpcRequest ownershipGetSocketRequest;
    kernel::ipc::IpcResponse ownershipGetSocketResponse;
    assert(!ownershipConnection->GetSocketDescriptor(session, ownershipGetSocketRequest, ownershipGetSocketResponse));
    const int exportedSocket{ResponseValue<i32>(ownershipGetSocketResponse)};
    ownershipConnection.reset();
    assert(ownershipContext->connectionCount.load() == 0);
    assert(::fcntl(ownershipSockets[0], F_GETFD) >= 0 && ::fcntl(exportedSocket, F_GETFD) >= 0);
    ::close(exportedSocket);
    ::close(ownershipSockets[0]);
    ::close(ownershipSockets[1]);

    int untrustedSockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, untrustedSockets) == 0);
    LocalTlsServer untrustedServer{untrustedSockets[1], dataDirectory / "server2-sha256.crt", dataDirectory / "server2.key", {}, false};
    untrustedServer.Start();
    auto untrustedContext{std::make_shared<SslContextState>(SslVersion{SslVersionAuto}, 3, ServicePermission::User, false)};
    TlsBackend untrustedBackend{untrustedContext, sharedState->sessionCache};
    assert(!untrustedBackend.SetSocket(untrustedSockets[0]));
    assert(!untrustedBackend.SetHostname("localhost"));
    assert(!untrustedBackend.SetVerifyOption(VerifyPeerCa | VerifyHostName | VerifyDate));
    const Result verificationResult{untrustedBackend.Handshake()};
    if (verificationResult.module != result::UnknownCa.module || verificationResult.id != result::UnknownCa.id)
        std::cerr << "unexpected verification result: module=" << verificationResult.module << " id=" << verificationResult.id << '\n';
    assert(verificationResult.module == result::UnknownCa.module && verificationResult.id == result::UnknownCa.id);
    assert(!untrustedBackend.VerificationErrors().empty());
    ::close(untrustedSockets[0]);
    untrustedServer.Join();

    std::filesystem::remove_all(directory);
    std::cout << "SSL state, PKI lifecycle, IPC gates, TLS handshake, I/O, poll and verification: PASS\n";
}
