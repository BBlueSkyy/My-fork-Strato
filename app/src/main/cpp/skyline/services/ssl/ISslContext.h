// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <services/serviceman.h>
#include "state.h"
namespace skyline::service::ssl {
 class ISslContext:public BaseService{
  std::shared_ptr<SslSharedState> sharedState;std::shared_ptr<SslContextState> contextState;
  Result CreateConnectionImpl(type::KSession&,ipc::IpcResponse&,bool);
 public:
  ISslContext(const DeviceState&,ServiceManager&,std::shared_ptr<SslSharedState>,SslVersion,u32,ServicePermission,bool);~ISslContext()override;
  Result SetOption(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result GetOption(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result CreateConnection(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result GetConnectionCount(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result ImportServerPki(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result ImportClientPki(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result RemoveServerPki(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result RemoveClientPki(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result RegisterInternalPki(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result AddPolicyOid(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result ImportCrl(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result RemoveCrl(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result ImportClientCertKeyPki(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result GeneratePrivateKeyAndCert(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);Result CreateConnectionForSystem(type::KSession&,ipc::IpcRequest&,ipc::IpcResponse&);
  SERVICE_DECL(SFUNC(0,ISslContext,SetOption),SFUNC(1,ISslContext,GetOption),SFUNC(2,ISslContext,CreateConnection),SFUNC(3,ISslContext,GetConnectionCount),SFUNC(4,ISslContext,ImportServerPki),SFUNC(5,ISslContext,ImportClientPki),SFUNC(6,ISslContext,RemoveServerPki),SFUNC(7,ISslContext,RemoveClientPki),SFUNC(8,ISslContext,RegisterInternalPki),SFUNC(9,ISslContext,AddPolicyOid),SFUNC(10,ISslContext,ImportCrl),SFUNC(11,ISslContext,RemoveCrl),SFUNC(12,ISslContext,ImportClientCertKeyPki),SFUNC(13,ISslContext,GeneratePrivateKeyAndCert),SFUNC(100,ISslContext,CreateConnectionForSystem))
 };
}
