// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)
#pragma once
#include <common.h>
namespace skyline::service::ssl {
    constexpr u16 ModuleId{123};
    namespace result {
        constexpr Result InternalLogicError{ModuleId, 13};
        constexpr Result InsufficientMemory{ModuleId, 102};
        constexpr Result NoSocket{ModuleId, 103};
        constexpr Result InvalidCertificateBufferSize{ModuleId, 112};
        constexpr Result InvalidSocket{ModuleId, 116};
        constexpr Result InvalidOption{ModuleId, 126};
        constexpr Result InvalidCrlFormat{ModuleId, 129};
        constexpr Result CertificateBufferTooSmall{ModuleId, 202};
        constexpr Result AlreadyInUse{ModuleId, 203};
        constexpr Result WouldBlock{ModuleId, 204};
        constexpr Result Timeout{ModuleId, 205};
        constexpr Result IoAborted{ModuleId, 206};
        constexpr Result NoConnection{ModuleId, 208};
        constexpr Result ConnectionReset{ModuleId, 209};
        constexpr Result ConnectionAborted{ModuleId, 210};
        constexpr Result SocketShutdown{ModuleId, 211};
        constexpr Result NetworkDown{ModuleId, 212};
        constexpr Result PkiNotFound{ModuleId, 214};
        constexpr Result ClientPkiAlreadyRegistered{ModuleId, 215};
        constexpr Result MaximumServerPkiRegistered{ModuleId, 218};
        constexpr Result InvalidCertificateDomain{ModuleId, 301};
        constexpr Result NoCertificate{ModuleId, 302};
        constexpr Result ExpiredCertificate{ModuleId, 303};
        constexpr Result RevokedCertificate{ModuleId, 304};
        constexpr Result UnsupportedCertificate{ModuleId, 305};
        constexpr Result BadCertificate{ModuleId, 307};
        constexpr Result UnknownCa{ModuleId, 308};
        constexpr Result AccessDenied{ModuleId, 309};
        constexpr Result AlertCloseNotify{ModuleId, 1501};
        constexpr Result HandshakeFailure{ModuleId, 1507};
        constexpr Result AlertProtocolVersion{ModuleId, 1520};
    }
    enum class ServicePermission { User, System };
    enum class CertificateFormat : u32 { Pem = 1, Der = 2 };
    enum class ContextOption : u32 { CrlImportDateCheckEnable = 1 };
    enum class InternalPki : u32 { DeviceClientCertDefault = 1 };
    enum class IoMode : u32 { Blocking = 1, NonBlocking = 2 };
    enum class SessionCacheMode : u32 { None = 0, SessionId = 1, SessionTicket = 2 };
    enum class RenegotiationMode : u32 { None = 0, Secure = 1 };
    enum class OptionType : u32 { DoNotCloseSocket = 0, GetServerCertChain = 1, SkipDefaultVerify = 2, EnableAlpn = 3 };
    enum class FlushSessionCacheOption : u32 { SingleHost = 0, AllHosts = 1 };
    enum class DebugOption : u32 { AllowDisableVerifyOption = 0 };
    enum class AlpnProtoState : u32 { NoSupport = 0, Negotiated = 1, NoOverlap = 2, Selected = 3, EarlyValue = 4 };
    enum VerifyOption : u32 {
        VerifyPeerCa = 1U << 0, VerifyHostName = 1U << 1, VerifyDate = 1U << 2,
        VerifyEvCertPartial = 1U << 3, VerifyEvPolicyOid = 1U << 4, VerifyEvCertFingerprint = 1U << 5,
        VerifyAll = VerifyPeerCa | VerifyHostName | VerifyDate | VerifyEvCertPartial | VerifyEvPolicyOid | VerifyEvCertFingerprint,
    };
    enum PollEvent : u32 { PollRead = 1U << 0, PollWrite = 1U << 1, PollExcept = 1U << 2, PollAll = PollRead | PollWrite | PollExcept };
    struct SslVersion {
        u32 raw{};
        [[nodiscard]] u8 ApiVersion() const { return static_cast<u8>(raw >> 24); }
    };
    constexpr u32 SslVersionAuto{1U << 0};
    constexpr u32 SslVersionTls10{1U << 3};
    constexpr u32 SslVersionTls11{1U << 4};
    constexpr u32 SslVersionTls12{1U << 5};
    constexpr u32 SslVersionTls13{1U << 6};
    constexpr u32 SslVersionProtocolMask{SslVersionAuto | SslVersionTls10 | SslVersionTls11 | SslVersionTls12 | SslVersionTls13};
    struct BuiltInCertificateInfo { i32 certificateId; i32 status; u64 certificateSize; u64 certificateOffset; };
    static_assert(sizeof(BuiltInCertificateInfo) == 0x18);
    struct CertStoreHeader { u32 magic; u32 entryCount; };
    static_assert(sizeof(CertStoreHeader) == 0x8);
    struct CertStoreEntry { i32 certificateId; i32 status; u32 certificateSize; u32 certificateOffset; };
    static_assert(sizeof(CertStoreEntry) == 0x10);
    struct ServerCertificateChainHeader { u64 magic; u32 certificateCount; u32 reserved; };
    static_assert(sizeof(ServerCertificateChainHeader) == 0x10);
    struct ServerCertificateChainEntry { u32 certificateSize; u32 certificateOffset; };
    static_assert(sizeof(ServerCertificateChainEntry) == 0x8);
    struct CipherInfo { char cipher[0x40]; char protocolVersion[0x8]; };
    static_assert(sizeof(CipherInfo) == 0x48);
    struct KeyAndCertParams { u32 version; i32 keySize; u64 publicExponent; char commonName[0x40]; u32 commonNameLength; };
    static_assert(sizeof(KeyAndCertParams) == 0x58);
    constexpr size_t MaximumHostnameLength{0xFF};
    constexpr size_t MaximumServerPkiCount{71};
    constexpr size_t MaximumUserConnectionCount{8};
    constexpr size_t MaximumSystemConnectionCount{10};
    constexpr u32 DefaultIoTimeoutMilliseconds{300000};
    constexpr u64 ServerCertificateChainMagic{0x4E4D684374726543ULL};
    constexpr u32 CertStoreMagic{0x546C7373};
    inline bool IsValidCertificateFormat(CertificateFormat format) { return format == CertificateFormat::Pem || format == CertificateFormat::Der; }
}
