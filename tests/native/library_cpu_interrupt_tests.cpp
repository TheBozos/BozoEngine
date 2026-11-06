// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native CpuDisableIntr assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound CpuDisableIntr fixture");}
const Entry entries[]={{0xb000,placeholder}};
const Registration registration("cpu-intr",0xc1,0xb000,0x400,entries,1);
uint32_t call(Env &e,uint32_t fn,uint32_t a=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[31]=0xb004;check(e.call(fn,0xb004));return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};unsigned mode=0;
    Fixture(){
        host.addImage({"cpu-intr",0xc1,0xb000,0xb000,0,std::vector<uint8_t>(0x400),
            {{0xb300,"intrman",8},{0xb308,"intrman",9},{0xb310,"intrman",17},{0xb318,"intrman",18}},{},false});
        host.bindNative(0xb000,[this](Env &e){
            const auto caller=e.cpu.gpr[31];
            check(call(e,0xb308)==0);
            check(call(e,0xb300)==0);
            check(int32_t(call(e,0xb300))==-102);
            check(call(e,0xb308)==0);
            check(call(e,0xb308)==0);
            if(mode==1){check(call(e,0xb310,0xb200)==0);call(e,0xb308);}
            if(mode==2){call(e,0xb300);e.cpu.pc=caller;return;}
            e.cpu.pc=caller;
        });
    }
};
}
int main(){try{
    Fixture plain;check(plain.host.startImage("cpu-intr")==1);
    for(unsigned mode:{1u,2u}){
        Fixture bad;bad.mode=mode;bool rejected=false;
        try{bad.host.startImage("cpu-intr");}catch(const std::runtime_error &){rejected=true;}check(rejected);
    }
    std::puts("native CpuDisableIntr/CpuEnableIntr scope, nested -102, idle enable, mismatch and leak rejection passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
