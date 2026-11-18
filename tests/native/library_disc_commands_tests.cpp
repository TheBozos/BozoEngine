// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace bozo::native::irx;
namespace {
void check(bool v,const char *what){if(!v)throw std::runtime_error(std::string("native disc commands: ")+what);}
template<class F>void rejects(F f,const char *message){bool yes=false;try{f();}catch(const std::runtime_error &e){yes=std::strstr(e.what(),message)!=nullptr;}check(yes,message);}
constexpr uint32_t entry=0xe000,callback=0xe100,initCall=0xe800,standbyCall=0xe808,seekCall=0xe810,errorCall=0xe818,
    syncCall=0xe820,typeCall=0xe828,readyCall=0xe830,trayCall=0xe838,stopCall=0xe840,clockCall=0xe848,statusCall=0xe850,
    callbackCall=0xe858,pauseCall=0xe860,breakCall=0xe868,mmodeCall=0xe870,tray=0x30000,clock=0x30010;
void unused(Env &){throw std::runtime_error("unbound fixture entry");}
const Entry entries[]={{entry,unused},{callback,unused}};
const Registration registration("native-disc-commands",1,0xe000,0x1000,entries,2);
uint32_t call(Env &e,uint32_t function,uint32_t a=0,uint32_t b=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[31]=0xe004;
    check(e.call(function,0xe004),"unexpected yield");return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x40000};std::vector<uint32_t> codes;
    explicit Fixture(bool enabled){
        Image image{"native-disc-commands",1,0xe000,entry,0x12340,std::vector<uint8_t>(0x1000)};
        image.imports={{initCall,"cdvdman",4},{standbyCall,"cdvdman",5},{seekCall,"cdvdman",7},{errorCall,"cdvdman",8},
            {syncCall,"cdvdman",11},{typeCall,"cdvdman",12},{readyCall,"cdvdman",13},{trayCall,"cdvdman",14},
            {stopCall,"cdvdman",15},{clockCall,"cdvdman",24},{statusCall,"cdvdman",28},{callbackCall,"cdvdman",37},
            {pauseCall,"cdvdman",38},{breakCall,"cdvdman",39},{mmodeCall,"cdvdman",75}};
        host.addImage(std::move(image));if(enabled)host.useNativeDiscFiles();
        host.bindNative(callback,[this](Env &e){codes.push_back(e.cpu.gpr[4]);e.cpu.pc=e.cpu.gpr[31];});
        host.bindNative(entry,[this](Env &e){const auto ret=e.cpu.gpr[31];
            check(call(e,initCall,0)==1 && call(e,mmodeCall,2)==1,"init and media mode accepted");
            check(call(e,typeCall)==0x14 && call(e,readyCall,0)==2 && call(e,statusCall)==0x0a,"a PS2 DVD, ready and paused");
            e.w32(tray,0x55);check(call(e,trayCall,2,tray)==1 && e.r32(tray)==0,"tray never moved");
            check(call(e,callbackCall,callback)==0,"install native completion");
            check(call(e,seekCall,100)==1 && call(e,standbyCall)==1 && call(e,stopCall)==1 && call(e,pauseCall)==1,"drive commands accepted");
            check(codes==std::vector<uint32_t>({4,5,6,7}),"each command notifies with its function code");
            check(call(e,syncCall,1)==0 && call(e,errorCall)==0 && call(e,breakCall)==1,"nothing outstanding");
            check(call(e,callbackCall,0)==callback && call(e,seekCall,100)==1 && codes.size()==4,"no callback, no notification");
            for(unsigned i=0;i<8;++i)e.w8(clock+i,0xff);
            check(call(e,clockCall,clock)==1 && e.r8(clock)==0 && e.r8(clock+3)==0x12 && e.r8(clock+5)==0x18 &&
                  e.r8(clock+6)==0x06 && e.r8(clock+7)==0x01,"the deterministic clock in BCD");
            e.cpu.gpr[2]=1;e.cpu.pc=ret;
        });
    }
    void run(){check(host.startImage("native-disc-commands")==1,"completed native entry");}
};
}
int main(){try{
    Fixture disabled(false);rejects([&]{disabled.run();},"unsupported native IRX import cdvdman:4");
    Fixture enabled(true);enabled.run();
    std::puts("native disc drive commands: init, mode, type, ready, status, tray, seek/standby/stop/pause notifications, break and clock passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
