// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native blocking RPC assertion failed");}
void call(Env &e,uint32_t target){e.cpu.gpr[31]=0x8004;check(e.call(target,0x8004));}
void root(Env &e){
    const auto caller=e.cpu.gpr[31];const uint32_t fields[]={0,0,0x8010,0x1000,32};
    for(unsigned i=0;i<5;++i)e.w32(0x8200+i*4,fields[i]);
    e.cpu.gpr[4]=0x8200;call(e,0x8380);check(e.cpu.gpr[2]==1);
    e.cpu.gpr[4]=1;e.cpu.gpr[5]=0;call(e,0x8388);e.cpu.pc=caller;
}
void worker(Env &e){
    e.cpu.gpr[4]=0x8200;e.cpu.gpr[5]=0x1234;e.cpu.gpr[6]=0x8020;e.cpu.gpr[7]=0x8300;
    call(e,0x8390);e.cpu.gpr[31]=0x8014;
    if(!e.call(0x8398,0x8014))return;
    throw std::runtime_error("native RPC worker did not park");
}
// The handler waits 1 ms (DelayThread) between two system-time reads and replies with the clocks that passed.
void handler(Env &e){
    e.w32(0x8250,e.cpu.gpr[31]);
    e.cpu.gpr[4]=0x8240;call(e,0x83a8);
    e.cpu.gpr[4]=1000;e.cpu.gpr[31]=0x8024;
    if(!e.call(0x83a0,0x8024))return;
    e.cpu.pc=0x8024;
}
void handlerResume(Env &e){
    e.cpu.gpr[4]=0x8248;call(e,0x83a8);
    e.w32(0x8300,e.r32(0x8248)-e.r32(0x8240));e.cpu.gpr[2]=0x8300;e.cpu.pc=e.r32(0x8250);
}
const Entry entries[]={{0x8000,root},{0x8010,worker},{0x8020,handler},{0x8024,handlerResume}};
const Registration registration("rpc-blocking",0xb10c,0x8000,0x400,entries,4);
}
int main(){try{
    bozo::libs::Libraries libs;CompiledLibraries host(libs,0x10000);
    host.addImage({"rpc-blocking",0xb10c,0x8000,0x8000,0x12345678,std::vector<uint8_t>(0x400),
        {{0x8380,"thbase",4},{0x8388,"thbase",6},{0x8390,"sifcmd",17},{0x8398,"sifcmd",22},
         {0x83a0,"thbase",33},{0x83a8,"thbase",34}},{},false});
    check(host.startImage("rpc-blocking")==1);
    Runtime runtime;R5900Context cpu{};bozo::libs::Guest guest(runtime,&cpu);
    for(unsigned i=0;i<3;++i){
        libs.sif.servers.at(0x1234)(libs,guest,{0,0x100000,0,0x110000,4});
        check(guest.read32(0x110000)>=36864);
    }
    std::puts("native RPC handler waiting on its server task: delay, reply, re-park passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
