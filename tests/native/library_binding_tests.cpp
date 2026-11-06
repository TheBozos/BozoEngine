// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native library binding assertion failed");}
void original(Env &){throw std::runtime_error("unreplaced hardware producer");}
void consumer(Env &e){check(e.r32(0x6100)==e.r32(0x6104));e.cpu.pc=e.cpu.gpr[31];}
const Entry entries[]={{0x6000,original},{0x6020,consumer}};
const Registration registration("producer",0x123,0x6000,0x200,entries,2);
void image(CompiledLibraries &h){h.addImage({"producer",0x123,0x6000,0x6000,0,
    std::vector<uint8_t>(0x200),{{0x6180,"sysmem",4}},{},false});}
template<class F> void rejects(F fn){bool rejected=false;try{fn();}catch(const std::runtime_error &){rejected=true;}check(rejected);}
void bind(CompiledLibraries &h,uint32_t &source,unsigned &reads){
    h.bindNative(0x6000,[&](Env &e){
        const auto caller=e.cpu.gpr[31];++reads;
        e.w32(0x6100,source);e.w32(0x6104,source);
        e.cpu.gpr[31]=0x6004;check(e.call(0x6020,0x6004));e.cpu.pc=caller;
    });
}
}
int main(){try{
    bozo::libs::Libraries a,b;CompiledLibraries first(a,0x10000),second(b,0x10000);
    uint32_t inputA=10,inputB=20;unsigned readsA=0,readsB=0;
    rejects([&]{bind(first,inputA,readsA);}); // Linked registration alone is not admission.
    image(first);image(second);
    rejects([&]{first.bindNative(0x6004,original);}); // Interior is not a linked entry.
    rejects([&]{first.bindNative(0x6180,original);}); // An SDK import is not a game algorithm.
    rejects([&]{first.bindNative(0x6000,{});});
    bind(first,inputA,readsA);bind(second,inputB,readsB);
    check(first.startImage("producer")==1 && readsA==1 && readsB==0);
    check(second.startImage("producer")==1 && readsB==1);
    rejects([&]{bind(first,inputA,readsA);}); // No mutation of active libraries.
    inputA=99;first.reset();check(first.startImage("producer")==1 && readsA==2 && readsB==1);
    check(first.startImage("producer")==1 && readsA==2);
    std::puts("per-host native library bindings, compiled consumer, admission, reset and isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
