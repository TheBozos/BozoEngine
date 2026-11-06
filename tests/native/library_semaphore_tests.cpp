// Native functions and SDK calls only, with no retail bytes or device backend.
#include "bozo/native/compiled_libraries.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("native library semaphore assertion failed");}
bool call(Env &e,uint32_t target,uint32_t ret){e.cpu.gpr[31]=ret;return e.call(target,ret);}
void root(Env &e) {
    const auto caller=e.cpu.gpr[31];
    e.w32(0x6200,0);e.w32(0x6204,123);e.w32(0x6208,0);e.w32(0x620c,1);
    e.cpu.gpr[4]=0x6200;check(call(e,0x6300,0x6004));check(e.cpu.gpr[2]==1);
    e.w32(0x6210,1);
    e.cpu.gpr[4]=1;check(call(e,0x6320,0x6004));check(int32_t(e.cpu.gpr[2])==-419);
    check(call(e,0x6308,0x6004));check(int32_t(e.cpu.gpr[2])==-100);
    check(call(e,0x6310,0x6004));check(e.cpu.gpr[2]==0);
    check(call(e,0x6318,0x6004));check(int32_t(e.cpu.gpr[2])==-420);
    check(call(e,0x6308,0x6004));check(e.cpu.gpr[2]==0);
    e.cpu.gpr[4]=999;check(call(e,0x6320,0x6004));check(int32_t(e.cpu.gpr[2])==-408);
    for(uint32_t entry:{0x6010u,0x6040u}) {
        const uint32_t desc[]={0,0,entry,0x1000,32};
        for(unsigned i=0;i<5;++i)e.w32(0x6240+i*4,desc[i]);
        e.cpu.gpr[4]=0x6240;check(call(e,0x6330,0x6004));
        e.cpu.gpr[4]=e.cpu.gpr[2];check(call(e,0x6338,0x6004));
    }
    e.cpu.pc=caller;
}
void waiter(Env &e) {
    auto &c=e.cpu;
    if(c.pc==0x6010) {
        c.gpr[16]=c.gpr[31];c.gpr[4]=1;
        if(!call(e,0x6308,0x6018))return;
    }
    if(c.pc==0x6018) {
        check(c.gpr[2]==0);e.w32(0x6214,1);
        // The wakeup credit sent during a semaphore wait must survive it.
        check(call(e,0x6348,0x601c));
    }
    if(c.pc==0x601c) {
        c.gpr[4]=1;if(!call(e,0x6308,0x6020))return;
    }
    check(c.pc==0x6020 && int32_t(c.gpr[2])==-425);
    e.w32(0x6214,2);c.pc=c.gpr[16];
}
void signaler(Env &e) {
    auto &c=e.cpu;
    if(c.pc==0x6040) {
        c.gpr[16]=c.gpr[31];c.gpr[4]=1;
        check(call(e,0x6340,0x6048));check(call(e,0x6318,0x6048));
        if(!call(e,0x6348,0x6050))return;
    }
    throw std::runtime_error("sleeping signaler unexpectedly resumed");
}
void deleteProbe(Env &e) {
    const auto caller=e.cpu.gpr[31];check(e.r32(0x6214)==1);
    e.cpu.gpr[4]=1;check(call(e,0x6328,0x6504));check(e.cpu.gpr[2]==0);
    e.cpu.pc=caller;
}
void completionProbe(Env &e) {
    check(e.r32(0x6214)==2);e.cpu.pc=e.cpu.gpr[31];
}
const Entry entries[]={{0x6000,root},{0x6010,waiter},{0x6018,waiter},{0x601c,waiter},{0x6020,waiter},
                       {0x6040,signaler},{0x6048,signaler},{0x6050,signaler}};
const Entry deletes[]={{0x6500,deleteProbe}},completes[]={{0x6600,completionProbe}};
const Registration registration("semas.irx",0xaaaa,0x6000,0x400,entries,std::size(entries));
const Registration deleteRegistration("delete.irx",0xbbbb,0x6500,0x100,deletes,1);
const Registration completeRegistration("complete.irx",0xcccc,0x6600,0x100,completes,1);
void images(CompiledLibraries &h) {
    Image image{"semas.irx",0xaaaa,0x6000,0x6000,0,std::vector<uint8_t>(0x400),
        {{0x6300,"thsemap",4},{0x6308,"thsemap",8},{0x6310,"thsemap",6},{0x6318,"thsemap",7},
         {0x6320,"thsemap",9},{0x6328,"thsemap",5},{0x6330,"thbase",4},{0x6338,"thbase",6},
         {0x6340,"thbase",25},{0x6348,"thbase",24}}, {},false};
    h.addImage(std::move(image));
    h.addImage({"delete.irx",0xbbbb,0x6500,0x6500,0,std::vector<uint8_t>(0x100),{},{},false});
    h.addImage({"complete.irx",0xcccc,0x6600,0x6600,0,std::vector<uint8_t>(0x100),{},{},false});
}
void run(CompiledLibraries &h) {
    check(h.startImage("semas.irx")==1);check(h.startImage("delete.irx")==2);
    check(h.startImage("complete.irx")==3);
}
}
int main(){try{
    bozo::libs::Libraries a,b;CompiledLibraries first(a,0x10000),second(b,0x10000);
    images(first);images(second);run(first);run(second);first.reset();run(first);
    std::puts("native library semaphore count/block/signal/delete/mixed-wait/reset/isolation passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
