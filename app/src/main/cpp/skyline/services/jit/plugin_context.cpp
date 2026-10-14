// SPDX-License-Identifier: MPL-2.0
#include "plugin_context.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace skyline::service::jit {
    class PluginContext::Impl : public Dynarmic::A64::UserCallbacks {
      public:
        enum HelperId : std::size_t { Stop, Resolve, Copy, Move, Set, Panic, Count };
        struct Mapping {
            std::uint64_t address;
            std::span<std::uint8_t> bytes;
            bool writable, executable;
            mutable std::size_t dirtyBegin{std::numeric_limits<std::size_t>::max()}, dirtyEnd{};
        };
        std::vector<Mapping> mappings;
        std::vector<std::uint8_t> local;
        std::uint64_t base{}, textEnd{}, helperBase{}, stackTop{}, heapStart{}, heap{};
        std::uint64_t threadPointer{};
        std::uint64_t tlsPointer{};
        std::function<bool()> cancelled;
        Dynarmic::ExclusiveMonitor monitor{1};
        std::unique_ptr<Dynarmic::A64::Jit> cpu;
        std::string fault;
        bool svcPending{}, invalidate{};
        std::uint32_t svc{};
        std::uint64_t svcAddress{};

        explicit Impl(std::function<bool()> cancel) : cancelled(std::move(cancel)) {
            Dynarmic::A64::UserConfig config{};
            config.callbacks=this;
            config.global_monitor=&monitor;
            config.processor_id=0;
            config.tpidr_el0=&threadPointer;
            config.tpidrro_el0=&tlsPointer;
            config.cntfrq_el0=1000000000;
            config.check_halt_on_memory_access=true;
            config.hook_isb=true;
            config.hook_data_cache_operations=true;
            config.code_cache_size=16*1024*1024;
            cpu=std::make_unique<Dynarmic::A64::Jit>(config);
        }
        static bool Contains(std::uint64_t start,std::size_t capacity,std::uint64_t address,std::size_t size) {
            return address>=start && address-start<=capacity && size<=capacity-(address-start);
        }
        std::uint8_t *Pointer(std::uint64_t address,std::size_t size,bool write=false,bool execute=false) const {
            if (Contains(base,local.size(),address,size)) {
                // ELF text/ro are immutable during execution. The BSS and IPC heap are RW.
                if (execute && !(Contains(base,textEnd-base,address,size) || Contains(helperBase,Count*8,address,size)))
                    throw std::runtime_error("JIT plugin executed non-code memory");
                if (write && address<writeStart) throw std::runtime_error("JIT plugin wrote read-only image");
                return const_cast<std::uint8_t *>(local.data())+(address-base);
            }
            for (const auto &map:mappings) if (Contains(map.address,map.bytes.size(),address,size)) {
                if ((write&&!map.writable) || (execute&&!map.executable))
                    throw std::runtime_error("JIT plugin shared-memory permission fault");
                if (write) {
                    map.dirtyBegin=std::min(map.dirtyBegin,static_cast<std::size_t>(address-map.address));
                    map.dirtyEnd=std::max(map.dirtyEnd,static_cast<std::size_t>(address-map.address)+size);
                }
                return map.bytes.data()+(address-map.address);
            }
            throw std::runtime_error("JIT plugin accessed unmapped memory at " + std::to_string(address));
        }
        std::uint64_t writeStart{};
        void Fail(std::string reason) {
            if (fault.empty()) fault=std::move(reason);
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined3);
        }
        template<class T> T Read(std::uint64_t address,bool code=false) {
            T value{};
            try { std::memcpy(&value,Pointer(address,sizeof(T),false,code),sizeof(T)); }
            catch(const std::exception &e) { Fail(e.what()); }
            return value;
        }
        template<class T> void Write(std::uint64_t address,const T &value) {
            try { std::memcpy(Pointer(address,sizeof(T),true),&value,sizeof(T)); }
            catch(const std::exception &e) { Fail(e.what()); }
        }
        template<class T> bool Exclusive(std::uint64_t address,T value,T expected) {
            const auto old=Read<T>(address);
            if (old!=expected || !fault.empty()) return false;
            Write(address,value); return fault.empty();
        }
        std::optional<std::uint32_t> MemoryReadCode(std::uint64_t a) override {
            const auto value=Read<std::uint32_t>(a,true);
            return fault.empty() ? std::optional{value} : std::nullopt;
        }
        std::uint8_t MemoryRead8(std::uint64_t a) override { return Read<std::uint8_t>(a); }
        std::uint16_t MemoryRead16(std::uint64_t a) override { return Read<std::uint16_t>(a); }
        std::uint32_t MemoryRead32(std::uint64_t a) override { return Read<std::uint32_t>(a); }
        std::uint64_t MemoryRead64(std::uint64_t a) override { return Read<std::uint64_t>(a); }
        Dynarmic::A64::Vector MemoryRead128(std::uint64_t a) override { return Read<Dynarmic::A64::Vector>(a); }
        void MemoryWrite8(std::uint64_t a,std::uint8_t v) override { Write(a,v); }
        void MemoryWrite16(std::uint64_t a,std::uint16_t v) override { Write(a,v); }
        void MemoryWrite32(std::uint64_t a,std::uint32_t v) override { Write(a,v); }
        void MemoryWrite64(std::uint64_t a,std::uint64_t v) override { Write(a,v); }
        void MemoryWrite128(std::uint64_t a,Dynarmic::A64::Vector v) override { Write(a,v); }
        bool MemoryWriteExclusive8(std::uint64_t a,std::uint8_t v,std::uint8_t e) override { return Exclusive(a,v,e); }
        bool MemoryWriteExclusive16(std::uint64_t a,std::uint16_t v,std::uint16_t e) override { return Exclusive(a,v,e); }
        bool MemoryWriteExclusive32(std::uint64_t a,std::uint32_t v,std::uint32_t e) override { return Exclusive(a,v,e); }
        bool MemoryWriteExclusive64(std::uint64_t a,std::uint64_t v,std::uint64_t e) override { return Exclusive(a,v,e); }
        bool MemoryWriteExclusive128(std::uint64_t a,Dynarmic::A64::Vector v,Dynarmic::A64::Vector e) override { return Exclusive(a,v,e); }
        void InterpreterFallback(std::uint64_t,std::size_t) override { Fail("Unsupported JIT plugin instruction"); }
        void CallSVC(std::uint32_t number) override {
            svc=number; svcAddress=cpu->GetPC()-4; svcPending=true;
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
        void ExceptionRaised(std::uint64_t,Dynarmic::A64::Exception e) override {
            // Architectural hints do not report a successful plugin return or an error.
            switch(e) {
                case Dynarmic::A64::Exception::Yield:
                case Dynarmic::A64::Exception::SendEvent:
                case Dynarmic::A64::Exception::SendEventLocal: return;
                default: Fail("JIT plugin ARM64 exception " + std::to_string(static_cast<int>(e)));
            }
        }
        void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation,std::uint64_t) override { invalidate=true; }
        void InstructionSynchronizationBarrierRaised() override {
            if (invalidate) cpu->HaltExecution(Dynarmic::HaltReason::UserDefined4);
        }
        void DataCacheOperationRaised(Dynarmic::A64::DataCacheOperation op,std::uint64_t address) override {
            if (op==Dynarmic::A64::DataCacheOperation::ZeroByVA) {
                // DCZID_EL0 is 4: 64-byte zero block.
                std::array<std::uint8_t,64> zero{};
                try { std::memcpy(Pointer(address&~63ULL,64,true),zero.data(),zero.size()); }
                catch(const std::exception &e) { Fail(e.what()); }
            }
        }
        void AddTicks(std::uint64_t) override {}
        std::uint64_t GetTicksRemaining() override {
            if (cancelled && cancelled()) { Fail("JIT plugin execution cancelled by process exit"); return 0; }
            // Scheduling quantum only: Call keeps running until RET, a genuine
            // plugin fault, or process exit. There is no compilation timeout.
            return 65536;
        }
        std::uint64_t GetCNTPCT() override {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        static std::uint64_t HelperAddress(std::string_view name,std::uint64_t helperBase) {
            std::size_t id;
            if (name=="_stop") id=Stop;
            else if (name=="_resolve") id=Resolve;
            else if (name=="memcpy") id=Copy;
            else if (name=="memmove") id=Move;
            else if (name=="memset") id=Set;
            else if (name=="_panic" || name=="PanicForPlugin" ||
                     name=="_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i" ||
                     name=="_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i") id=Panic;
            else return 0;
            return helperBase+id*8;
        }
        std::uint64_t Helper(std::string_view name) const { return HelperAddress(name,helperBase); }
        bool DispatchHelper() {
            svcPending=false;
            if (svc || svcAddress<helperBase || svcAddress>=helperBase+Count*8 || (svcAddress-helperBase)%8)
                throw std::runtime_error("JIT plugin issued unsupported SVC");
            const auto id=(svcAddress-helperBase)/8;
            if (id==Stop) return true;
            if (id==Panic) throw std::runtime_error("JIT plugin panicked");
            const auto dest=cpu->GetRegister(0), src=cpu->GetRegister(1), size=cpu->GetRegister(2);
            if (id==Resolve) {
                std::string name;
                auto current=dest;
                while (true) {
                    const auto c=*Pointer(current++,1);
                    if (!c) break;
                    name.push_back(static_cast<char>(c));
                }
                const auto value=Helper(name);
                if (!value) throw std::runtime_error("Unresolved JIT basic symbol: " + name);
                cpu->SetRegister(0,value);
            } else if (id==Set) {
                if (size) std::memset(Pointer(dest,size,true),static_cast<int>(src),size);
                cpu->SetRegister(0,dest);
            } else if (id==Copy || id==Move) {
                if (size) std::memmove(Pointer(dest,size,true),Pointer(src,size),size);
                cpu->SetRegister(0,dest);
            }
            return false;
        }
    };

    PluginContext::PluginContext(std::function<bool()> cancelled) : impl(std::make_unique<Impl>(std::move(cancelled))) {}
    PluginContext::~PluginContext()=default;
    void PluginContext::Load(const PluginImage &image) {
        impl->base=image.Base(); impl->textEnd=image.Base()+image.TextSize();
        impl->local.assign(image.Bytes().begin(),image.Bytes().end());
        // NRO's data offset marks the first mutable page.
        std::uint32_t dataOffset{}; std::memcpy(&dataOffset,image.Bytes().data()+0x30,4);
        impl->writeStart=image.Base()+dataOffset;
        impl->local.resize((impl->local.size()+15)&~std::size_t{15});
        impl->helperBase=image.Base()+impl->local.size();
        const std::array<std::uint32_t,2> helper{0xd4000001,0xd65f03c0};
        for(std::size_t i=0;i<Impl::Count;i++) {
            const auto pos=impl->local.size(); impl->local.resize(pos+sizeof(helper));
            std::memcpy(impl->local.data()+pos,helper.data(),sizeof(helper));
        }
        impl->local.resize(((impl->local.size()+15)&~std::size_t{15})+128*1024);
        impl->stackTop=image.Base()+impl->local.size();
        impl->heapStart=impl->stackTop; impl->heap=impl->heapStart;
        impl->cpu->ClearCache();
    }
    void PluginContext::Map(std::uint64_t address,std::span<std::uint8_t> backing,bool writable,bool executable) {
        if (backing.empty()) return;
        if (address>std::numeric_limits<std::uint64_t>::max()-backing.size() ||
            (address<impl->base+impl->local.size() && impl->base<address+backing.size()))
            throw std::runtime_error("Overlapping JIT plugin mapping");
        for(const auto &map:impl->mappings) if(address<map.address+map.bytes.size() && map.address<address+backing.size())
            throw std::runtime_error("Duplicate JIT plugin mapping");
        impl->mappings.push_back({address,backing,writable,executable});
    }
    void PluginContext::ResetHeap() { impl->heap=impl->heapStart; }
    std::uint64_t PluginContext::Add(const void *data,std::size_t size) {
        if (!size) return 0;
        if (size>std::numeric_limits<std::size_t>::max()-15) throw std::runtime_error("JIT IPC buffer too large");
        const auto rounded=(size+15)&~std::size_t{15};
        if (impl->heap>std::numeric_limits<std::uint64_t>::max()-rounded) throw std::runtime_error("JIT heap overflow");
        const auto address=impl->heap, end=address+rounded;
        for(const auto &map:impl->mappings) if(address<map.address+map.bytes.size() && map.address<end)
            throw std::runtime_error("JIT private heap overlaps shared memory");
        if (end-impl->base>impl->local.size()) impl->local.resize(end-impl->base);
        std::memcpy(impl->local.data()+address-impl->base,data,size);
        impl->heap=end; return address;
    }
    void PluginContext::Get(std::uint64_t address,void *out,std::size_t size) const {
        if (size) std::memcpy(out,impl->Pointer(address,size),size);
    }
    std::uint64_t PluginContext::Helper(std::string_view name) const { return impl->Helper(name); }
    std::uint64_t PluginContext::HelperAddress(std::string_view name,std::uint64_t helperBase) {
        return Impl::HelperAddress(name,helperBase);
    }
    std::pair<std::size_t,std::size_t> PluginContext::TakeWrites(std::uint64_t address) {
        for(auto &map:impl->mappings) if(map.address==address) {
            const auto begin=map.dirtyBegin, end=map.dirtyEnd;
            map.dirtyBegin=std::numeric_limits<std::size_t>::max(); map.dirtyEnd=0;
            return end>begin ? std::pair{begin,end-begin} : std::pair<std::size_t,std::size_t>{0,0};
        }
        return {0,0};
    }
    std::uint64_t PluginContext::Call(std::uint64_t address,std::initializer_list<std::uint64_t> arguments) {
        impl->Pointer(address,4,false,true);
        impl->fault.clear(); impl->svcPending=false;
        impl->cpu->ClearHalt(static_cast<Dynarmic::HaltReason>(~static_cast<std::uint32_t>(Dynarmic::HaltReason::CacheInvalidation)));
        impl->cpu->SetRegisters({}); impl->cpu->SetVectors({});
        impl->cpu->SetPstate(0); impl->cpu->SetFpcr(0); impl->cpu->SetFpsr(0);
        impl->cpu->ClearExclusiveState();
        std::size_t index{};
        const auto stackSize=arguments.size()>8 ? ((arguments.size()-8)*8+15)&~std::size_t{15} : 0;
        if (stackSize>128*1024) throw std::runtime_error("JIT argument stack overflow");
        const auto sp=impl->stackTop-stackSize;
        for(auto arg:arguments) {
            if(index<8) impl->cpu->SetRegister(index,arg);
            else std::memcpy(impl->Pointer(sp+(index-8)*8,8,true),&arg,8);
            ++index;
        }
        impl->cpu->SetSP(sp); impl->cpu->SetRegister(30,impl->Helper("_stop")); impl->cpu->SetPC(address);
        while(true) {
            impl->cpu->Run();
            if(!impl->fault.empty()) throw std::runtime_error(impl->fault);
            if(impl->invalidate) { impl->cpu->ClearCache(); impl->invalidate=false; }
            impl->cpu->ClearHalt(static_cast<Dynarmic::HaltReason>(~static_cast<std::uint32_t>(Dynarmic::HaltReason::CacheInvalidation)));
            if(impl->svcPending && impl->DispatchHelper()) return impl->cpu->GetRegister(0);
        }
    }
}
