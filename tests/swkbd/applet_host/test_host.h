// SPDX-License-Identifier: MPL-2.0
// Minimal kernel/storage edges; the applet and IApplet queues are production sources.
#pragma once
#include <common.h>
#include <cstring>
#include <mutex>
#include <vector>
namespace skyline {
    class JvmManager;
    namespace kernel::type { class KProcess; struct KSession {}; }
    using KHandle = std::uint32_t;
    struct DeviceState { JvmManager *jvm{}; kernel::type::KProcess *process{}; };
    struct Result {
        u32 raw{};
        constexpr Result() = default;
        constexpr Result(u32 module, u32 description) : raw{module | (description << 9)} {}
    };
    template<typename... Args> void TestLog(const char *, const Args &...) {}
    namespace kernel::type {
        struct KSyncObject { static inline std::mutex syncObjectMutex; };
        struct KEvent {
            KEvent() = default;
            KEvent(const DeviceState &, bool) {}
            bool signalled{};
            void Signal() { signalled = true; }
            void ResetSignal() { signalled = false; }
        };
    }
    namespace kernel::type {
        class KProcess {
            KHandle next{1};
          public:
            KHandle InsertItem(std::shared_ptr<KEvent>) { return next++; }
        };
    }
    namespace service::am { class IStorage; class IndirectLayerRegistry; }
    namespace ipc {
        struct IpcRequest {
            u64 pid{0x1234};
            std::shared_ptr<service::am::IStorage> storage;
            std::vector<u8> data;
            size_t offset{};
            template<typename T> std::shared_ptr<T> PopService(unsigned, kernel::type::KSession &) {
                auto result{std::dynamic_pointer_cast<T>(storage)};
                if (!result) throw exception("fixture storage missing");
                return result;
            }
            template<typename T> T Pop() {
                if (offset + sizeof(T) > data.size()) throw exception("fixture IPC request too short");
                T value{}; std::memcpy(&value, data.data() + offset, sizeof(T)); offset += sizeof(T); return value;
            }
        };
        struct IpcResponse {
            std::vector<KHandle> copyHandles;
            std::vector<u8> data;
            std::shared_ptr<void> object;
            template<typename T> void Push(T value) {
                const auto offset{data.size()}; data.resize(offset + sizeof(T));
                std::memcpy(data.data() + offset, &value, sizeof(T));
            }
        };
    }
    namespace service {
        namespace type = kernel::type;
        struct ServiceManager {
            std::shared_ptr<am::IndirectLayerRegistry> indirectLayers;
            template<typename T> void RegisterService(std::shared_ptr<T> object, type::KSession &, ipc::IpcResponse &response) {
                response.object = std::move(object);
            }
        };
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
