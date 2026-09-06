// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>
#include "IClient.h"

namespace skyline::service::socket {
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
        i32 type{request.Pop<i32>()};
        i32 protocol{request.Pop<i32>()};
        i32 fd{::socket(domain, type, protocol)};
        LOGI("File Descriptor {} with Domain {}, Type {}, Protocol {}", fd, domain, type, protocol);
        if (fd == -1)
            LOGE("Error creating socket: {}", strerror(errno));
        return PushBsdResult(response, fd, 0);
    }

    Result IClient::Poll(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fdsCount{request.Pop<i32>()};
        i32 timeout{request.Pop<i32>()};

        if (fdsCount < 0 || timeout < -1)
            return PushBsdResult(response, -1, EINVAL);

        if (fdsCount == 0) {
            i32 result{poll(nullptr, 0, timeout)};
            return PushBsdResult(response, result, result == -1 ? errno : 0);
        }

        const auto requiredSize{static_cast<size_t>(fdsCount) * sizeof(pollfd)};
        auto inputBuf{request.inputBuf.at(0)};
        auto outputBuf{request.outputBuf.at(0)};

        if (inputBuf.size() < requiredSize || outputBuf.size() < requiredSize)
            return PushBsdResult(response, -1, EINVAL);

        std::vector<pollfd> fds(static_cast<size_t>(fdsCount));
        std::memcpy(fds.data(), inputBuf.data(), requiredSize);

        i32 result{poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout)};
        const i32 errorCode{result == -1 ? errno : 0};

        if (result >= 0)
            std::memcpy(outputBuf.data(), fds.data(), requiredSize);

        return PushBsdResult(response, result, errorCode);
    }

    Result IClient::Recv(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 flags{request.Pop<i32>()};
        if (fcntl(fd, F_GETFL) == -1)
            return PushBsdResult(response, -1, EBADF);

        bool shouldBlockAfterOperation{false};
        if (!(fcntl(fd, F_GETFL) & O_NONBLOCK) && (flags & MSG_EOR)) {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            shouldBlockAfterOperation = true;
        }

        ssize_t result{recv(fd, request.outputBuf.at(0).data(), request.outputBuf.at(0).size(), flags)};

        if (shouldBlockAfterOperation)
            fcntl(fd, F_SETFL, MSG_EOR);
        return PushBsdResultErrno(response, result);
    }

    Result IClient::RecvFrom(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 flags{request.Pop<i32>()};
        if (fcntl(fd, F_GETFL) == -1)
            return PushBsdResult(response, -1, EBADF);

        bool shouldBlockAfterOperation{false};
        if (!(fcntl(fd, F_GETFL) & O_NONBLOCK) && (flags & MSG_EOR)) {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            shouldBlockAfterOperation = true;
        }

        sockaddr addrIn{};
        socklen_t addrLen{sizeof(addrIn)};
        span message{request.outputBuf.at(0)};
        ssize_t result{recvfrom(fd, message.data(), message.size(), 0, &addrIn, &addrLen)};

        if (shouldBlockAfterOperation)
            fcntl(fd, F_SETFL, MSG_EOR);

        request.outputBuf.at(0).copy_from(message);
        if (!request.outputBuf.at(1).empty())
            request.outputBuf.at(1).copy_from(span{addrIn});
        response.Push(request.outputBuf.at(1).size());
        return PushBsdResultErrno(response, result);
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

        sockaddr addrIn{request.inputBuf.at(1).as<sockaddr>()};
        addrIn.sa_family = AF_INET;
        ssize_t result{sendto(fd, request.inputBuf.at(0).data(), request.inputBuf.at(0).size(), flags,
                          &addrIn, sizeof(addrIn))};
        return PushBsdResultErrno(response, result);
    }

    Result IClient::Accept(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr addr{};
        socklen_t addrLen{sizeof(addr)};
        i32 result{accept(fd, &addr, &addrLen)};
        if (errno != 0)
            return PushBsdResult(response, -1, errno);

        request.outputBuf.at(0).copy_from(span{addr});
        response.Push(request.outputBuf.at(0).size());
        return PushBsdResult(response, result, errno);
    }

    Result IClient::Bind(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr addr{request.inputBuf.at(0).as<sockaddr>()};
        addr.sa_family = AF_INET;

        i32 result{bind(fd, &addr, sizeof(addr))};
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::Connect(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr addr{request.inputBuf.at(0).as<sockaddr>()};
        addr.sa_family = AF_INET;

        i32 result{connect(fd, &addr, sizeof(addr))};
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::GetPeerName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr addr{};
        socklen_t addrLen{sizeof(addr)};
        i32 result{getpeername(fd, &addr, &addrLen)};
        request.outputBuf.at(0).copy_from(span{addr});
        response.Push(request.outputBuf.at(0).size());
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::GetSockName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        sockaddr addr{};
        socklen_t addrLen{sizeof(addr)};
        i32 result{getsockname(fd, &addr, &addrLen)};
        request.outputBuf.at(0).copy_from(span{addr});
        response.Push(request.outputBuf.at(0).size());
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::GetSockOpt(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 level{request.Pop<i32>()};
        OptionName optionName{request.Pop<OptionName>()};
        socklen_t addrLen{sizeof(request.outputBuf.at(0))};
        i32 result{getsockopt(fd, level, GetOption(optionName), request.outputBuf.at(0).data(), &addrLen)};
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::Listen(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 backlog{request.Pop<i32>()};
        i32 result{listen(fd, backlog)};
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::Fcntl(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 cmd{request.Pop<i32>()};
        i32 arg{request.Pop<i32>()};
        i32 result{fcntl(fd, cmd, arg)};
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::SetSockOpt(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 level{request.Pop<i32>()};
        OptionName optionName{request.Pop<OptionName>()};
        if (level == 0xFFFF)
            level = SOL_SOCKET;
        i32 result{setsockopt(fd, level, GetOption(optionName), request.inputBuf.at(0).data(), static_cast<socklen_t>(request.inputBuf.at(0).size()))};
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::Shutdown(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 how{request.Pop<i32>()};
        i32 result{shutdown(fd, how)};
        return PushBsdResult(response, 0, errno);
    }

    Result IClient::ShutdownAllSockets(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IClient::Write(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 flags{request.Pop<i32>()};
        ssize_t result{send(fd, request.inputBuf.at(0).data(), request.inputBuf.at(0).size(), flags)};
        return PushBsdResultErrno(response, result);
    }

    Result IClient::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        ssize_t result{recv(fd, request.outputBuf.at(0).data(), request.outputBuf.at(0).size(), 0)};
        return PushBsdResultErrno(response, result);
    }

    Result IClient::Close(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 fd{request.Pop<i32>()};
        i32 result{close(fd)};
        return PushBsdResult(response, result, result == -1 ? errno : 0);
    }

    Result IClient::EventFd(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u64 initialValue{request.Pop<u64>()};
        u32 flags{request.Pop<u32>()};

        constexpr u32 GuestEfdSemaphore{0x1};
        constexpr u32 GuestEfdNonBlock{0x800};
        constexpr u32 GuestEfdCloseExec{0x80000};
        constexpr u32 SupportedFlags{
            GuestEfdSemaphore | GuestEfdNonBlock | GuestEfdCloseExec
        };

        if ((flags & ~SupportedFlags) != 0)
            return PushBsdResult(response, -1, EINVAL);

        i32 hostFlags{};
        if ((flags & GuestEfdSemaphore) != 0)
            hostFlags |= EFD_SEMAPHORE;
        if ((flags & GuestEfdNonBlock) != 0)
            hostFlags |= EFD_NONBLOCK;
        if ((flags & GuestEfdCloseExec) != 0)
            hostFlags |= EFD_CLOEXEC;

        i32 fd{eventfd(0, hostFlags)};
        if (fd == -1)
            return PushBsdResult(response, -1, errno);

        if (initialValue != 0 && eventfd_write(fd, initialValue) == -1) {
            const i32 errorCode{errno};
            close(fd);
            return PushBsdResult(response, -1, errorCode);
        }

        return PushBsdResult(response, fd, 0);
    }

    Result IClient::PushBsdResult(ipc::IpcResponse &response, i32 result, i32 errorCode) {
        if (errorCode != 0)
            result = -1;

        response.Push(result);
        response.Push(errorCode);
        return {};
    }

    Result IClient::PushBsdResultErrno(ipc::IpcResponse &response, i64 result) {
        response.Push(result);
        response.Push(result == -1 ? errno : 0);
        return {};
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
    }
}
