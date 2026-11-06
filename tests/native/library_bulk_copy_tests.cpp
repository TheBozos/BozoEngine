// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include "bozo/ee/dispatcher.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native library bulk-copy assertion failed");}
struct TestHost:bozo::ee::Host {
    bozo::libs::Libraries libraries;
    void libraryCall(bozo::ee::Dispatcher &,R5900Context *,uint32_t)override{throw std::runtime_error("unexpected EE library call");}
    void vu0Microprogram(bozo::ee::Dispatcher &,R5900Context *,uint32_t)override{throw std::runtime_error("unexpected VU");}
    void ioRead(bozo::ee::Dispatcher &,R5900Context *,uint32_t,uint32_t,uint64_t[2])override{throw std::runtime_error("unexpected device read");}
    void ioWrite(bozo::ee::Dispatcher &,R5900Context *,uint32_t,uint32_t,const uint64_t[2])override{throw std::runtime_error("unexpected device write");}
    void fault(bozo::ee::Dispatcher &,R5900Context *,const std::string &s)override{throw std::runtime_error(s);}
};
void placeholder(Env &){throw std::runtime_error("unbound native bulk fixture");}
const Entry entries[]={{0xc000,placeholder},{0xc010,placeholder}};
const Registration registration("bulk-copy",0xb017,0xc000,0x400,entries,2);
uint32_t call(Env &e,uint32_t target,uint32_t a,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xc004;
    check(e.call(target,0xc004));return e.cpu.gpr[2];
}
void descriptor(Env &e,uint32_t at,uint32_t source,uint32_t destination,uint32_t bytes,uint32_t flags=0){
    e.w32(at,source);e.w32(at+4,destination);e.w32(at+8,bytes);e.w32(at+12,flags);
}
struct Fixture {
    TestHost eeHost;bozo::ee::Dispatcher dispatcher{eeHost};CompiledLibraries host{eeHost.libraries,0x10000};
    unsigned completions=0;
    Fixture(){
        eeHost.libraries.attach(dispatcher);
        host.addImage({"bulk-copy",0xb017,0xc000,0xc000,0x12345678,std::vector<uint8_t>(0x400),
            {{0xc300,"sifman",7},{0xc308,"sifman",8},{0xc310,"sifman",32}},{},false});
        host.bindNative(0xc010,[this](Env &e){
            check(e.cpu.gpr[4]==0x88 && e.cpu.gpr[28]==0x12345678 && completions==0);
            check(dispatcher.runtime().memory.ram()[0x100000]==0xa0);++completions;e.cpu.pc=e.cpu.gpr[31];
        });
        host.bindNative(0xc000,[this](Env &e){
            const auto caller=e.cpu.gpr[31];auto *ram=dispatcher.runtime().memory.ram();
            std::fill_n(ram+0x100000,64,0x5a);
            for(unsigned i=0;i<32;++i)e.w8(0xc100+i,uint8_t(i+1));
            for(unsigned i=0;i<16;++i)e.w8(0xc120+i,uint8_t(0xa0+i));
            descriptor(e,0xc200,0xc100,0x100000,16);descriptor(e,0xc210,0xc120,0x100008,8);
            check(call(e,0xc300,0xc200,2)==1 && int32_t(call(e,0xc308,1))==-1);
            for(unsigned i=0;i<8;++i)check(ram[0x100000+i]==i+1 && ram[0x100008+i]==0xa0+i);
            check(ram[0x100010]==0x5a && completions==0);
            auto rejects=[&](uint32_t fn,uint32_t count,uint32_t callback=0){
                bool failed=false;try{call(e,fn,0xc200,count,callback,0x88);}catch(const std::runtime_error &){failed=true;}check(failed);
                for(unsigned i=32;i<64;++i)check(ram[0x100000+i]==0x5a);
            };
            descriptor(e,0xc200,0xc100,0x100020,8);
            descriptor(e,0xc210,0xc120,0x100028,8,0x20);rejects(0xc300,2);
            descriptor(e,0xc210,0x1fffff,0x100028,2);rejects(0xc300,2);
            descriptor(e,0xc210,0xc120,0x12001000,8);rejects(0xc300,2);
            descriptor(e,0xc210,0xc120,0x11004000,8);rejects(0xc300,2);
            rejects(0xc300,0);rejects(0xc300,33);rejects(0xc310,1,0xc080);
            descriptor(e,0xc210,0xc120,0x100028,8);check(call(e,0xc300,0xc200,2)==2);
            descriptor(e,0xc200,0xc120,0x100000,16);check(call(e,0xc310,0xc200,1,0xc010,0x88)==3);
            check(completions==0 && ram[0x100000]==0xa0);e.cpu.pc=caller;
        });
    }
    void run(){check(host.startImage("bulk-copy")==1 && completions==0);host.advanceTo(0);check(completions==1);host.advanceTo(1000);check(completions==1);}
};
}
int main(){try{
    Fixture first,second;first.run();check(second.completions==0);second.run();
    first.host.reset();first.completions=0;first.run();
    std::puts("native bulk actual bytes/order/batch atomicity/bounds/attributes/completion/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
