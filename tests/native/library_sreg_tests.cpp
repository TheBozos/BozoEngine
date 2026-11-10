// SPDX-License-Identifier: MIT
// sifcmd's software registers on both sides (sceSifGetSreg 6, sceSifSetSreg 7, a module's set-sreg system command),
// sceSifInitCmd/sceSifExitCmd (4/5), cdvdman's constant answers under native disc files (sceCdInit 4, sceCdDiskReady
// 13, sceCdMmode 75), the root call budget, and ready audio completions delivered when an import returns.
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native software register assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound software register fixture");}
const Entry entries[]={{0x14000,placeholder},{0x14010,placeholder},{0x14020,placeholder}};
const Registration registration("sreg",0x5e6,0x14000,0x400,entries,3);
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0x14004;
    check(e.call(fn,0x14004));return e.cpu.gpr[2];
}
Image image(){
    return {"sreg",0x5e6,0x14000,0x14000,0x12345678,std::vector<uint8_t>(0x400),
        {{0x14300,"sifcmd",4},{0x14308,"sifcmd",5},{0x14310,"sifcmd",6},{0x14318,"sifcmd",7},{0x14320,"sifcmd",12},
         {0x14328,"cdvdman",4},{0x14330,"cdvdman",13},{0x14338,"cdvdman",75}},{},false};
}
// Registers, commands and constant answers, all inside the module's start (a root call).
void registers(){
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x20000};
    host.addImage(image());host.useNativeDiscFiles();
    libraries.sif.iopSreg[0x1f]=0x7000; // what the EE side wrote (a port answering its set-sreg)
    host.bindNative(0x14000,[&](Env &e){
        const auto caller=e.cpu.gpr[31];
        check(call(e,0x14300)==0 && call(e,0x14308)==0);
        check(call(e,0x14310,0x1f)==0x7000 && call(e,0x14310,0x3f)==0x7000); // the index wraps at 32
        check(call(e,0x14318,0x1e,0x1234)==0 && call(e,0x14318,0x1e,0x5678)==0x1234 && libraries.sif.iopSreg[0x1e]==0x5678);
        // set-sreg to the EE: index at +0x10, value at +0x14 of a 0x18-byte packet; a nonzero id comes back.
        e.w32(0x14200+0x10,0x1d);e.w32(0x14200+0x14,0xabcd);
        e.cpu.gpr[7]=0;e.w32(e.cpu.gpr[29]+16,0);e.w32(e.cpu.gpr[29]+20,0);
        check(call(e,0x14320,0x80000001u,0x14200,0x18)!=0 && libraries.sif.sreg[0x1d]==0xabcd);
        bool rejected=false;try{call(e,0x14320,0x80000001u,0x14200,0x10);}catch(const std::runtime_error &){rejected=true;}
        check(rejected && libraries.sif.sreg[0x1d]==0xabcd);
        check(call(e,0x14328,0)==1 && call(e,0x14330,0)==2 && call(e,0x14338,2)==1);
        e.cpu.pc=caller;
    });
    check(host.startImage("sreg")==1);
}
// A start that does more work than the default root call allows fails, and succeeds once the budget is raised.
void budget(){
    for(bool raised:{false,true}){
        bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x20000};
        host.addImage(image());if(raised)host.setRootCallBudget(5000000);
        host.bindNative(0x14000,[](Env &e){
            const auto caller=e.cpu.gpr[31];call(e,0x14010);e.cpu.pc=caller;
        });
        host.bindNative(0x14010,[](Env &e){
            for(unsigned i=0;i<3000000;++i)e.tick(1);
            e.cpu.pc=e.expired()?0x14010:e.cpu.gpr[31];
        });
        bool failed=false;try{host.startImage("sreg");}catch(const std::runtime_error &){failed=true;}
        check(failed!=raised);
    }
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x20000};bool rejected=false;
    try{host.setRootCallBudget(0);}catch(const std::runtime_error &){rejected=true;}check(rejected);
}
// A libsd transfer's completion callback: after the import returns with the import-time policy, only after a linked
// function returns with the plain one.
void afterImports(){
    for(bool atImports:{false,true}){
        bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x20000};
        Image i=image();i.imports={{0x14300,"libsd",4},{0x14308,"libsd",26},{0x14310,"libsd",17}};
        host.addImage(i);
        if(atImports)host.useReadyAudioCompletionsAfterImports();else host.useReadyAudioCompletions();
        unsigned completions=0;bool seenAtImport=false;
        host.bindNative(0x14020,[&](Env &e){++completions;e.cpu.pc=e.cpu.gpr[31];});
        host.bindNative(0x14000,[&](Env &e){
            const auto caller=e.cpu.gpr[31];call(e,0x14300);call(e,0x14308,0,0x14020,0);
            for(unsigned b=0;b<64;++b)e.w8(0x14100+b,uint8_t(b));
            e.w32(e.cpu.gpr[29]+16,64);
            check(call(e,0x14310,0,0,0x14100,0x1000)==64);
            seenAtImport=completions==1; // a call-free poll would see the flag here
            e.cpu.pc=caller;
        });
        check(host.startImage("sreg")==1 && seenAtImport==atImports);
        host.advanceTo(100000);check(completions==1);
    }
}
}
int main(){try{
    registers();budget();afterImports();
    std::puts("native sregs both ways/set-sreg/init-exit/cdvdman constants/root budget/import-time audio completions passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
