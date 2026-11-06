// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
using namespace bozo::native::irx;
namespace {
void check(bool ok,const char *what){if(!ok)throw std::runtime_error(std::string("host services: ")+what);}
enum : uint32_t {kMemcmp=0xf800,kSprintf=0xf808,kStrcat=0xf810,kStrcmp=0xf818,kStrcpy=0xf820,kStrncpy=0xf828,
    kRelease=0xf830,kCreate=0xf838,kStart=0xf840,kDelete=0xf848,kExit=0xf850,kTerminate=0xf858,kCancelWakeup=0xf860,
    kSleep=0xf868,kWakeup=0xf870,kFrameWait=0xf878,kAllocTimer=0xf880,kCompare=0xf888,kMode=0xf890,kCounter=0xf898,
    kText=0x30000,kOther=0x30100,kDescriptor=0x30200};
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0,uint32_t ret=0xf004){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=ret;
    check(e.call(fn,ret),"import blocked in a root call");return e.cpu.gpr[2];
}
void put(Env &e,uint32_t at,const char *text){for(size_t i=0;i<=std::strlen(text);++i)e.w8(at+uint32_t(i),uint8_t(text[i]));}
std::string get(Env &e,uint32_t at){std::string s;for(uint8_t c;(c=e.r8(at++));)s+=char(c);return s;}
uint32_t create(Env &e,uint32_t entry){
    const uint32_t d[]={0,0,entry,0x800,32};for(unsigned i=0;i<5;++i)e.w32(kDescriptor+i*4,d[i]);
    return call(e,kCreate,kDescriptor);
}
unsigned exits=0,frames=0;uint32_t sleeper=0,exiter=0,framed=0,timer=0;
void strings(Env &e){
    const auto caller=e.cpu.gpr[31];
    put(e,kText,"abcd");put(e,kOther,"abce");
    check(call(e,kMemcmp,kText,kOther,3)==0,"memcmp equal prefix");
    check(int32_t(call(e,kMemcmp,kText,kOther,4))<0,"memcmp orders by byte");
    check(int32_t(call(e,kStrcmp,kOther,kText))>0,"strcmp orders by byte");
    check(call(e,kStrcmp,kText,kText)==0,"strcmp equal");
    put(e,kOther,"xy");
    check(call(e,kStrcpy,kText+0x10,kOther)==kText+0x10 && get(e,kText+0x10)=="xy","strcpy");
    check(call(e,kStrcat,kText+0x10,kOther)==kText+0x10 && get(e,kText+0x10)=="xyxy","strcat");
    for(uint32_t i=0;i<8;++i)e.w8(kText+0x20+i,0x55);
    check(call(e,kStrncpy,kText+0x20,kOther,5)==kText+0x20,"strncpy result");
    check(get(e,kText+0x20)=="xy" && e.r8(kText+0x24)==0 && e.r8(kText+0x25)==0x55,"strncpy pads to its length only");
    put(e,kOther,"%s=%d %04x %c %5u%% %-3d|");put(e,kText+0x40,"ax");
    const uint32_t sp=e.cpu.gpr[29];
    e.w32(sp+16,0x2f);e.w32(sp+20,'q');e.w32(sp+24,7);e.w32(sp+28,9);
    const auto length=call(e,kSprintf,kText+0x80,kOther,kText+0x40,uint32_t(-5));
    check(get(e,kText+0x80)=="ax=-5 002f q     7% 9  |","sprintf stack arguments and flags");
    check(length==get(e,kText+0x80).size(),"sprintf length");
    for(uint32_t i=0;i<700;++i)e.w8(kText+0x400+i,'z');
    e.w8(kText+0x400+700,0);put(e,kOther,"%s");
    check(call(e,kSprintf,kText+0x800,kOther,kText+0x400)==700 && get(e,kText+0x800).size()==700,"sprintf of a long string");
    check(call(e,kRelease,0x1234)==0,"ReleaseLibraryEntries");
    e.cpu.pc=caller;
}
void threads(Env &e){
    const auto caller=e.cpu.gpr[31];
    const auto dormant=create(e,0xf030);
    check(int32_t(call(e,kDelete,999))==-407,"DeleteThread unknown");
    check(int32_t(call(e,kTerminate,999))==-407,"TerminateThread unknown");
    check(int32_t(call(e,kTerminate,dormant))==-413,"TerminateThread dormant");
    check(call(e,kDelete,dormant)==0,"DeleteThread dormant");
    check(int32_t(call(e,kDelete,dormant))==-407,"DeleteThread twice");
    sleeper=create(e,0xf030);
    call(e,kWakeup,sleeper);call(e,kWakeup,sleeper);
    check(call(e,kCancelWakeup,sleeper)==2,"iCancelWakeupThread returns the count");
    check(call(e,kCancelWakeup,sleeper)==0,"iCancelWakeupThread clears it");
    check(call(e,kStart,sleeper)==0,"StartThread");
    exiter=create(e,0xf040);check(call(e,kStart,exiter)==0,"StartThread exiting");
    framed=create(e,0xf050);check(call(e,kStart,framed)==0,"StartThread frame waiter");
    e.cpu.pc=caller;
}
void sleepBody(Env &e){
    if(e.cpu.pc==0xf030){e.cpu.gpr[31]=0xf038;if(!e.call(kSleep,0xf038))return;}
    throw std::runtime_error("a terminated task resumed");
}
void exitBody(Env &e){
    ++exits;e.cpu.gpr[31]=0xf048;if(!e.call(kExit,0xf048))return;
    throw std::runtime_error("ExitThread returned");
}
void frameBody(Env &e){
    ++frames;e.cpu.pc=0xf050;e.cpu.gpr[31]=0xf050;if(!e.call(kFrameWait,0xf050))return;
}
void lifecycle(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(int32_t(call(e,kDelete,sleeper))==-414,"DeleteThread of a live task");
    check(call(e,kTerminate,sleeper)==0,"TerminateThread of a sleeping task");
    check(int32_t(call(e,kTerminate,sleeper))==-413,"TerminateThread twice");
    check(call(e,kDelete,sleeper)==0,"DeleteThread after TerminateThread");
    check(exits==1,"ExitThread ended its task");
    check(call(e,kStart,exiter)==0,"StartThread of an exited task");
    e.cpu.pc=caller;
}
void timers(Env &e){
    const auto caller=e.cpu.gpr[31];
    timer=call(e,kAllocTimer,1,32,1);
    check(call(e,kCounter,timer)==0,"a stopped counter");
    call(e,kCompare,timer,1000);call(e,kMode,timer,1);
    e.cpu.pc=caller;
}
void counter(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(call(e,kCounter,timer)==400,"GetTimerCounter counts clocks since the period began");
    e.cpu.pc=caller;
}
const Entry entries[]={{0xf000,strings},{0xf030,sleepBody},{0xf038,sleepBody},{0xf040,exitBody},{0xf048,exitBody},
    {0xf050,frameBody}};
const Entry threadEntries[]={{0x9000,threads}},lifeEntries[]={{0x9400,lifecycle}},timerEntries[]={{0x9800,timers}},
    counterEntries[]={{0x9c00,counter}};
const Registration registration("services",0x5e5,0xf000,0x1000,entries,std::size(entries));
const Registration threadRegistration("threads",0x5e6,0x9000,0x100,threadEntries,1);
const Registration lifeRegistration("life",0x5e7,0x9400,0x100,lifeEntries,1);
const Registration timerRegistration("timers",0x5e8,0x9800,0x100,timerEntries,1);
const Registration counterRegistration("counter",0x5e9,0x9c00,0x100,counterEntries,1);
void run(){
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};
    exits=frames=0;
    host.addImage({"services",0x5e5,0xf000,0xf000,0,std::vector<uint8_t>(0x1000),
        {{kMemcmp,"sysclib",11},{kSprintf,"sysclib",19},{kStrcat,"sysclib",20},{kStrcmp,"sysclib",22},
         {kStrcpy,"sysclib",23},{kStrncpy,"sysclib",30},{kRelease,"loadcore",7},{kCreate,"thbase",4},
         {kStart,"thbase",6},{kDelete,"thbase",5},{kExit,"thbase",8},{kTerminate,"thbase",10},
         {kCancelWakeup,"thbase",28},{kSleep,"thbase",24},{kWakeup,"thbase",26},{kFrameWait,"vblank",5},
         {kAllocTimer,"timrman",4},{kCompare,"timrman",11},{kMode,"timrman",7},{kCounter,"timrman",10}},{},false});
    host.addImage({"threads",0x5e6,0x9000,0x9000,0,std::vector<uint8_t>(0x100),{},{},false});
    host.addImage({"life",0x5e7,0x9400,0x9400,0,std::vector<uint8_t>(0x100),{},{},false});
    host.addImage({"timers",0x5e8,0x9800,0x9800,0,std::vector<uint8_t>(0x100),{},{},false});
    host.addImage({"counter",0x5e9,0x9c00,0x9c00,0,std::vector<uint8_t>(0x100),{},{},false});
    host.advanceTo(0);
    host.startImage("services");
    host.startImage("threads");
    check(exits==1,"tasks ran after their start");
    check(frames==1,"the frame waiter blocks after its first pass");
    host.startImage("life");
    host.advanceTo(8);
    check(exits==2,"an exited task restarts from its entry");
    const uint64_t tick=36864000/60;
    host.advanceTo((tick-1)*8);host.advanceTo((tick-1)*8);
    check(frames==1,"no frame before the tick");
    host.advanceTo(tick*8);host.advanceTo(tick*8);
    check(frames==2,"one pass per tick");
    host.advanceTo((tick*3+5)*8);host.advanceTo((tick*3+5)*8);
    check(frames==3,"a late waiter waits for one tick, it does not catch up");
    const uint64_t at=tick*4;
    host.advanceTo(at*8);
    host.startImage("timers");
    host.advanceTo((at+400)*8);
    host.startImage("counter");
}
}
int main(){try{
    run();run();
    std::puts("native host services: sysclib, thread lifecycle, wakeup credits, frame tick, timer counter passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
