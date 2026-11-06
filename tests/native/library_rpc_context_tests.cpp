// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native RPC task context assertion failed");}
void call(Env &e,uint32_t target){e.cpu.gpr[31]=0x8004;check(e.call(target,0x8004));}
void root(Env &e){
    const auto caller=e.cpu.gpr[31];const uint32_t fields[]={0,0,0x8010,0x1000,32};
    for(unsigned i=0;i<5;++i)e.w32(0x8200+i*4,fields[i]);
    e.cpu.gpr[4]=0x8200;call(e,0x8380);check(e.cpu.gpr[2]==1);
    e.cpu.gpr[4]=1;e.cpu.gpr[5]=0;call(e,0x8388);e.cpu.pc=caller;
}
void worker(Env &e){
    e.w32(0x8290,e.r32(0x8290)+1);
    e.cpu.gpr[4]=0x8200;e.cpu.gpr[5]=0x1234;e.cpu.gpr[6]=0x8020;e.cpu.gpr[7]=0x8300;
    call(e,0x8390);e.cpu.gpr[31]=0x8014;
    if(!e.call(0x8398,0x8014))return;
    throw std::runtime_error("native RPC worker did not park");
}
void handler(Env &e){
    const auto caller=e.cpu.gpr[31],command=e.cpu.gpr[4];
    check(e.cpu.gpr[28]==0x12345678);call(e,0x83a0);check(e.cpu.gpr[2]==1);
    call(e,0x83a8);check(e.cpu.gpr[2]==32);
    if(command==1)throw std::runtime_error("intentional native RPC failure");
    e.cpu.gpr[4]=0;e.cpu.gpr[5]=27;call(e,0x83b0);check(e.cpu.gpr[2]==0);
    call(e,0x83a8);check(e.cpu.gpr[2]==27);
    e.cpu.gpr[4]=0;e.cpu.gpr[5]=0x8240;call(e,0x83b8);
    check(e.r32(0x8248)==1 && e.r32(0x825c)==32 && e.r32(0x8260)==27);
    e.cpu.gpr[4]=1;e.cpu.gpr[5]=32;call(e,0x83b0);check(e.cpu.gpr[2]==0);
    check(e.r32(0x8290)==1);e.w32(0x8300,32);e.cpu.gpr[2]=0x8300;e.cpu.pc=caller;
}
void rootProbe(Env &e){
    const auto caller=e.cpu.gpr[31];call(e,0x83a0);check(e.cpu.gpr[2]==0);
    call(e,0x83a8);check(int32_t(e.cpu.gpr[2])==-100);e.cpu.pc=caller;
}
const Entry entries[]={{0x8000,root},{0x8010,worker},{0x8020,handler}};
const Entry probe[]={{0x8400,rootProbe}};
const Registration registration("rpc-context",0xc017,0x8000,0x400,entries,3);
const Registration probeRegistration("root-context",0xc018,0x8400,0x100,probe,1);
void images(CompiledLibraries &host){
    host.addImage({"rpc-context",0xc017,0x8000,0x8000,0x12345678,std::vector<uint8_t>(0x400),
        {{0x8380,"thbase",4},{0x8388,"thbase",6},{0x8390,"sifcmd",17},{0x8398,"sifcmd",22},
         {0x83a0,"thbase",20},{0x83a8,"thbase",42},{0x83b0,"thbase",14},{0x83b8,"thbase",22}},{},false});
    host.addImage({"root-context",0xc018,0x8400,0x8400,0,std::vector<uint8_t>(0x100),{},{},false});
}
void run(CompiledLibraries &host,bozo::libs::Libraries &libs){
    check(host.startImage("rpc-context")==1);
    Runtime runtime;R5900Context cpu{};bozo::libs::Guest guest(runtime,&cpu);
    for(unsigned i=0;i<3;++i){
        libs.sif.servers.at(0x1234)(libs,guest,{0,0x100000,0,0x110000,4});check(guest.read32(0x110000)==32);
    }
    bool rejected=false;try{libs.sif.servers.at(0x1234)(libs,guest,{1,0x100000,0,0x110000,4});}
    catch(const std::runtime_error &){rejected=true;}check(rejected);
    check(host.startImage("root-context")==2);
    libs.sif.servers.at(0x1234)(libs,guest,{0,0x100000,0,0x110000,4});
}
}
int main(){try{
    bozo::libs::Libraries a,b;CompiledLibraries first(a,0x10000),second(b,0x10000);
    images(first);images(second);run(first,a);check(b.sif.servers.empty());run(second,b);
    first.reset();check(a.sif.servers.empty() && !b.sif.servers.empty());run(first,a);
    std::puts("native RPC owner identity/priority/status/park/context restoration/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
