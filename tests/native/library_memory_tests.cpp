// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native library memset assertion failed");}
void fill(Env &e){
    const auto caller=e.cpu.gpr[31];
    for(unsigned size:{0u,1u,3u,16u,255u})for(unsigned offset:{0u,1u,7u}){
        for(unsigned i=0;i<264;++i)e.w8(0x7100+i,0x35);
        e.cpu.gpr[4]=0x7100+offset;e.cpu.gpr[5]=0x1234ab;e.cpu.gpr[6]=size;e.cpu.gpr[31]=0x7004;
        check(e.call(0x7380,0x7004) && e.cpu.gpr[2]==0x7100+offset);
        for(unsigned i=0;i<264;++i)check(e.r8(0x7100+i)==(i>=offset && i<offset+size?0xab:0x35));
    }
    e.cpu.pc=caller;
}
void invalid(Env &e){
    e.cpu.gpr[4]=0x1fffff;e.cpu.gpr[5]=1;e.cpu.gpr[6]=2;e.cpu.gpr[31]=0x7404;
    (void)e.call(0x7780,0x7404);throw std::logic_error("out-of-bounds native memset was accepted");
}
const Entry entries[]={{0x7000,fill}},invalidEntries[]={{0x7400,invalid}};
const Registration registration("memset",0x1414,0x7000,0x400,entries,1);
const Registration invalidRegistration("bad-memset",0x1415,0x7400,0x400,invalidEntries,1);
void images(CompiledLibraries &host){
    host.addImage({"memset",0x1414,0x7000,0x7000,0,std::vector<uint8_t>(0x400),{{0x7380,"sysclib",14}},{},false});
    host.addImage({"bad-memset",0x1415,0x7400,0x7400,0,std::vector<uint8_t>(0x400),{{0x7780,"sysclib",14}},{},false});
}
}
int main(){try{
    bozo::libs::Libraries a,b;CompiledLibraries first(a,0x10000),second(b,0x10000);images(first);images(second);
    check(first.startImage("memset")==1 && second.startImage("memset")==1);
    bool rejected=false;try{first.startImage("bad-memset");}catch(const std::runtime_error &){rejected=true;}check(rejected);
    first.reset();check(first.startImage("memset")==1);
    std::puts("native library memset byte truncation/ranges/canaries/bounds/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
