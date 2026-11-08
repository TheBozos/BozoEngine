// Synthetic native callbacks: scheduler deadlines, no game/device/input state.
#include "bozo/ee/dispatcher.h"
#include "bozo/recomp.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::ee;
namespace {
void check(bool v) { if(!v) throw std::runtime_error("deadline assertion failed"); }
struct Done {};
struct TestHost final:Host {
    bool sleep=false,owned=false; unsigned callbacks=0,idles=0; uint64_t due=100;
    void libraryCall(Dispatcher &d,R5900Context *c,uint32_t pc) override {
        check(pc==0x300000 && d.now(c)>=due && d.now(c)<d.kernel().field());
        check(c->r[4].uw[0]==123 && c->r[28].uw[0]==0x123456);
        if(owned) {
            check(c->r[29].uw[0]==0x1ffef80);
            auto *ram=d.runtime().memory.ram();
            for(unsigned i=0;i<0x60;++i) ram[c->r[29].uw[0]-0x60+i]=0xCC;
        }
        ++callbacks; d.runtime().memory.ram()[0x2000]=1;
        if(sleep) check(d.kernel().wakeupThread(1,true)==1);
        c->pc=c->r[31].uw[0];
    }
    void threadEnded(Dispatcher &,Thread &) override { throw Done{}; }
    void idle(Dispatcher &,uint64_t from,uint64_t to) override { check(to==due && from<to);++idles; }
    void vu0Microprogram(Dispatcher &,R5900Context *,uint32_t) override { throw std::runtime_error("VU"); }
    void ioRead(Dispatcher &,R5900Context *,uint32_t,uint32_t,uint64_t[2]) override { throw std::runtime_error("IO"); }
    void ioWrite(Dispatcher &,R5900Context *,uint32_t,uint32_t,const uint64_t[2]) override { throw std::runtime_error("IO"); }
    void fault(Dispatcher &,R5900Context *,const std::string &s) override { throw std::runtime_error(s); }
};
void mainThread(uint8_t *ram,R5900Context *c,Runtime *rt) {
    if(c->pc==0x100000 && ram[0x2001]) {
        c->r[31].uw[0]=0x100008;
        bozo_call_indirect(ram,c,rt,0x300010);
        throw std::runtime_error("sleep did not stop");
    }
    while(!ram[0x2000]) if(bozo::checkpoint(ram,c,rt,8,0x100008)) return;
    c->pc=0;
}
}
int main() {
 try {
    Kernel k; k.boot(0x100000,0x1fff000);
    QueuedHandler h;h.pc=0x300000;h.a[0]=123;h.gp=0x123456;h.tag=0x123456789abcdef0ull;
    const auto original=k.nextEvent();
    k.scheduleDeadline(9,200,h); k.scheduleDeadline(9,100,h);
    k.scheduleDeadline(7,50,h);check(k.cancelDeadline(7) && !k.cancelDeadline(7));
    check(k.nextEvent()==100);k.processEvents(99);check(!k.handlerQueued());
    k.processEvents(100);check(k.handlerQueued() && k.nextEvent()==original);
    check(!k.cancelDeadline(9));
    auto &frame=k.stackHandler(*k.thread(1));
    check(frame.pc==h.pc && frame.tag==h.tag && frame.a[0]==123 && frame.gp==h.gp);
    k.endHandler(*k.thread(1));
    h.a[0]=2;k.scheduleDeadline(2,400,h);h.a[0]=1;k.scheduleDeadline(1,400,h);
    k.processEvents(400);
    check(k.stackHandler(*k.thread(1)).a[0]==1);k.endHandler(*k.thread(1));
    check(k.stackHandler(*k.thread(1)).a[0]==2);k.endHandler(*k.thread(1));
    // Invalid replacements preserve the prior pending callback.
    k.scheduleDeadline(3,600,h);
    try { k.scheduleDeadline(3,~0ull,h);check(false); } catch(const std::invalid_argument &) {}
    check(k.nextEvent()==600);check(k.cancelDeadline(3));
    Kernel independent;check(independent.nextEvent()==original);
    // Both a checkpointing thread and an otherwise idle sleeping thread must
    // observe the callback before the next field, through the actual dispatcher.
    for(bool owned:{false,true}) for(bool sleep:{false,true}) {
        TestHost host;host.sleep=sleep;host.owned=owned;Dispatcher d(host);
        const BozoFunctionEntry f[]={{0x100000,mainThread}};
        const BozoResumeEntry r[]={{0x100000,0x100010,mainThread}};
        d.setFunctions(f,1,r,1);d.bindLibrary(0x300000);
        d.bindKernelCall(0x300010,KernelCall::SleepThread);
        d.boot(0x100000,0x1fff000);d.runtime().memory.ram()[0x2001]=sleep;
        if(owned) {
            d.kernel().threadSetup(0x1ff0000,0x10000,0);
            d.kernel().useOwnedHandlerStacks();
            auto *ram=d.runtime().memory.ram();
            for(unsigned i=0;i<0x80;++i) ram[0x1fff000+i]=0xA5;
            for(unsigned i=0;i<0x100;++i) ram[0x1fe7f00+i]=0x5A;
        }
        h.a[0]=123;d.kernel().scheduleDeadline(1,host.due,h);
        try { d.run();check(false); } catch(const Done &) {}
        check(host.callbacks==1 && host.idles==unsigned(sleep));
        check(!d.kernel().handlerQueued());
        if(owned) {
            auto *ram=d.runtime().memory.ram();
            for(unsigned i=0;i<0x80;++i) check(ram[0x1fff000+i]==0xA5);
            for(unsigned i=0;i<0x100;++i) check(ram[0x1fe7f00+i]==0x5A);
            for(unsigned i=0;i<0x60;++i) check(ram[0x1ffef20+i]==0xCC);
        }
    }
    std::puts("native callback deadline replacement/cancel/order/isolation/checkpoint/idle tests passed");return 0;
 } catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what());return 1; }
}
