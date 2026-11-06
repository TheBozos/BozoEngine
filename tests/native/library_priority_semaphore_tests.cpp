// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native priority semaphore assertion failed");}
void call(Env &e,uint32_t target){e.cpu.gpr[31]=0xd004;check(e.call(target,0xd004));}
void root(Env &e){
    const auto caller=e.cpu.gpr[31];
    e.w32(0xd180,2);e.w32(0xd184,0);e.w32(0xd188,0);e.w32(0xd18c,1);
    e.cpu.gpr[4]=0xd180;
    bool rejected=false;try{call(e,0xd300);}catch(const std::runtime_error &){rejected=true;}check(rejected);
    e.w32(0xd180,1);e.cpu.gpr[4]=0xd180;call(e,0xd300);check(e.cpu.gpr[2]==1);
    for(unsigned i=0;i<3;++i){
        const uint32_t fields[]={0,0,0xd020,0x800,i==0?40u:10u};
        for(unsigned n=0;n<5;++n)e.w32(0xd1a0+n*4,fields[n]);
        e.cpu.gpr[4]=0xd1a0;call(e,0xd308);check(e.cpu.gpr[2]==i+1);
        e.cpu.gpr[4]=i+1;e.cpu.gpr[5]=i+1;call(e,0xd310);
    }
    e.cpu.pc=caller;
}
void waiter(Env &e){
    auto &c=e.cpu;
    if(c.pc==0xd020){
        c.gpr[16]=c.gpr[4];c.gpr[17]=c.gpr[31];c.gpr[4]=1;c.gpr[31]=0xd080;
        if(!e.call(0xd318,0xd080))return;
    }
    check(c.pc==0xd080 && c.gpr[2]==0);
    const auto count=e.r32(0xd220);check(count<3);e.w32(0xd224+count*4,c.gpr[16]);e.w32(0xd220,count+1);
    c.pc=c.gpr[17];
}
void signal(Env &e){const auto caller=e.cpu.gpr[31];e.cpu.gpr[4]=1;call(e,0xd320);check(e.cpu.gpr[2]==0);e.cpu.pc=caller;}
void reprioritize(Env &e){
    check(e.r32(0xd220)==1 && e.r32(0xd224)==2);
    const auto caller=e.cpu.gpr[31];e.cpu.gpr[4]=1;e.cpu.gpr[5]=5;call(e,0xd328);check(e.cpu.gpr[2]==0);
    e.cpu.gpr[4]=1;call(e,0xd320);e.cpu.pc=caller;
}
void inspect(Env &e){check(e.r32(0xd220)==3 && e.r32(0xd224)==2 && e.r32(0xd228)==1 && e.r32(0xd22c)==3);e.cpu.pc=e.cpu.gpr[31];}
const Entry entries[]={{0xd000,root},{0xd020,waiter},{0xd080,waiter}};
const Entry signals[]={{0xd400,signal}},changes[]={{0xd500,reprioritize}},lasts[]={{0xd600,signal}},checks[]={{0xd700,inspect}};
const Registration mainRegistration("priority-semas",0x901,0xd000,0x400,entries,3);
const Registration signalRegistration("priority-signal",0x902,0xd400,0x100,signals,1);
const Registration changeRegistration("priority-change",0x903,0xd500,0x100,changes,1);
const Registration lastRegistration("priority-last",0x904,0xd600,0x100,lasts,1);
const Registration checkRegistration("priority-check",0x905,0xd700,0x100,checks,1);
void images(CompiledLibraries &h){
    h.addImage({"priority-semas",0x901,0xd000,0xd000,0,std::vector<uint8_t>(0x400),
        {{0xd300,"thsemap",4},{0xd308,"thbase",4},{0xd310,"thbase",6},{0xd318,"thsemap",8},
         {0xd320,"thsemap",6},{0xd328,"thbase",14}},{},false});
    unsigned i=0;for(const char *name:{"priority-signal","priority-change","priority-last","priority-check"}){
        const uint32_t base=0xd400+i*0x100;h.addImage({name,0x902+i,base,base,0,std::vector<uint8_t>(0x100),{},{},false});++i;
    }
}
void run(CompiledLibraries &h){unsigned id=0;for(const char *name:{"priority-semas","priority-signal","priority-change","priority-last","priority-check"})check(h.startImage(name)==int32_t(++id));}
}
int main(){try{
    bozo::libs::Libraries a,b;CompiledLibraries first(a,0x10000),second(b,0x10000);
    images(first);images(second);run(first);run(second);first.reset();run(first);
    std::puts("native priority semaphore blocking/priority grants/FIFO ties/reprioritization/attribute rejection/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
