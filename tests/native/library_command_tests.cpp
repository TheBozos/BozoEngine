// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native user command assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound command fixture");}
const Entry entries[]={{0x12000,placeholder},{0x12010,placeholder}};
const Registration registration("user-command",0xc0aaad,0x12000,0x400,entries,2);
uint32_t call(Env &e,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t c=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[31]=0x12004;
    check(e.call(fn,0x12004));return e.cpu.gpr[2];
}
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x20000};
    unsigned callbacks=0;bool guard=false;
    Fixture(bool excluded=false):guard(excluded){
        host.addImage({"user-command",0xc0aaad,0x12000,0x12000,0x12345678,std::vector<uint8_t>(0x400),
            {{0x12300,"sifcmd",8},{0x12308,"sifcmd",10},{0x12310,"sifcmd",11},
             {0x12318,"intrman",17},{0x12320,"intrman",18}},{},false});
        host.bindNative(0x12010,[this](Env &e){
            const auto caller=e.cpu.gpr[31];check(e.cpu.gpr[5]==0x555 && e.cpu.gpr[28]==0x12345678);
            for(unsigned i=0;i<64;++i)check(e.r8(e.cpu.gpr[4]+i)==uint8_t(i*13));
            ++callbacks;e.cpu.pc=caller;
        });
        host.bindNative(0x12000,[this](Env &e){const auto caller=e.cpu.gpr[31];
            call(e,0x12300,0x12200,5);call(e,0x12308,3,0x12010,0x555);
            check(e.r32(0x12218)==0x12010 && e.r32(0x1221c)==0x555);
            call(e,0x12310,3);check(e.r32(0x12218)==0 && e.r32(0x1221c)==0);
            call(e,0x12308,3,0x12010,0x555);
            if(guard)call(e,0x12318,0x12240);
            std::vector<uint8_t> bytes(64);for(unsigned i=0;i<64;++i)bytes[i]=uint8_t(i*13);
            check(host.submitCommand(3,bytes)==1);std::fill(bytes.begin(),bytes.end(),0xff);
            if(guard){host.advanceTo(80000000);check(callbacks==0);call(e,0x12320,e.r32(0x12240));}
            e.cpu.pc=caller;
        });
    }
    void run(){check(host.startImage("user-command")==1 && callbacks==0);host.advanceTo(0);check(callbacks==1);
        host.advanceTo(80000000);check(callbacks==1);
        const std::vector<uint8_t> small(15),large(113),valid(64);
        for(unsigned command:{2u,5u,0x80000003u}){bool rejected=false;
            try{host.submitCommand(command,valid);}catch(const std::runtime_error &){rejected=true;}check(rejected);}
        for(const auto *bytes:{&small,&large}){bool rejected=false;
            try{host.submitCommand(3,*bytes);}catch(const std::runtime_error &){rejected=true;}check(rejected);}
        std::vector<uint8_t> bytes(64);for(unsigned i=0;i<64;++i)bytes[i]=uint8_t(i*13);
        check(host.submitCommand(3,bytes)==2);host.advanceTo(80000001);check(callbacks==2);
    }
};
}
int main(){try{
    for(bool guard:{false,true}){Fixture first(guard),second(guard);first.run();check(second.callbacks==0);second.run();
        first.host.reset();first.callbacks=0;first.run();}
    std::puts("native command table/registration/removal/copied bytes/GP/once/bounds/exclusion/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
