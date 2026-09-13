// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <chrono>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crl.h>
#include <mbedtls/x509_crt.h>
#include "state.h"
namespace skyline::service::ssl {
    class TlsSessionCache {
        struct Session;
        std::mutex mutex;
        std::unordered_map<std::string, std::unique_ptr<Session>> sessions;
      public:
        TlsSessionCache(); ~TlsSessionCache();
        bool Load(const std::string &, mbedtls_ssl_context &);
        void Save(const std::string &, const mbedtls_ssl_context &);
        u32 Flush(const std::optional<std::string> &);
    };
    class TlsBackend {
        std::shared_ptr<SslContextState> contextState;
        std::shared_ptr<TlsSessionCache> sessionCache;
        mbedtls_ssl_context ssl{}; mbedtls_ssl_config config{}; mbedtls_entropy_context entropy{}; mbedtls_ctr_drbg_context random{};
        mbedtls_x509_crt caCertificates{}; mbedtls_x509_crt clientCertificate{}; mbedtls_pk_context clientPrivateKey{}; mbedtls_x509_crl crls{};
        int socket{-1}; int socketError{}; std::string hostname; u32 verifyOption{VerifyPeerCa | VerifyHostName};
        IoMode ioMode{IoMode::Blocking}; SessionCacheMode cacheMode{SessionCacheMode::None}; RenegotiationMode renegotiationMode{RenegotiationMode::None};
        u32 ioTimeoutMilliseconds{DefaultIoTimeoutMilliseconds}; bool configured{}; bool handshaken{};
        std::vector<std::string> alpnProtocols; std::vector<const char *> alpnPointers; std::vector<Result> verificationErrors;
        static int SendCallback(void *, const unsigned char *, size_t);
        static int ReceiveCallback(void *, unsigned char *, size_t);
        static int VerifyCallback(void *, mbedtls_x509_crt *, int, u32 *);
        Result Configure(); Result MapError(int, bool); void CollectVerificationErrors(u32);
        Result WaitForIo(short, std::chrono::steady_clock::time_point) const;
        Result Drive(const std::function<int()> &, bool, int &);
      public:
        TlsBackend(std::shared_ptr<SslContextState>, std::shared_ptr<TlsSessionCache>); ~TlsBackend();
        TlsBackend(const TlsBackend &) = delete; TlsBackend &operator=(const TlsBackend &) = delete;
        Result SetSocket(int); Result SetHostname(std::string); Result SetVerifyOption(u32); Result SetIoMode(IoMode);
        Result SetSessionCacheMode(SessionCacheMode); Result SetRenegotiationMode(RenegotiationMode); Result SetIoTimeout(u32);
        Result SetAlpnProtocols(std::vector<std::string>); Result Handshake(); ResultValue<size_t> Read(span<u8>); ResultValue<size_t> Write(span<const u8>); Result CloseNotify();
        [[nodiscard]] size_t Pending() const; [[nodiscard]] std::vector<std::vector<u8>> PeerCertificateChain() const;
        [[nodiscard]] std::vector<Result> VerificationErrors() const; [[nodiscard]] std::string CipherName() const;
        [[nodiscard]] std::string ProtocolVersion() const; [[nodiscard]] std::optional<std::string> SelectedAlpn() const; [[nodiscard]] bool IsHandshaken() const;
    };
}
