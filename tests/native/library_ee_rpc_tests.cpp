// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using namespace bozo::native::irx;
namespace {
void check(bool ok,const char *what){if(!ok)throw std::runtime_error(std::string("EE RPC: ")+what);}
enum : uint32_t {kBind=0xe800,kCall=0xe808,kRegister=0xe810,kRemove=0xe818,kRemoveQueue=0xe820,kClient=0x30000,
    kOther=0x30040,kSend=0x30100,kReceive=0x30200,kServer=0x30300,kQueue=0x30400,kBuffer=0x30500,kService=0x80000704};
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xe004;
    check(e.call(fn,0xe004),"import blocked");return e.cpu.gpr[2];
}
unsigned completions=0;uint32_t completionArgument=0;
void rpcCall(Env &e,uint32_t client,uint32_t command,uint32_t sendBytes,uint32_t receive,uint32_t receiveBytes,uint32_t end,uint32_t argument){
    const uint32_t sp=e.cpu.gpr[29];
    e.w32(sp+16,sendBytes);e.w32(sp+20,receive);e.w32(sp+24,receiveBytes);e.w32(sp+28,end);e.w32(sp+32,argument);
    check(call(e,kCall,client,command,0,kSend)==0,"CallRpc result");
}
void root(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(call(e,kBind,kOther,0x1234,0)==0,"BindRpc of a service nobody serves");
    check(e.r32(kOther+0x24)==0,"an unserved client has no server");
    bool threw=false;
    try{rpcCall(e,kOther,1,0,0,0,0,0);}catch(const std::runtime_error &){threw=true;}
    check(threw,"a call on an unbound client fails explicitly");
    check(call(e,kBind,kClient,kService,0)==0,"BindRpc");
    check(e.r32(kClient+0x24)!=0,"a bound client names its server");
    for(uint32_t i=0;i<8;++i)e.w8(kSend+i,uint8_t(0x10+i));
    e.w32(kReceive+4,0x55555555);
    rpcCall(e,kClient,7,8,kReceive,4,0xe010,0x77);
    check(e.r32(kReceive)==0x12345678,"the reply reached the caller");
    check(e.r32(kReceive+4)==0x55555555,"only the receive size is written");
    check(e.r32(kClient+0x10)==7,"the client keeps its command");
    check(completions==0,"the completion runs after the call, as a callback");
    check(call(e,kRegister,kServer,0x700,0xe020,kBuffer)==kServer,"RegisterRpc");
    e.cpu.pc=caller;
}
void completion(Env &e){++completions;completionArgument=e.cpu.gpr[4];e.cpu.pc=e.cpu.gpr[31];}
void placeholder(Env &){throw std::runtime_error("unexpected RPC handler call");}
void removal(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(call(e,kRemove,kServer,kQueue)==kServer,"RemoveRpc returns its server");
    check(call(e,kRemoveQueue,kQueue)==kQueue,"RemoveRpcQueue returns its queue");
    e.cpu.pc=caller;
}
const Entry entries[]={{0xe000,root},{0xe010,completion},{0xe020,placeholder}},removeEntries[]={{0xd000,removal}};
const Registration registration("eerpc",0xee1,0xe000,0x1000,entries,std::size(entries));
const Registration removeRegistration("remove",0xee2,0xd000,0x100,removeEntries,1);
void run(){
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};
    completions=0;
    unsigned calls=0;uint32_t command=0;std::vector<uint8_t> sent;
    libraries.sif.eeServers[kService]=[&](uint32_t c,std::span<const uint8_t> in,std::span<uint8_t> out){
        ++calls;command=c;sent.assign(in.begin(),in.end());
        const uint32_t reply=0x12345678;check(out.size()==4,"the reply span has the receive size");std::memcpy(out.data(),&reply,4);
    };
    host.addImage({"eerpc",0xee1,0xe000,0xe000,0,std::vector<uint8_t>(0x1000),
        {{kBind,"sifcmd",15},{kCall,"sifcmd",16},{kRegister,"sifcmd",17},{kRemove,"sifcmd",24},{kRemoveQueue,"sifcmd",25}},{},false});
    host.addImage({"remove",0xee2,0xd000,0xd000,0,std::vector<uint8_t>(0x100),{},{},false});
    host.advanceTo(0);
    host.startImage("eerpc");
    check(calls==1 && command==7,"the EE service ran once with the command");
    check(sent==std::vector<uint8_t>({0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17}),"the EE service got the bytes sent");
    host.advanceTo(8);
    check(completions==1 && completionArgument==0x77,"the completion ran once with its argument");
    check(libraries.sif.servers.contains(0x700),"the IOP server is registered");
    host.startImage("remove");
    check(!libraries.sif.servers.contains(0x700),"RemoveRpc removes it");
}
}
int main(){try{
    run();run();
    std::puts("native IOP to EE RPC: bind, call, reply, completion, unbound failure, removal passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
