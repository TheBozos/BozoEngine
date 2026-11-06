// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native allocation view assertion failed");}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::runtime_error &){rejected=true;}check(rejected);}
void consumer(Env &e){const auto caller=e.cpu.gpr[31];e.cpu.gpr[4]=0;e.cpu.gpr[5]=33;e.cpu.gpr[31]=0x13004;
    check(e.call(0x13300,0x13004));const auto allocation=e.cpu.gpr[2];e.w32(0x13200,allocation);e.cpu.pc=caller;}
const Entry entries[]={{0x13000,consumer}};
const Registration registration("allocation",0xa110c,0x13000,0x400,entries,1);
void image(CompiledLibraries &host){host.addImage({"allocation",0xa110c,0x13000,0x13000,0,
    std::vector<uint8_t>(0x400),{{0x13300,"sysmem",4}},{},false});}
void run(CompiledLibraries &host){
    check(host.startImage("allocation")==1);
    // sysmem:4 and the public allocator share the same owned heap.
    auto first=host.allocatedBuffer(0x20000,48);check(first.size()==48);
    std::fill(first.begin(),first.end(),0x5a);
    auto sub=host.allocatedBuffer(0x20007,33);std::fill(sub.begin(),sub.end(),0x77);
    for(unsigned i=0;i<48;++i)check(first[i]==(i>=7 && i<40?0x77:0x5a));
    const auto adjacent=host.allocateBuffer(16);check(adjacent==0x20030);
    rejects([&]{host.allocatedBuffer(0x20000,49);}); // Cannot span adjacent owned blocks.
    rejects([&]{host.allocatedBuffer(0x1ffff,2);});rejects([&]{host.allocatedBuffer(0x13000,1);});
    rejects([&]{host.allocatedBuffer(0x190000,1);});rejects([&]{host.allocatedBuffer(0x40000000,1);});
    rejects([&]{host.allocatedBuffer(0xffffffff,16);});rejects([&]{host.allocatedBuffer(0x20000,0xffffffff);});
    check(host.allocatedBuffer(0x20030,0).empty());
    check(host.freeBuffer(0x20000));rejects([&]{host.allocatedBuffer(0x20000,1);});
    check(host.allocatedBuffer(adjacent,16).size()==16);
}
}
int main(){try{
    bozo::libs::Libraries a,b;CompiledLibraries first(a,0x20000),second(b,0x20000);image(first);image(second);
    run(first);rejects([&]{second.allocatedBuffer(0x20000,1);});run(second);
    first.reset();rejects([&]{first.allocatedBuffer(0x20030,1);});run(first);
    std::puts("native allocated spans/subranges/bytes/adjacent-block rejection/free/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
