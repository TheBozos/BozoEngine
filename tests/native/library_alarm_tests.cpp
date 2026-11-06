// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native alarm assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound native alarm fixture");}
const Entry entries[]={{0xf000,placeholder},{0xf010,placeholder}};
const Registration registration("alarm",0xa1a7,0xf000,0x400,entries,2);
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[31]=0xf004;
    check(e.call(fn,0xf004));return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};
    unsigned callbacks=0;bool cancel=false;
    explicit Fixture(bool cancelAtStart):cancel(cancelAtStart){
        host.addImage({"alarm",0xa1a7,0xf000,0xf000,0x12345678,std::vector<uint8_t>(0x400),
            {{0xf300,"thbase",35},{0xf308,"thbase",37},{0xf310,"thbase",39},{0xf318,"sysmem",14}},{},false});
        host.bindNative(0xf010,[this](Env &e){
            const auto caller=e.cpu.gpr[31];
            check(e.cpu.gpr[4]==0x555 && e.cpu.gpr[28]==0x12345678);++callbacks;
            e.cpu.gpr[2]=callbacks==1 ? 200 : 0;e.cpu.pc=caller;
        });
        host.bindNative(0xf000,[this](Env &e){
            const auto caller=e.cpu.gpr[31];
            call(e,0xf318,0xf200);
            call(e,0xf310,1000,0xf100);
            check(e.r32(0xf100)==36864 && e.r32(0xf104)==0);
            check(int32_t(call(e,0xf308,0xf010,0x555))==-105);
            check(call(e,0xf300,0xf100,0xf010,0x555)==0);
            check(int32_t(call(e,0xf300,0xf100,0xf010,0x555))==-104);
            if(cancel){check(call(e,0xf308,0xf010,0x555)==0);check(int32_t(call(e,0xf308,0xf010,0x555))==-105);}
            e.cpu.pc=caller;
        });
    }
    void run(){
        host.advanceTo(0);check(host.startImage("alarm")==1 && callbacks==0);
        const uint64_t first=36864,second=first+200;
        host.advanceTo((first-1)*8);check(callbacks==0);
        host.advanceTo(first*8);check(callbacks==(cancel?0u:1u));
        host.advanceTo(second*8);check(callbacks==(cancel?0u:2u));
        host.advanceTo((second+36864)*8);check(callbacks==(cancel?0u:2u));
    }
};
}
int main(){try{
    Fixture alarm(false),cancelled(false);alarm.run();check(cancelled.callbacks==0);cancelled.run();
    Fixture early(true);early.run();
    std::puts("native alarms: interval, next interval, stop, cancel, duplicate, GP, USec2SysClock, isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
