// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native user reply assertion failed");}
void placeholder(Env &){throw std::runtime_error("unbound reply fixture");}
const Entry entries[]={{0x14000,placeholder}};
const Registration registration("user-reply",0xeeaa,0x14000,0x400,entries,1);
struct Fixture {
    bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x20000};unsigned replies=0;bool fail=false;
    Fixture(bool reject=false):fail(reject){
        host.addImage({"user-reply",0xeeaa,0x14000,0x14000,0,std::vector<uint8_t>(0x400),
            {{0x14300,"sifcmd",12},{0x14308,"sifcmd",13}},{},false});
        libraries.sif.receiveCommand=[this](uint32_t command,std::span<const uint8_t> data,uint32_t destination,std::span<const uint8_t> extra){
            check(command==3 && data.size()==32 && destination==0x110000 && extra.size()==16);
            uint32_t header[4];std::memcpy(header,data.data(),16);
            check(header[0]==(32|(16<<8)) && header[1]==destination && header[2]==3 && header[3]==0x35353535);
            for(unsigned i=16;i<32;++i)check(data[i]==0x35);
            for(unsigned i=0;i<16;++i)check(extra[i]==uint8_t(i*13));
            if(fail)throw std::runtime_error("rejected real native reply");
            return ++replies;
        };
        host.bindNative(0x14000,[this](Env &e){const auto caller=e.cpu.gpr[31];
            for(unsigned n=0;n<2;++n){
                for(unsigned i=0;i<32;++i)e.w8(0x14100+i,0x35);
                for(unsigned i=0;i<16;++i)e.w8(0x14200+i,uint8_t(i*13));
                e.w32(e.cpu.gpr[29]+16,0x110000);e.w32(e.cpu.gpr[29]+20,16);
                e.cpu.gpr[4]=3;e.cpu.gpr[5]=0x14100;e.cpu.gpr[6]=32;e.cpu.gpr[7]=0x14200;e.cpu.gpr[31]=0x14004;
                if(fail){bool rejected=false;try{e.call(0x14300,0x14004);}catch(const std::runtime_error &){rejected=true;}
                    check(rejected && replies==0 && e.r32(0x14100)==0x35353535);break;}
                check(e.call(0x14300+n*8,0x14004) && e.cpu.gpr[2]==n+1 && e.r32(0x14100)==(32|(16<<8)));
            }
            e.cpu.pc=caller;
        });
    }
};
}
int main(){try{
    Fixture first,second;check(first.host.startImage("user-reply")==1 && first.replies==2 && second.replies==0);
    check(second.host.startImage("user-reply")==1 && second.replies==2);
    first.host.reset();first.replies=0;check(first.host.startImage("user-reply")==1 && first.replies==2);
    Fixture rejected(true);check(rejected.host.startImage("user-reply")==1 && rejected.replies==0);
    std::puts("native reply data/extra/header/job IDs/provider rejection/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
