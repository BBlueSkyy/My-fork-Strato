// SPDX-License-Identifier: MPL-2.0
// Minimal kernel/storage edges; the applet and IApplet queues are production sources.
#pragma once
#include <common.h>
#include <cstring>
#include <mutex>
#include <vector>
namespace skyline {
    class JvmManager;
    struct DeviceState { JvmManager *jvm{}; };
    struct Result {};
    struct AsyncLogger {
        enum class LogLevel { Info, Warning };
        static bool CheckLogLevel(LogLevel) { return true; }
    };
    template<typename... Args> void TestLog(const char *, const Args &...) {}
    namespace kernel::type {
        struct KEvent {
            bool signalled{};
            void Signal() { signalled = true; }
            void ResetSignal() { signalled = false; }
        };
    }
    namespace service {
        struct ServiceManager {};
        class BaseService : public std::enable_shared_from_this<BaseService> {
          protected:
            const DeviceState &state;
            ServiceManager &manager;
          public:
            BaseService(const DeviceState &state, ServiceManager &manager) : state{state}, manager{manager} {}
            virtual ~BaseService() = default;
        };
        namespace am {
            struct StorageSpan : span<u8> {
                using span<u8>::span;
                template<typename T> T &as() { return *reinterpret_cast<T *>(data()); }
            };
            class IStorage {
              protected:
                std::vector<u8> bytes;
              public:
                explicit IStorage(std::vector<u8> bytes) : bytes{std::move(bytes)} {}
                virtual ~IStorage() = default;
                StorageSpan GetSpan() { return StorageSpan{bytes}; }
            };
            class VectorIStorage : public IStorage {
              public:
                VectorIStorage(const DeviceState &, ServiceManager &, std::vector<u8> bytes) : IStorage{std::move(bytes)} {}
            };
            class TransferMemoryIStorage : public VectorIStorage {
              public:
                using VectorIStorage::VectorIStorage;
            };
            template<typename T> class ObjIStorage : public IStorage {
              public:
                ObjIStorage(const DeviceState &, ServiceManager &, const T &value) : IStorage{std::vector<u8>(sizeof(T))} {
                    std::memcpy(bytes.data(), &value, sizeof(T));
                }
            };
        }
    }
}
#define LOGI(...) ::skyline::TestLog(__VA_ARGS__)
#define LOGW(...) ::skyline::TestLog(__VA_ARGS__)
