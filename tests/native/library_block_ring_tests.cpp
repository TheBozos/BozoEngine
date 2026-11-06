// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace bozo::native::irx;
namespace {
void check(bool ok,const char *what){if(!ok)throw std::runtime_error(std::string("block ring: ")+what);}
enum : uint32_t {kSetParam=0xb800,kBlockTrans=0xb808,kStatus=0xb810,kHandler=0xb818,kRing=0x40000,kRingBytes=0x1000,
    kHalfSamples=512,kCycles=768};
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xb004;
    check(e.call(fn,0xb004),"import blocked");return e.cpu.gpr[2];
}
std::vector<uint32_t> bits;unsigned signals=0;
void start(Env &e){
    const auto caller=e.cpu.gpr[31];
    call(e,kHandler,1,0xb010,0x42);
    for(uint32_t core=0;core<2;++core)for(uint32_t side=0;side<2;++side)call(e,kSetParam,(side?0x0A80u:0x0980u)|core,0x3FFF);
    const uint32_t sp=e.cpu.gpr[29];e.w32(sp+16,0);
    check(call(e,kBlockTrans,1,0x10,kRing,kRingBytes)==0,"sceSdBlockTrans");
    e.cpu.pc=caller;
}
void handler(Env &e){
    const auto caller=e.cpu.gpr[31];
    check(e.cpu.gpr[4]==1 && e.cpu.gpr[5]==0x42,"the handler's channel and argument");
    const uint32_t status=call(e,kStatus,1),second=status>>24;
    bits.push_back(second);
    check((status&0xFFFFFFu)==kRing+second*(kRingBytes/2),"the status names the half that followed the signal");
    ++signals;
    const uint32_t refill=kRing+(second?0:kRingBytes/2);
    for(uint32_t i=0;i<kRingBytes/2;i+=2)e.w16(refill+i,uint16_t(signals*2000));
    e.cpu.gpr[2]=0;e.cpu.pc=caller;
}
const Entry entries[]={{0xb000,start},{0xb010,handler}};
const Registration registration("ring",0xb10c,0xb000,0x1000,entries,2);
void run(bool ready){
    bits.clear();signals=0;
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};
    if(ready)host.useReadyAudioCompletions();
    host.addImage({"ring",0xb10c,0xb000,0xb000,0,std::vector<uint8_t>(0x1000),
        {{kSetParam,"libsd",5},{kBlockTrans,"libsd",18},{kStatus,"libsd",20},{kHandler,"libsd",26}},{},false});
    auto *mixer=host.advanceTo(0);
    host.startImage("ring");
    const unsigned halves=6;
    const uint32_t samples=halves*kHalfSamples+4;
    host.advanceTo(uint64_t(samples)*kCycles*8);
    check(bits.size()==halves,"one signal for each half, in one long clock step");
    for(unsigned i=0;i<halves;++i)check(bits[i]==(i%2==0?1u:0u),"the status answers each half in turn");
    std::vector<int16_t> out(size_t(samples)*2);
    const size_t taken=mixer->take(out.data(),samples);
    check(taken>=halves*kHalfSamples,"the clock step made its samples");
    const auto level=[&](unsigned half){return out[size_t(half*kHalfSamples+kHalfSamples/2)*2];};
    check(level(0)==0 && level(1)==0,"the first halves play as they were");
    for(unsigned half=2;half<halves;++half)
        check(level(half)>level(half-1),"each half plays what its signal refilled, never a stale one");
}
}
int main(){try{
    run(true);run(false);
    std::puts("native block ring: one signal per half, status in turn, refills played in order passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
