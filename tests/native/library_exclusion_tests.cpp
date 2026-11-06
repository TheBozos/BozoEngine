// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native callback exclusion assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound exclusion fixture");}
const Entry entries[]={{0xa000,placeholder},{0xa010,placeholder}};
const Registration registration("exclusion",0xe17,0xa000,0x400,entries,2);
void image(CompiledLibraries &h){h.addImage({"exclusion",0xe17,0xa000,0xa000,0,std::vector<uint8_t>(0x400),
    {{0xa300,"intrman",17},{0xa308,"intrman",18},{0xa310,"timrman",4},
     {0xa318,"intrman",4},{0xa320,"intrman",6},{0xa328,"timrman",11},
     {0xa330,"timrman",7},{0xa338,"timrman",6}},{},false});}
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xa004;
    check(e.call(fn,0xa004));return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};unsigned callbacks=0,mode=0;
    Fixture(){image(host);
        host.bindNative(0xa010,[this](Env &e){const auto caller=e.cpu.gpr[31];++callbacks;call(e,0xa338,1);e.cpu.pc=caller;});
        host.bindNative(0xa000,[this](Env &e){
            const auto caller=e.cpu.gpr[31];
            if(mode==1){call(e,0xa308,1);return;}
            check(call(e,0xa300,0xa200)==0 && e.r32(0xa200)==1);
            if(mode==2){e.cpu.pc=caller;return;}
            check(int32_t(call(e,0xa300,0xa204))==-102 && e.r32(0xa204)==0);
            if(mode==3){call(e,0xa308,1);e.cpu.pc=caller;return;}
            check(call(e,0xa310,0,0,1)==1);call(e,0xa318,17,0,0xa010,0);
            call(e,0xa320,17);call(e,0xa328,1,100);call(e,0xa330,1,1);
            host.advanceTo(0);host.advanceTo(80000);check(callbacks==0);
            call(e,0xa308,0);check(callbacks==0);call(e,0xa308,1);check(callbacks==0);
            e.cpu.pc=caller;
        });
    }
    void run(){check(host.startImage("exclusion")==1 && callbacks==0);host.advanceTo(0);check(callbacks==1);}
};
}
int main(){try{
    Fixture first,second;first.run();check(second.callbacks==0);second.run();
    first.host.reset();first.mode=3;first.callbacks=0;check(first.host.startImage("exclusion")==1);
    for(unsigned mode:{1u,2u}){
        first.host.reset();first.mode=mode;first.callbacks=0;
        bool rejected=false;try{first.host.startImage("exclusion");}catch(const std::runtime_error &){rejected=true;}check(rejected);
        first.host.reset();first.mode=0;first.callbacks=0;first.run();
    }
    std::puts("native nested callback exclusion/deferred timer/release validation/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
