// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <iostream>
#include <kernel/ipc.h>
using namespace skyline;
using namespace skyline::kernel::ipc;
int main() {
    alignas(16) std::array<u8,constant::TlsIpcSize> tls{};
    DeviceState state{std::make_shared<kernel::type::KProcess>(), std::make_shared<kernel::type::KThread>()};
    state.thread->tlsRegion=tls.data();
    auto *header=reinterpret_cast<CommandHeader*>(tls.data());
    header->type=CommandType::Request;
    header->rawSize=(constant::IpcPaddingSum+sizeof(PayloadHeader)+8)/4;
    auto *payload=reinterpret_cast<PayloadHeader*>(tls.data()+16);
    payload->magic=util::MakeMagic<u32>("SFCI"); payload->value=1;
    const u64 command=0x1122334455667788;
    std::memcpy(payload+1,&command,8);
    IpcRequest control(false,state);
    assert(control.cmdArgSz==8 && "HIPC raw size must exclude CMIF header and padding");
    assert(control.Pop<u64>()==command);
    tls.fill(0);
    header->type=CommandType::Request;header->wNo=1;
    header->rawSize=(constant::IpcPaddingSum+sizeof(PayloadHeader))/4;
    auto *w=reinterpret_cast<BufferDescriptorABW*>(tls.data()+8);
    w->address0_31=0x10000;w->size0_31=4;
    payload=reinterpret_cast<PayloadHeader*>(tls.data()+32);
    payload->magic=util::MakeMagic<u32>("SFCI");
    IpcRequest exchange(false,state);
    assert(exchange.inputBuf.size()==1 && "W descriptor must expose input");
    assert(exchange.outputBuf.size()==1 && "W descriptor must expose output once");
    assert(exchange.inputBuf[0].data()==exchange.outputBuf[0].data());
    exchange.outputBuf[0][0]=42;
    assert(exchange.inputBuf[0][0]==42);
    tls.fill(0);
    header->type=static_cast<CommandType>(0x10);
    header->rawSize=2;
    std::memcpy(tls.data()+8,&command,8);
    IpcRequest tipc(false,state);
    assert(tipc.isTipc && tipc.cmdArgSz==8 && tipc.Pop<u64>()==command);
    tls.fill(0);
    header->type=CommandType::Request;
    header->rawSize=(constant::IpcPaddingSum+sizeof(DomainHeaderRequest)+sizeof(PayloadHeader)+8+4)/4;
    auto *domain=reinterpret_cast<DomainHeaderRequest*>(tls.data()+16);
    domain->command=DomainCommand::SendMessage;domain->payloadSz=sizeof(PayloadHeader)+8;
    domain->inputCount=1;domain->objectId=3;
    payload=reinterpret_cast<PayloadHeader*>(domain+1);
    payload->magic=util::MakeMagic<u32>("SFCI");payload->value=1;
    std::memcpy(payload+1,&command,8);
    const u32 object=7;std::memcpy(reinterpret_cast<u8*>(payload+1)+8,&object,4);
    IpcRequest domainControl(true,state);
    assert(domainControl.cmdArgSz==8 && domainControl.Pop<u64>()==command);
    assert(domainControl.domainObjects.size()==1 && domainControl.domainObjects[0]==object);
    std::cout<<"HIPC/TIPC/domain argument extents and exchange-buffer directions passed\n";
}
