// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native timer handler assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound native timer fixture");}
const Entry entries[]={{0xf000,placeholder},{0xf010,placeholder}};
const Registration registration("timer-handler",0x71ae,0xf000,0x400,entries,2);
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xf004;
    check(e.call(fn,0xf004));return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};
    unsigned callbacks=0,action=0,source=1,divisor=1;uint32_t id=0;
    Fixture(unsigned mode=0,unsigned clock=1,unsigned scale=1):action(mode),source(clock),divisor(scale){
        host.addImage({"timer-handler",0x71ae,0xf000,0xf000,0x12345678,std::vector<uint8_t>(0x400),
            {{0xf300,"timrman",4},{0xf308,"timrman",20},{0xf310,"timrman",22},
             {0xf318,"timrman",23},{0xf320,"timrman",24},{0xf328,"timrman",6}},{},false});
        host.bindNative(0xf010,[this](Env &e){
            const auto caller=e.cpu.gpr[31];
            check(e.cpu.gpr[4]==0x555 && e.cpu.gpr[28]==0x12345678);++callbacks;
            if(callbacks==2 && action==1)check(call(e,0xf320,id)==0);
            if(callbacks==2 && action==2)check(call(e,0xf328,id)==0);
            e.cpu.gpr[2]=callbacks==1 || action ? 200 : 0;e.cpu.pc=caller;
        });
        host.bindNative(0xf000,[this](Env &e){
            const auto caller=e.cpu.gpr[31];id=call(e,0xf300,source,16,divisor);check(id==1);
            check(int32_t(call(e,0xf318,id))==-155);
            check(call(e,0xf308,id,100,0xf010,0x555)==0);
            check(call(e,0xf310,id,source,0,divisor)==0);
            check(call(e,0xf318,id)==0);
            check(int32_t(call(e,0xf318,id))==-154);
            check(int32_t(call(e,0xf308,id,100,0xf010,0x555))==-156);
            check(int32_t(call(e,0xf310,id,source,0,divisor))==-154);
            e.cpu.pc=caller;
        });
    }
    uint64_t period(unsigned compare){const uint64_t rate=source==4?15734:source==2?13500000:36864000;
        return (uint64_t(compare)*divisor*36864000+rate-1)/rate;}
    void run(){
        host.advanceTo(0);check(host.startImage("timer-handler")==1 && callbacks==0);
        const auto first=period(100),second=first+period(200);
        host.advanceTo((first-1)*8);check(callbacks==0);
        host.advanceTo(first*8);check(callbacks==1);
        host.advanceTo((second-1)*8);check(callbacks==1);
        host.advanceTo(second*8);check(callbacks==2);
        host.advanceTo((second+period(1000))*8);check(callbacks==2);
    }
};
}
int main(){try{
    for(unsigned action:{0u,1u,2u}){Fixture first(action),second(action);first.run();check(second.callbacks==0);second.run();
        first.host.reset();first.callbacks=0;first.run();}
    for(auto clock:{2u,4u}){Fixture other(0,clock,8);other.run();}
    std::puts("native timer deadlines/next-period/stop/free/GP/reset/clock-divisor/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
