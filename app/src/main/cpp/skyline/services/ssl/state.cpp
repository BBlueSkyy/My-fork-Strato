// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crl.h>
#include <mbedtls/x509_crt.h>
#include <os.h>
#include "state.h"
#include "tls_backend.h"
namespace skyline::service::ssl {
    namespace {
        std::vector<u8> PreparePem(span<const u8> data) {
            std::vector<u8> out(data.begin(), data.end());
            if (out.empty() || out.back() != 0) out.push_back(0);
            return out;
        }
        Result ParseCertificate(mbedtls_x509_crt &crt, CertificateFormat format, span<const u8> data) {
            if (!IsValidCertificateFormat(format) || data.empty()) return result::InvalidCertificateBufferSize;
            int rc{};
            if (format == CertificateFormat::Pem) {
                auto prepared{PreparePem(data)};
                rc = mbedtls_x509_crt_parse(&crt, prepared.data(), prepared.size());
            } else rc = mbedtls_x509_crt_parse_der(&crt, data.data(), data.size());
            return rc == 0 ? Result{} : result::BadCertificate;
        }
        std::optional<std::vector<u8>> ReadFile(const std::filesystem::path &path) {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) return std::nullopt;
            auto size{stream.tellg()};
            if (size < 0 || static_cast<u64>(size) > std::numeric_limits<u32>::max()) return std::nullopt;
            std::vector<u8> data(static_cast<size_t>(size));
            stream.seekg(0);
            if (!data.empty() && !stream.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()))) return std::nullopt;
            return data;
        }
        bool ParseStore(span<const u8> data, std::vector<CertificateStoreCertificate> &output) {
            if (data.size() < sizeof(CertStoreHeader)) return false;
            CertStoreHeader header{};
            std::memcpy(&header, data.data(), sizeof(header));
            if (header.magic != CertStoreMagic || header.entryCount > (data.size() - sizeof(header)) / sizeof(CertStoreEntry)) return false;
            const size_t tableEnd{sizeof(header) + static_cast<size_t>(header.entryCount) * sizeof(CertStoreEntry)};
            output.clear(); output.reserve(header.entryCount);
            for (u32 i{}; i < header.entryCount; ++i) {
                CertStoreEntry entry{};
                std::memcpy(&entry, data.data() + sizeof(header) + static_cast<size_t>(i) * sizeof(entry), sizeof(entry));
                const u64 offset{sizeof(header) + static_cast<u64>(entry.certificateOffset)};
                const u64 end{offset + entry.certificateSize};
                if (offset < tableEnd || end < offset || end > data.size()) return false;
                CertificateStoreCertificate cert{entry.certificateId, entry.status,
                    std::vector<u8>(data.begin() + static_cast<ptrdiff_t>(offset), data.begin() + static_cast<ptrdiff_t>(end))};
                mbedtls_x509_crt parsed{}; mbedtls_x509_crt_init(&parsed);
                bool valid{mbedtls_x509_crt_parse_der(&parsed, cert.der.data(), cert.der.size()) == 0};
                mbedtls_x509_crt_free(&parsed);
                if (!valid) return false;
                output.emplace_back(std::move(cert));
            }
            return true;
        }
    }
    CertificateStore::CertificateStore(const DeviceState &state) {
        const std::array paths{
            std::filesystem::path(state.os->privateAppFilesPath) / "ssl" / "ssl_TrustedCerts.bdf",
            std::filesystem::path(state.os->publicAppFilesPath) / "switch" / "ssl" / "ssl_TrustedCerts.bdf"};
        for (const auto &path : paths) {
            auto data{ReadFile(path)};
            if (data && ParseStore(*data, certificates)) { loaded = true; LOGI("Loaded HOS SSL certificate store from {}", path.string()); return; }
        }
        LOGI("No HOS SSL certificate store supplied; Android roots remain available");
    }
    ResultValue<std::vector<const CertificateStoreCertificate *>> CertificateStore::Select(span<const i32> ids) const {
        if (!loaded) return result::NoCertificate;
        if (ids.empty()) return result::InvalidCertificateBufferSize;
        std::vector<const CertificateStoreCertificate *> selected;
        if (ids.size() == 1 && ids.front() == -1) {
            for (const auto &certificate : certificates) selected.push_back(&certificate);
            return selected;
        }
        for (i32 id : ids) {
            auto it{std::find_if(certificates.begin(), certificates.end(), [id](const auto &entry) { return entry.id == id; })};
            if (it != certificates.end()) selected.push_back(&*it);
        }
        return selected;
    }
    std::vector<CertificateBlob> CertificateStore::TrustedCertificates() const {
        std::vector<CertificateBlob> trusted;
        for (const auto &certificate : certificates) if (certificate.status == 1) trusted.push_back({CertificateFormat::Der, certificate.der});
        return trusted;
    }
    SslSharedState::SslSharedState(const DeviceState &state) : certificateStore(state), sessionCache(std::make_shared<TlsSessionCache>()) {}
    SslContextState::SslContextState(SslVersion version, u32 interfaceVersion, ServicePermission permission, bool allowDisableVerifyOption,
                                     std::vector<CertificateBlob> defaultTrustedPki)
        : defaultTrustedPki(std::move(defaultTrustedPki)), version(version), interfaceVersion(interfaceVersion), permission(permission),
          allowDisableVerifyOption(allowDisableVerifyOption) {}
    std::optional<u64> SslContextState::AllocateIdLocked() {
        if (nextPkiId == 0 || nextPkiId == std::numeric_limits<u64>::max()) return std::nullopt;
        return nextPkiId++;
    }
    Result SslContextState::SetOption(ContextOption option, i32 value) {
        if (option != ContextOption::CrlImportDateCheckEnable || (value != 0 && value != 1)) return result::InvalidOption;
        crlImportDateCheck.store(value); return {};
    }
    ResultValue<i32> SslContextState::GetOption(ContextOption option) const {
        if (option != ContextOption::CrlImportDateCheckEnable) return result::InvalidOption;
        return crlImportDateCheck.load();
    }
    Result ValidateCertificate(CertificateFormat format, span<const u8> data) {
        mbedtls_x509_crt crt{}; mbedtls_x509_crt_init(&crt); Result rc{ParseCertificate(crt, format, data)}; mbedtls_x509_crt_free(&crt); return rc;
    }
    ResultValue<u64> SslContextState::ImportServerPki(CertificateFormat format, span<const u8> data) {
        if (Result rc{ValidateCertificate(format, data)}; rc) return rc;
        std::scoped_lock lock{mutex};
        if (serverPki.size() >= MaximumServerPkiCount) return result::MaximumServerPkiRegistered;
        auto id{AllocateIdLocked()}; if (!id) return result::InsufficientMemory;
        serverPki.emplace(*id, CertificateBlob{format, {data.begin(), data.end()}}); return *id;
    }
    ResultValue<u64> SslContextState::ImportClientCertKeyPki(CertificateFormat format, span<const u8> certificate, span<const u8> keyData) {
        if (!IsValidCertificateFormat(format) || certificate.empty() || keyData.empty()) return result::InvalidCertificateBufferSize;
        mbedtls_x509_crt crt{}; mbedtls_pk_context key{}; mbedtls_x509_crt_init(&crt); mbedtls_pk_init(&key);
        Result rc{ParseCertificate(crt, format, certificate)};
        if (!rc) {
            auto prepared{format == CertificateFormat::Pem ? PreparePem(keyData) : std::vector<u8>(keyData.begin(), keyData.end())};
            if (mbedtls_pk_parse_key(&key, prepared.data(), prepared.size(), nullptr, 0) != 0 || mbedtls_pk_check_pair(&crt.pk, &key) != 0) rc = result::BadCertificate;
        }
        mbedtls_pk_free(&key); mbedtls_x509_crt_free(&crt); if (rc) return rc;
        std::scoped_lock lock{mutex}; if (clientPki) return result::ClientPkiAlreadyRegistered;
        auto id{AllocateIdLocked()}; if (!id) return result::InsufficientMemory;
        clientPki.emplace(*id, ClientCertificateBlob{format, {certificate.begin(), certificate.end()}, {keyData.begin(), keyData.end()}}); return *id;
    }
    ResultValue<u64> SslContextState::ImportClientPki(span<const u8> pkcs12, span<const u8>) {
        if (pkcs12.empty()) return result::InvalidCertificateBufferSize;
        return result::UnsupportedCertificate;
    }
    Result SslContextState::RemoveServerPki(u64 id) { std::scoped_lock lock{mutex}; return serverPki.erase(id) ? Result{} : result::PkiNotFound; }
    Result SslContextState::RemoveClientPki(u64 id) { std::scoped_lock lock{mutex}; if (!clientPki || clientPki->first != id) return result::PkiNotFound; clientPki.reset(); return {}; }
    ResultValue<u64> SslContextState::ImportCrl(span<const u8> data) {
        if (data.empty()) return result::InvalidCrlFormat;
        mbedtls_x509_crl crl{}; mbedtls_x509_crl_init(&crl); int rc{mbedtls_x509_crl_parse_der(&crl, data.data(), data.size())}; mbedtls_x509_crl_free(&crl);
        if (rc) return result::InvalidCrlFormat;
        std::scoped_lock lock{mutex}; auto id{AllocateIdLocked()}; if (!id) return result::InsufficientMemory; crls.emplace(*id, std::vector<u8>(data.begin(), data.end())); return *id;
    }
    Result SslContextState::RemoveCrl(u64 id) { std::scoped_lock lock{mutex}; return crls.erase(id) ? Result{} : result::PkiNotFound; }
    Result SslContextState::AddPolicyOid(span<const u8> oid) {
        if (oid.empty() || oid.size() > 0xFF) return result::InvalidOption;
        auto end{std::find(oid.begin(), oid.end(), 0)}; if (end == oid.end() || end == oid.begin()) return result::InvalidOption;
        std::string value(reinterpret_cast<const char *>(oid.data()), static_cast<size_t>(end - oid.begin()));
        if (!std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) return result::InvalidOption;
        std::scoped_lock lock{mutex}; if (std::find(policyOids.begin(), policyOids.end(), value) == policyOids.end()) policyOids.push_back(value); return {};
    }
    std::vector<CertificateBlob> SslContextState::CopyServerPki() const { std::scoped_lock lock{mutex}; std::vector<CertificateBlob> out; for (const auto &[id, cert] : serverPki) out.push_back(cert); return out; }
    std::vector<CertificateBlob> SslContextState::CopyDefaultTrustedPki() const { return defaultTrustedPki; }
    std::optional<ClientCertificateBlob> SslContextState::CopyClientPki() const { std::scoped_lock lock{mutex}; return clientPki ? std::optional{clientPki->second} : std::nullopt; }
    std::vector<std::vector<u8>> SslContextState::CopyCrls() const { std::scoped_lock lock{mutex}; std::vector<std::vector<u8>> out; for (const auto &[id, crl] : crls) out.push_back(crl); return out; }
    bool SslContextState::HasPolicyOids() const { std::scoped_lock lock{mutex}; return !policyOids.empty(); }
    Result ValidateSslVersion(SslVersion version) {
        u32 protocols{version.raw & SslVersionProtocolMask}; u32 unknown{version.raw & 0x00FFFFFFU & ~SslVersionProtocolMask};
        if ((protocols == 0 && version.ApiVersion() == 0) || unknown) return result::InvalidOption;
        if ((protocols & SslVersionTls13) && !(protocols & ~SslVersionTls13)) return result::AlertProtocolVersion;
        return {};
    }
}
