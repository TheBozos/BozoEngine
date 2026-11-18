// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace bozo::native::irx;
namespace {
void check(bool ok,const char *what){if(!ok)throw std::runtime_error(std::string("event flags: ")+what);}
enum : uint32_t {kCreateFlag=0xf800,kDeleteFlag=0xf808,kSetFlag=0xf810,kISetFlag=0xf818,kClearFlag=0xf820,
    kIClearFlag=0xf828,kWaitFlag=0xf830,kPollFlag=0xf838,kReferFlag=0xf840,kIReferFlag=0xf848,kCreateThread=0xf850,
    kStartThread=0xf858,kExitThread=0xf860,kQueryIntr=0xf868,kBzero=0xf870,kCreateSema=0xf878,kReferSema=0xf880,
    kWaitSema=0xf888,kDescriptor=0x30000,kFlagDescriptor=0x30100,kInfo=0x30200,kResult=0x30300,kBytes=0x30400};
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0,uint32_t ret=0xf004){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=ret;
    check(e.call(fn,ret),"import blocked in a root call");return e.cpu.gpr[2];
}
uint32_t createTask(Env &e,uint32_t entry){
    const uint32_t d[]={0,0,entry,0x800,32};for(unsigned i=0;i<5;++i)e.w32(kDescriptor+i*4,d[i]);
    return call(e,kCreateThread,kDescriptor);
}
uint32_t createFlag(Env &e,uint32_t attr,uint32_t bits){
    e.w32(kFlagDescriptor,attr);e.w32(kFlagDescriptor+4,0x77);e.w32(kFlagDescriptor+8,bits);
    return call(e,kCreateFlag,kFlagDescriptor);
}
uint32_t andFlag=0,orFlag=0,goneFlag=0,sema=0;
int32_t andResult=1,orResult=1,goneResult=1,taskContext=-1,semaResult=1;
uint32_t andBits=0,orBits=0;
unsigned andWakes=0,orWakes=0,goneWakes=0;
void andWaiter(Env &e){
    if(e.cpu.pc==0xf030){
        taskContext=int32_t(call(e,kQueryIntr));
        e.cpu.gpr[4]=andFlag;e.cpu.gpr[5]=0x5;e.cpu.gpr[6]=0;e.cpu.gpr[7]=kResult;e.cpu.gpr[31]=0xf038;
        if(!e.call(kWaitFlag,0xf038))return;
    }
    ++andWakes;andResult=int32_t(e.cpu.gpr[2]);andBits=e.r32(kResult);
    e.cpu.gpr[31]=0xf03c;e.call(kExitThread,0xf03c);
}
void orWaiter(Env &e){
    if(e.cpu.pc==0xf040){
        e.cpu.gpr[4]=orFlag;e.cpu.gpr[5]=0x30;e.cpu.gpr[6]=1;e.cpu.gpr[7]=kResult+4;e.cpu.gpr[31]=0xf048;
        if(!e.call(kWaitFlag,0xf048))return;
    }
    ++orWakes;orResult=int32_t(e.cpu.gpr[2]);orBits=e.r32(kResult+4);
    e.cpu.gpr[31]=0xf04c;e.call(kExitThread,0xf04c);
}
void goneWaiter(Env &e){
    if(e.cpu.pc==0xf050){
        e.cpu.gpr[4]=goneFlag;e.cpu.gpr[5]=0x1;e.cpu.gpr[6]=0;e.cpu.gpr[7]=0;e.cpu.gpr[31]=0xf058;
        if(!e.call(kWaitFlag,0xf058))return;
    }
    ++goneWakes;goneResult=int32_t(e.cpu.gpr[2]);
    e.cpu.gpr[31]=0xf05c;e.call(kExitThread,0xf05c);
}
void semaWaiter(Env &e){
    if(e.cpu.pc==0xf060){
        e.cpu.gpr[4]=sema;e.cpu.gpr[31]=0xf068;
        if(!e.call(kWaitSema,0xf068))return;
    }
    semaResult=int32_t(e.cpu.gpr[2]);
    e.cpu.gpr[31]=0xf06c;e.call(kExitThread,0xf06c);
}
void setUp(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(call(e,kQueryIntr)==1,"a root call runs outside any task");
    andFlag=createFlag(e,0,0x1);orFlag=createFlag(e,2,0);goneFlag=createFlag(e,0,0);
    check(andFlag && orFlag && goneFlag && andFlag!=orFlag,"CreateEventFlag ids");
    check(int32_t(call(e,kSetFlag,999,1))==-409,"SetEventFlag of an unknown flag");
    check(int32_t(call(e,kWaitFlag,andFlag,0,0,0))==-423,"WaitEventFlag of no bits");
    check(int32_t(call(e,kPollFlag,andFlag,0x5,0,kResult))==-421,"PollEventFlag unmet");
    check(call(e,kPollFlag,andFlag,0x1,0,kResult)==0 && e.r32(kResult)==0x1,"PollEventFlag met writes the bits");
    check(call(e,kReferFlag,andFlag,kInfo)==0,"ReferEventFlagStatus");
    check(e.r32(kInfo)==0 && e.r32(kInfo+4)==0x77 && e.r32(kInfo+8)==0x1 && e.r32(kInfo+12)==0x1 && e.r32(kInfo+16)==0,
          "ReferEventFlagStatus fields");
    const uint32_t semaDescriptor[]={0,0,0,1};for(unsigned i=0;i<4;++i)e.w32(kDescriptor+0x40+i*4,semaDescriptor[i]);
    sema=call(e,kCreateSema,kDescriptor+0x40);
    for(uint32_t i=0;i<8;++i)e.w8(kBytes+i,0xaa);
    call(e,kBzero,kBytes+2,4);
    check(e.r8(kBytes+1)==0xaa && e.r8(kBytes+2)==0 && e.r8(kBytes+5)==0 && e.r8(kBytes+6)==0xaa,"bzero clears its range only");
    call(e,kStartThread,createTask(e,0xf030));
    call(e,kStartThread,createTask(e,0xf040));
    call(e,kStartThread,createTask(e,0xf050));
    call(e,kStartThread,createTask(e,0xf060));
    e.cpu.pc=caller;
}
void waiting(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(taskContext==0,"a task runs in thread context");
    check(andWakes==0 && orWakes==0 && goneWakes==0,"waiters wait for their bits");
    check(call(e,kIReferFlag,andFlag,kInfo)==0 && e.r32(kInfo+16)==1,"a waiter is counted");
    check(int32_t(call(e,kPollFlag,andFlag,0x4,0,0))==-422,"a single-waiter flag refuses a second waiter");
    check(call(e,kReferSema,sema,kInfo)==0,"ReferSemaStatus");
    check(e.r32(kInfo+8)==0 && e.r32(kInfo+12)==1 && e.r32(kInfo+16)==0 && e.r32(kInfo+20)==1,"ReferSemaStatus fields");
    check(int32_t(call(e,kReferSema,999,kInfo))==-408,"ReferSemaStatus of an unknown semaphore");
    call(e,kISetFlag,andFlag,0x4);
    call(e,kSetFlag,orFlag,0x20);
    call(e,kDeleteFlag,goneFlag);
    check(int32_t(call(e,kSetFlag,goneFlag,1))==-409,"a deleted flag is gone");
    e.cpu.pc=caller;
}
void woken(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(andWakes==1 && andResult==0 && andBits==0x5,"an AND waiter wakes when all its bits are set, with them");
    check(orWakes==1 && orResult==0 && orBits==0x20,"an OR waiter wakes on any of its bits");
    check(goneWakes==1 && goneResult==-425,"DeleteEventFlag releases its waiters");
    call(e,kClearFlag,andFlag,0x4);
    check(call(e,kReferFlag,andFlag,kInfo)==0 && e.r32(kInfo+12)==0x4 && e.r32(kInfo+16)==0,"ClearEventFlag keeps the given bits");
    call(e,kIClearFlag,andFlag,0);
    check(call(e,kReferFlag,andFlag,kInfo)==0 && e.r32(kInfo+12)==0,"iClearEventFlag");
    e.cpu.pc=caller;
}
void start(Env &e){e.cpu.gpr[2]=0;e.cpu.pc=e.cpu.gpr[31];}
const Entry entries[]={{0xf000,start},{0xf030,andWaiter},{0xf038,andWaiter},{0xf040,orWaiter},{0xf048,orWaiter},{0xf050,goneWaiter},
    {0xf058,goneWaiter},{0xf060,semaWaiter},{0xf068,semaWaiter}};
const Entry setUpEntries[]={{0x9000,setUp}},waitingEntries[]={{0x9400,waiting}},wokenEntries[]={{0x9800,woken}};
const Registration registration("flags",0x6e1,0xf000,0x1000,entries,std::size(entries));
const Registration setUpRegistration("setup",0x6e2,0x9000,0x100,setUpEntries,1);
const Registration waitingRegistration("waiting",0x6e3,0x9400,0x100,waitingEntries,1);
const Registration wokenRegistration("woken",0x6e4,0x9800,0x100,wokenEntries,1);
void run(){
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};
    andResult=orResult=goneResult=semaResult=1;taskContext=-1;andBits=orBits=0;andWakes=orWakes=goneWakes=0;
    host.addImage({"flags",0x6e1,0xf000,0xf000,0,std::vector<uint8_t>(0x1000),
        {{kCreateFlag,"thevent",4},{kDeleteFlag,"thevent",5},{kSetFlag,"thevent",6},{kISetFlag,"thevent",7},
         {kClearFlag,"thevent",8},{kIClearFlag,"thevent",9},{kWaitFlag,"thevent",10},{kPollFlag,"thevent",11},
         {kReferFlag,"thevent",13},{kIReferFlag,"thevent",14},{kCreateThread,"thbase",4},{kStartThread,"thbase",6},
         {kExitThread,"thbase",8},{kQueryIntr,"intrman",23},{kBzero,"sysclib",17},{kCreateSema,"thsemap",4},
         {kReferSema,"thsemap",11},{kWaitSema,"thsemap",8}},{},false});
    host.addImage({"setup",0x6e2,0x9000,0x9000,0,std::vector<uint8_t>(0x100),{},{},false});
    host.addImage({"waiting",0x6e3,0x9400,0x9400,0,std::vector<uint8_t>(0x100),{},{},false});
    host.addImage({"woken",0x6e4,0x9800,0x9800,0,std::vector<uint8_t>(0x100),{},{},false});
    host.advanceTo(0);
    host.startImage("flags");
    host.startImage("setup");
    host.startImage("waiting");
    host.startImage("woken");
    check(semaResult==1,"a semaphore waiter stays parked");
}
}
int main(){try{
    run();run();
    std::puts("native event flags: create, set, clear, AND and OR waits, poll, refer, delete; QueryIntrContext, bzero, ReferSemaStatus passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
