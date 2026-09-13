// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)
#pragma once
#include <atomic>
#include <optional>
#include <unordered_map>
#include "types.h"
namespace skyline::service::ssl {
    class TlsSessionCache;
    struct CertificateBlob { CertificateFormat format{}; std::vector<u8> data; };
    struct ClientCertificateBlob { CertificateFormat format{}; std::vector<u8> certificate; std::vector<u8> privateKey; };
    struct CertificateStoreCertificate { i32 id{}; i32 status{}; std::vector<u8> der; };
    class CertificateStore {
        std::vector<CertificateStoreCertificate> certificates;
        bool loaded{};
      public:
        explicit CertificateStore(const DeviceState &state);
        [[nodiscard]] ResultValue<std::vector<const CertificateStoreCertificate *>> Select(span<const i32> ids) const;
        [[nodiscard]] std::vector<CertificateBlob> TrustedCertificates() const;
    };
    struct SslSharedState {
        std::atomic<u32> contextCount{};
        CertificateStore certificateStore;
        std::shared_ptr<TlsSessionCache> sessionCache;
        explicit SslSharedState(const DeviceState &state);
    };
    class SslContextState {
        mutable std::mutex mutex;
        u64 nextPkiId{1};
        std::unordered_map<u64, CertificateBlob> serverPki;
        std::optional<std::pair<u64, ClientCertificateBlob>> clientPki;
        std::unordered_map<u64, std::vector<u8>> crls;
        std::vector<std::string> policyOids;
        std::vector<CertificateBlob> defaultTrustedPki;
        [[nodiscard]] std::optional<u64> AllocateIdLocked();
      public:
        const SslVersion version;
        const u32 interfaceVersion;
        const ServicePermission permission;
        const bool allowDisableVerifyOption;
        std::atomic<u32> connectionCount{};
        std::atomic<i32> crlImportDateCheck{1};
        SslContextState(SslVersion version, u32 interfaceVersion, ServicePermission permission, bool allowDisableVerifyOption,
                        std::vector<CertificateBlob> defaultTrustedPki = {});
        Result SetOption(ContextOption option, i32 value);
        ResultValue<i32> GetOption(ContextOption option) const;
        ResultValue<u64> ImportServerPki(CertificateFormat format, span<const u8> data);
        ResultValue<u64> ImportClientCertKeyPki(CertificateFormat format, span<const u8> certificate, span<const u8> privateKey);
        ResultValue<u64> ImportClientPki(span<const u8> pkcs12, span<const u8> password);
        Result RemoveServerPki(u64 id);
        Result RemoveClientPki(u64 id);
        ResultValue<u64> ImportCrl(span<const u8> data);
        Result RemoveCrl(u64 id);
        Result AddPolicyOid(span<const u8> oid);
        [[nodiscard]] std::vector<CertificateBlob> CopyServerPki() const;
        [[nodiscard]] std::vector<CertificateBlob> CopyDefaultTrustedPki() const;
        [[nodiscard]] std::optional<ClientCertificateBlob> CopyClientPki() const;
        [[nodiscard]] std::vector<std::vector<u8>> CopyCrls() const;
        [[nodiscard]] bool HasPolicyOids() const;
    };
    Result ValidateSslVersion(SslVersion version);
    Result ValidateCertificate(CertificateFormat format, span<const u8> data);
}
