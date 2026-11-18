// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include "bozo/ee/dispatcher.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool v,const char *what){if(!v)throw std::runtime_error(std::string("native heap queries and EE reads: ")+what);}
template<class F>void rejects(F f,const char *message){bool yes=false;try{f();}catch(const std::runtime_error &e){yes=std::strstr(e.what(),message)!=nullptr;}check(yes,message);}
struct TestHost:bozo::ee::Host {
    bozo::libs::Libraries libraries;
    void libraryCall(bozo::ee::Dispatcher &,R5900Context *,uint32_t)override{throw std::runtime_error("unexpected EE library call");}
    void vu0Microprogram(bozo::ee::Dispatcher &,R5900Context *,uint32_t)override{throw std::runtime_error("unexpected VU");}
    void ioRead(bozo::ee::Dispatcher &,R5900Context *,uint32_t,uint32_t,uint64_t[2])override{throw std::runtime_error("unexpected device read");}
    void ioWrite(bozo::ee::Dispatcher &,R5900Context *,uint32_t,uint32_t,const uint64_t[2])override{throw std::runtime_error("unexpected device write");}
    void fault(bozo::ee::Dispatcher &,R5900Context *,const std::string &s)override{throw std::runtime_error(s);}
};
constexpr uint32_t entry=0xc000,allocCall=0xc300,freeCall=0xc308,largestCall=0xc310,totalCall=0xc318,otherDataCall=0xc320,
    receive=0xc200,buffer=0xc100,heapStart=0x10000,heapEnd=0x180000;
void unused(Env &){throw std::runtime_error("unbound fixture entry");}
const Entry entries[]={{entry,unused}};
const Registration registration("heap-queries",1,0xc000,0x400,entries,1);
uint32_t call(Env &e,uint32_t function,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xc004;
    check(e.call(function,0xc004),"unexpected yield");return e.cpu.gpr[2];
}
struct Fixture {
    TestHost eeHost;bozo::ee::Dispatcher dispatcher{eeHost};CompiledLibraries host{eeHost.libraries,heapStart};
    Fixture(){
        eeHost.libraries.attach(dispatcher);
        Image image{"heap-queries",1,0xc000,entry,0x12340,std::vector<uint8_t>(0x400)};
        image.imports={{allocCall,"sysmem",4},{freeCall,"sysmem",5},{largestCall,"sysmem",7},{totalCall,"sysmem",8},
            {otherDataCall,"sifcmd",23}};
        host.addImage(std::move(image));
        host.bindNative(entry,[this](Env &e){const auto ret=e.cpu.gpr[31];
            constexpr uint32_t whole=heapEnd-heapStart;
            check(call(e,largestCall)==whole && call(e,totalCall)==whole,"an empty heap is one free block");
            const uint32_t first=call(e,allocCall,0,0x1000),second=call(e,allocCall,0,0x2000);
            check(first && second,"two blocks allocated");
            check(call(e,freeCall,first)==0,"the first block released");
            check(call(e,largestCall)==whole-0x3000 && call(e,totalCall)==whole-0x2000,"a hole: the largest block and the sum differ");
            auto *ram=dispatcher.runtime().memory.ram();
            for(unsigned i=0;i<32;++i)ram[0x100000+i]=uint8_t(0x40+i);
            for(unsigned i=0;i<64;++i)e.w8(buffer+i,0x5a);
            check(call(e,otherDataCall,receive,0x100000,buffer,32)==0,"EE data read");
            for(unsigned i=0;i<32;++i)check(e.r8(buffer+i)==0x40+i,"the EE bytes arrive");
            check(e.r8(buffer+32)==0x5a,"nothing past the size is written");
            check(call(e,otherDataCall,receive,0x20100008,buffer+40,4)==0 && e.r8(buffer+40)==0x48,"the uncached alias reads RAM");
            check(call(e,otherDataCall,receive,0x12001000,buffer,0)==0 && e.r8(buffer)==0x40,"an empty read touches nothing");
            rejects([&]{call(e,otherDataCall,receive,0x12001000,buffer,16);},"outside EE RAM");
            e.cpu.gpr[2]=1;e.cpu.pc=ret;
        });
    }
    void run(){check(host.startImage("heap-queries")==1,"completed native entry");}
};
}
int main(){try{
    Fixture fixture;fixture.run();
    std::puts("native sysmem free-memory queries and sifcmd EE data reads passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
