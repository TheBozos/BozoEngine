// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native frame listener assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound frame fixture");}
const Entry entries[]={{0x11000,placeholder},{0x11010,placeholder},{0x11020,placeholder},{0x11030,placeholder}};
const Registration registration("frame-listener",0xfaaae,0x11000,0x400,entries,4);
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0x11004;
    check(e.call(fn,0x11004));return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x20000};
    std::vector<unsigned> order;bool guard=false,release=false;
    Fixture(bool excluded=false,bool removed=false):guard(excluded),release(removed){
        host.addImage({"frame-listener",0xfaaae,0x11000,0x11000,0x12345678,std::vector<uint8_t>(0x400),
            {{0x11300,"vblank",8},{0x11308,"vblank",9},{0x11310,"intrman",17},{0x11318,"intrman",18}},{},false});
        host.useNativeFrameCallbacks();
        host.bindNative(0x11010,[this](Env &e){check(e.cpu.gpr[4]==1 && e.cpu.gpr[28]==0x12345678);
            order.push_back(1);e.cpu.gpr[2]=0;e.cpu.pc=e.cpu.gpr[31];});
        host.bindNative(0x11020,[this](Env &e){check(e.cpu.gpr[4]==2 && e.cpu.gpr[28]==0x12345678);
            order.push_back(2);e.cpu.gpr[2]=1;e.cpu.pc=e.cpu.gpr[31];});
        host.bindNative(0x11030,[this](Env &e){check(e.cpu.gpr[4]==3 && e.cpu.gpr[28]==0x12345678);
            order.push_back(3);e.cpu.gpr[2]=1;e.cpu.pc=e.cpu.gpr[31];});
        host.bindNative(0x11000,[this](Env &e){const auto caller=e.cpu.gpr[31];
            check(call(e,0x11300,0,20,0x11010,1)==0);
            check(call(e,0x11300,0,10,0x11020,2)==0);
            check(call(e,0x11300,0,10,0x11030,3)==0);
            check(int32_t(call(e,0x11300,0,30,0x11020,2))==-104);
            check(int32_t(call(e,0x11308,0,0x11034))==-105);
            if(guard){call(e,0x11310,0x11200);host.notifyFrameCompleted();host.advanceTo(80000000);check(order.empty());
                if(release)check(call(e,0x11308,0,0x11020)==0);
                call(e,0x11318,e.r32(0x11200));}
            e.cpu.pc=caller;
        });
    }
    void run(){check(host.startImage("frame-listener")==1 && order.empty());
        host.advanceTo(0);if(!guard){check(order.empty());host.advanceTo(80000000);check(order.empty());host.notifyFrameCompleted();}
        check(order== (release?std::vector<unsigned>{3,1}:std::vector<unsigned>{2,3,1}));
        order.clear();host.notifyFrameCompleted();check(order==(release?std::vector<unsigned>{3}:std::vector<unsigned>{2,3}));
    }
};
}
int main(){try{
    for(bool guard:{false,true}){Fixture first(guard),second(guard);first.run();check(second.order.empty());second.run();
        first.host.reset();first.order.clear();first.run();}
    Fixture removed(true,true);removed.run();
    std::puts("native actual-frame-only listeners/priority/FIFO/cancellation/GP/exclusion/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
