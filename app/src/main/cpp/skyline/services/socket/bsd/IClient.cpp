// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>
#include "IClient.h"

namespace skyline::service::socket {
    namespace {
        constexpr u32 GuestSocketCloseOnExecution{1U << 28};
        constexpr u32 GuestSocketNonBlocking{2U << 28};
        constexpr u32 GuestSocketTypeMask{0x0FFFFFFFU};

        constexpr u32 GuestEventFdSemaphore{1U << 0};
        constexpr u32 GuestEventFdNonBlocking{1U << 2};
        constexpr u32 GuestEventFdValidFlags{GuestEventFdSemaphore | GuestEventFdNonBlocking};

        struct BsdSockAddrIn {
            u8 length;
            u8 family;
            u16 port;
            u32 address;
            u8 reserved[8];
        };
        static_assert(sizeof(BsdSockAddrIn) == 0x10);

        bool ToHostAddress(span<u8> buffer, sockaddr_in &address) {
            if (buffer.size() < sizeof(BsdSockAddrIn))
                return false;

            const auto guest{buffer.as<BsdSockAddrIn>()};
            if (guest.family != AF_INET)
                return false;

            address = {};
            address.sin_family = AF_INET;
            address.sin_port = guest.port;
            address.sin_addr.s_addr = guest.address;
            return true;
        }

        BsdSockAddrIn ToGuestAddress(const sockaddr_in &address) {
            BsdSockAddrIn guest{};
            guest.family = static_cast<u8>(address.sin_family);
            guest.port = address.sin_port;
            guest.address = address.sin_addr.s_addr;
            return guest;
        }
    }

    IClient::IClient(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IClient::RegisterClient(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i32>(0);
        return {};
    }

    Result IClient::StartMonitoring(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IClient::Socket(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 domain{request.Pop<i32>()};
        u32 guestType{request.Pop<u32>()};
        i32 protocol{request.Pop<i32>()};

        const u32 guestFlags{guestType & ~GuestSocketTypeMask};
        if (guestFlags & ~(GuestSocketCloseOnExecution | GuestSocketNonBlocking))
            return PushBsdResult(response, -1, EINVAL);

        i32 hostType{static_cast<i32>(guestType & GuestSocketTypeMask)};
        if (guestFlags & GuestSocketCloseOnExecution)
            hostType |= SOCK_CLOEXEC;
        if (guestFlags & GuestSocketNonBlocking)
            hostType |= SOCK_NONBLOCK;

        i32 fd{::socket(domain, hostType, protocol)};
        i32 errorCode{fd < 0 ? errno : 0};
        LOGI("File Descriptor {} with Domain {}, Type {}, Protocol {}", fd, domain, guestType, protocol);
        if (fd < 0)
            LOGE("Error creating socket: {}", strerror(errorCode));
        return PushBsdResult(response, fd, errorCode);
    }

    Result IClient::Poll(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fdsCount{request.Pop<i32>()};
        i32 timeout{request.Pop<i32>()};

        if (fdsCount == 0)
            return PushBsdResult(response, -1, 0);
        if (fdsCount < 0 || timeout < -1 || request.inputBuf.empty() || request.outputBuf.empty())
            return PushBsdResult(response, -1, EINVAL);

        const size_t fdsSize{static_cast<size_t>(fdsCount) * sizeof(pollfd)};
        if (request.inputBuf.at(0).size() < fdsSize || request.outputBuf.at(0).size() < fdsSize)
            return PushBsdResult(response, -1, EINVAL);

        std::vector<pollfd> fds(static_cast<size_t>(fdsCount));
        std::memcpy(fds.data(), request.inputBuf.at(0).data(), fdsSize);
        for (auto &fd : fds)
            fd.revents = 0;

        i32 result{::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout)};
        i32 errorCode{result < 0 ? errno : 0};
        if (result >= 0)
            std::memcpy(request.outputBuf.at(0).data(), fds.data(), fdsSize);

        return PushBsdResult(response, result, errorCode);
    }

    Result IClient::Recv(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 flags{request.Pop<i32>()};

        i32 fileStatus{fcntl(fd, F_GETFL)};
        if (fileStatus < 0)
            return PushBsdResult(response, -1, errno);

        const bool temporaryNonBlocking{!(fileStatus & O_NONBLOCK) && (flags & MSG_EOR)};
        if (temporaryNonBlocking && fcntl(fd, F_SETFL, fileStatus | O_NONBLOCK) < 0)
            return PushBsdResult(response, -1, errno);

        ssize_t result{recv(fd, request.outputBuf.at(0).data(), request.outputBuf.at(0).size(), flags)};
        i32 errorCode{result < 0 ? errno : 0};

        if (temporaryNonBlocking && fcntl(fd, F_SETFL, fileStatus) < 0)
            LOGW("Failed to restore socket flags for fd {}: {}", fd, strerror(errno));

        if (result > std::numeric_limits<i32>::max())
            return PushBsdResult(response, -1, EOVERFLOW);
        return PushBsdResult(response, static_cast<i32>(result), errorCode);
    }

    Result IClient::RecvFrom(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 flags{request.Pop<i32>()};

        i32 fileStatus{fcntl(fd, F_GETFL)};
        if (fileStatus < 0)
            return PushBsdResult(response, -1, errno);

        const bool temporaryNonBlocking{!(fileStatus & O_NONBLOCK) && (flags & MSG_EOR)};
        if (temporaryNonBlocking && fcntl(fd, F_SETFL, fileStatus | O_NONBLOCK) < 0)
            return PushBsdResult(response, -1, errno);

        sockaddr_in hostAddress{};
        socklen_t addressLength{sizeof(hostAddress)};
        ssize_t result{recvfrom(fd, request.outputBuf.at(0).data(), request.outputBuf.at(0).size(), flags,
                                reinterpret_cast<sockaddr *>(&hostAddress), &addressLength)};
        i32 errorCode{result < 0 ? errno : 0};

        if (temporaryNonBlocking && fcntl(fd, F_SETFL, fileStatus) < 0)
            LOGW("Failed to restore socket flags for fd {}: {}", fd, strerror(errno));

        u32 guestAddressLength{};
        if (result >= 0 && request.outputBuf.size() > 1 && !request.outputBuf.at(1).empty()) {
            const auto guestAddress{ToGuestAddress(hostAddress)};
            request.outputBuf.at(1).copy_from(span{guestAddress});
            guestAddressLength = sizeof(guestAddress);
        }

        if (result > std::numeric_limits<i32>::max())
            return PushBsdResult(response, -1, EOVERFLOW);

        response.Push<i32>(errorCode ? -1 : static_cast<i32>(result));
        response.Push<i32>(errorCode);
        response.Push<u32>(guestAddressLength);
        return {};
    }

    Result IClient::Send(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 flags{request.Pop<i32>()};

        ssize_t result{send(fd, request.inputBuf.at(0).data(), request.inputBuf.at(0).size(), flags)};
        return PushBsdResultErrno(response, result);
    }

    Result IClient::SendTo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 flags{request.Pop<i32>()};

        sockaddr_in hostAddress{};
        if (request.inputBuf.size() < 2 || !ToHostAddress(request.inputBuf.at(1), hostAddress))
            return PushBsdResult(response, -1, EAFNOSUPPORT);

        ssize_t result{sendto(fd, request.inputBuf.at(0).data(), request.inputBuf.at(0).size(), flags,
                              reinterpret_cast<sockaddr *>(&hostAddress), sizeof(hostAddress))};
        return PushBsdResultErrno(response, result);
    }

    Result IClient::Accept(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr_in hostAddress{};
        socklen_t addressLength{sizeof(hostAddress)};
        i32 result{accept(fd, reinterpret_cast<sockaddr *>(&hostAddress), &addressLength)};
        i32 errorCode{result < 0 ? errno : 0};

        u32 guestAddressLength{};
        if (result >= 0 && !request.outputBuf.empty() && !request.outputBuf.at(0).empty()) {
            const auto guestAddress{ToGuestAddress(hostAddress)};
            request.outputBuf.at(0).copy_from(span{guestAddress});
            guestAddressLength = sizeof(guestAddress);
        }

        response.Push<i32>(errorCode ? -1 : result);
        response.Push<i32>(errorCode);
        response.Push<u32>(guestAddressLength);
        return {};
    }

    Result IClient::Bind(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr_in hostAddress{};
        if (request.inputBuf.empty() || !ToHostAddress(request.inputBuf.at(0), hostAddress))
            return PushBsdResult(response, -1, EAFNOSUPPORT);

        i32 result{bind(fd, reinterpret_cast<sockaddr *>(&hostAddress), sizeof(hostAddress))};
        return PushBsdResult(response, result, result < 0 ? errno : 0);
    }

    Result IClient::Connect(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr_in hostAddress{};
        if (request.inputBuf.empty() || !ToHostAddress(request.inputBuf.at(0), hostAddress))
            return PushBsdResult(response, -1, EAFNOSUPPORT);

        i32 result{connect(fd, reinterpret_cast<sockaddr *>(&hostAddress), sizeof(hostAddress))};
        return PushBsdResult(response, result, result < 0 ? errno : 0);
    }

    Result IClient::GetPeerName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr_in hostAddress{};
        socklen_t addressLength{sizeof(hostAddress)};
        i32 result{getpeername(fd, reinterpret_cast<sockaddr *>(&hostAddress), &addressLength)};
        i32 errorCode{result < 0 ? errno : 0};

        u32 guestAddressLength{};
        if (result >= 0 && !request.outputBuf.empty() && !request.outputBuf.at(0).empty()) {
            const auto guestAddress{ToGuestAddress(hostAddress)};
            request.outputBuf.at(0).copy_from(span{guestAddress});
            guestAddressLength = sizeof(guestAddress);
        }

        response.Push<i32>(errorCode ? -1 : result);
        response.Push<i32>(errorCode);
        response.Push<u32>(guestAddressLength);
        return {};
    }

    Result IClient::GetSockName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr_in hostAddress{};
        socklen_t addressLength{sizeof(hostAddress)};
        i32 result{getsockname(fd, reinterpret_cast<sockaddr *>(&hostAddress), &addressLength)};
        i32 errorCode{result < 0 ? errno : 0};

        u32 guestAddressLength{};
        if (result >= 0 && !request.outputBuf.empty() && !request.outputBuf.at(0).empty()) {
            const auto guestAddress{ToGuestAddress(hostAddress)};
            request.outputBuf.at(0).copy_from(span{guestAddress});
            guestAddressLength = sizeof(guestAddress);
        }

        response.Push<i32>(errorCode ? -1 : result);
        response.Push<i32>(errorCode);
        response.Push<u32>(guestAddressLength);
        return {};
    }

    Result IClient::GetSockOpt(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 level{request.Pop<i32>()};
        OptionName optionName{request.Pop<OptionName>()};

        if (request.outputBuf.empty())
            return PushBsdResult(response, -1, EINVAL);
        if (level == 0xFFFF)
            level = SOL_SOCKET;

        i32 option{GetOption(optionName)};
        if (option < 0)
            return PushBsdResult(response, -1, EINVAL);

        socklen_t optionLength{static_cast<socklen_t>(request.outputBuf.at(0).size())};
        i32 result{getsockopt(fd, level, option, request.outputBuf.at(0).data(), &optionLength)};
        i32 errorCode{result < 0 ? errno : 0};

        response.Push<i32>(errorCode ? -1 : result);
        response.Push<i32>(errorCode);
        response.Push<u32>(result < 0 ? 0 : static_cast<u32>(optionLength));
        return {};
    }

    Result IClient::Listen(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 backlog{request.Pop<i32>()};
        i32 result{listen(fd, backlog)};
        return PushBsdResult(response, result, result < 0 ? errno : 0);
    }

    Result IClient::Fcntl(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 cmd{request.Pop<i32>()};
        i32 arg{request.Pop<i32>()};
        i32 result{fcntl(fd, cmd, arg)};
        return PushBsdResult(response, result, result < 0 ? errno : 0);
    }

    Result IClient::SetSockOpt(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 level{request.Pop<i32>()};
        OptionName optionName{request.Pop<OptionName>()};

        if (request.inputBuf.empty())
            return PushBsdResult(response, -1, EINVAL);
        if (level == 0xFFFF)
            level = SOL_SOCKET;

        i32 option{GetOption(optionName)};
        if (option < 0)
            return PushBsdResult(response, -1, EINVAL);

        i32 result{setsockopt(fd, level, option, request.inputBuf.at(0).data(),
                              static_cast<socklen_t>(request.inputBuf.at(0).size()))};
        return PushBsdResult(response, result, result < 0 ? errno : 0);
    }

    Result IClient::Shutdown(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 how{request.Pop<i32>()};
        i32 result{shutdown(fd, how)};
        return PushBsdResult(response, result, result < 0 ? errno : 0);
    }

    Result IClient::ShutdownAllSockets(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IClient::Write(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        if (request.inputBuf.empty())
            return PushBsdResult(response, -1, EINVAL);

        ssize_t result{::write(fd, request.inputBuf.at(0).data(), request.inputBuf.at(0).size())};
        return PushBsdResultErrno(response, result);
    }

    Result IClient::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        if (request.outputBuf.empty())
            return PushBsdResult(response, -1, EINVAL);

        ssize_t result{::read(fd, request.outputBuf.at(0).data(), request.outputBuf.at(0).size())};
        return PushBsdResultErrno(response, result);
    }

    Result IClient::Close(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 result{::close(fd)};
        return PushBsdResult(response, result, result < 0 ? errno : 0);
    }

    Result IClient::EventFd(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u64 initialValue{request.Pop<u64>()};
        u32 flags{request.Pop<u32>()};
        request.Skip<u32>();

        if (flags & ~GuestEventFdValidFlags)
            return PushBsdResult(response, -1, EINVAL);

        i32 hostFlags{};
        if (flags & GuestEventFdSemaphore)
            hostFlags |= EFD_SEMAPHORE;
        if (flags & GuestEventFdNonBlocking)
            hostFlags |= EFD_NONBLOCK;

        i32 fd{eventfd(0, hostFlags)};
        if (fd < 0)
            return PushBsdResult(response, -1, errno);

        if (initialValue != 0) {
            ssize_t written{::write(fd, &initialValue, sizeof(initialValue))};
            if (written != sizeof(initialValue)) {
                i32 errorCode{written < 0 ? errno : EIO};
                ::close(fd);
                return PushBsdResult(response, -1, errorCode);
            }
        }

        return PushBsdResult(response, fd, 0);
    }

    Result IClient::PushBsdResult(ipc::IpcResponse &response, i32 result, i32 errorCode) {
        if (errorCode != 0)
            result = -1;

        response.Push<i32>(result);
        response.Push<i32>(errorCode);
        return {};
    }

    Result IClient::PushBsdResultErrno(ipc::IpcResponse &response, i64 result) {
        if (result > std::numeric_limits<i32>::max())
            return PushBsdResult(response, -1, EOVERFLOW);

        return PushBsdResult(response, static_cast<i32>(result), result < 0 ? errno : 0);
    }

    i32 IClient::GetOption(OptionName optionName) {
        switch (optionName) {
            case OptionName::ReuseAddr: return SO_REUSEADDR;
            case OptionName::Broadcast: return SO_BROADCAST;
            case OptionName::Linger: return SO_LINGER;
            case OptionName::SndBuf: return SO_SNDBUF;
            case OptionName::RcvBuf: return SO_RCVBUF;
            case OptionName::SndTimeo: return SO_SNDTIMEO;
            case OptionName::RcvTimeo: return SO_RCVTIMEO;
        }
        return -1;
    }
}
