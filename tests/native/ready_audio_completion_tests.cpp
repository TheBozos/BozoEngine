// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native ready audio completion assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound ready audio fixture");}
const Entry entries[]={{0xe000,placeholder},{0xe010,placeholder},{0xe020,placeholder}};
const Registration registration("ready-audio",0xa0d10,0xe000,0x400,entries,3);
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xe004;
    check(e.call(fn,0xe004));return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x10000};
    bozo::audio::Mixer *mixer=nullptr;unsigned completions=0;bool excluded=false;
    Fixture(bool ready,bool guard):excluded(guard){
        host.addImage({"ready-audio",0xa0d10,0xe000,0xe000,0x12345678,std::vector<uint8_t>(0x400),
            {{0xe300,"libsd",4},{0xe308,"libsd",26},{0xe310,"libsd",17},
             {0xe318,"libsd",6},{0xe320,"intrman",17},{0xe328,"intrman",18}},{},false});
        if(ready)host.useReadyAudioCompletions();
        mixer=host.advanceTo(0);
        host.bindNative(0xe010,[this](Env &e){
            const auto caller=e.cpu.gpr[31];
            check(completions==0 && e.cpu.gpr[4]==0 && e.cpu.gpr[5]==0x555 && e.cpu.gpr[28]==0x12345678);
            uint8_t bytes[64];mixer->read(0x1000,bytes,64);
            for(unsigned i=0;i<64;++i)check(bytes[i]==uint8_t(i*13));
            ++completions;call(e,0xe318,0); // Reentrant SDK call cannot redeliver this event.
            check(completions==1);e.w32(0xe200,0);e.cpu.pc=caller;
        });
        host.bindNative(0xe020,[](Env &e){e.tick(6);e.cpu.gpr[2]=e.r32(0xe200);e.cpu.pc=e.cpu.gpr[31];});
        host.bindNative(0xe000,[this](Env &e){
            const auto caller=e.cpu.gpr[31];call(e,0xe300);
            call(e,0xe308,0,0xe010,0x555);
            for(unsigned i=0;i<64;++i)e.w8(0xe100+i,uint8_t(i*13));
            e.w32(0xe200,1);e.w32(e.cpu.gpr[29]+16,64);
            if(excluded)check(call(e,0xe320,0xe204)==0);
            check(call(e,0xe310,0,0,0xe100,0x1000)==64 && completions==0);
            if(excluded){
                for(unsigned i=0;i<16;++i)check(call(e,0xe020)==1 && completions==0);
                call(e,0xe328,e.r32(0xe204));check(completions==0);
            }
            unsigned polls=0;
            while(call(e,0xe020)!=0){if(++polls>4000)throw std::runtime_error("reproduced queued completion starvation");}
            check(completions==1 && polls<4);e.cpu.pc=caller;
        });
    }
    void run(){check(host.startImage("ready-audio")==1 && completions==1);
        host.advanceTo(100000);check(completions==1);
        check(host.startImage("ready-audio")==1 && completions==1);
    }
};
}
int main(){try{
    Fixture before(false,false);bool reproduced=false;
    try{before.host.startImage("ready-audio");}catch(const std::runtime_error &e){reproduced=std::strstr(e.what(),"reproduced queued completion starvation");}
    check(reproduced && before.completions==0);
    Fixture first(true,false),second(true,true);first.run();check(second.completions==0);second.run();
    bool rejected=false;try{first.host.useReadyAudioCompletions();}catch(const std::runtime_error &){rejected=true;}check(rejected);
    first.host.reset();first.completions=0;first.run();
    std::puts("native real-upload callback progress/bytes/once/reentrancy/exclusion/reset/isolation passed; default starvation reproduced");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
