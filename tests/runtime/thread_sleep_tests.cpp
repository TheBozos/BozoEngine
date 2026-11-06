// Native thread scheduling only: no disc, window, input, devices or game code.
#include "bozo/ee/dispatcher.h"
#include <stdexcept>
#include <cstdio>
using namespace bozo::ee;
namespace {
void check(bool ok) { if (!ok) throw std::runtime_error("sleep/wakeup assertion failed"); }
struct Done {};
struct TestHost final : Host {
    unsigned ended=0;
    void libraryCall(Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("library"); }
    void vu0Microprogram(Dispatcher &, R5900Context *, uint32_t) override { throw std::runtime_error("VU"); }
    void ioRead(Dispatcher &, R5900Context *, uint32_t, uint32_t, uint64_t[2]) override { throw std::runtime_error("IO"); }
    void ioWrite(Dispatcher &, R5900Context *, uint32_t, uint32_t, const uint64_t[2]) override { throw std::runtime_error("IO"); }
    void fault(Dispatcher &, R5900Context *, const std::string &s) override { throw std::runtime_error(s); }
    void threadEnded(Dispatcher &, Thread &) override { if (++ended==2) throw Done{}; }
    void idle(Dispatcher &, uint64_t, uint64_t) override { throw std::runtime_error("unexpected idle"); }
};
void mainThread(uint8_t *ram, R5900Context *ctx, Runtime *rt) {
    if (ctx->pc==0x100000) {
        ctx->r[31].uw[0]=0x100008;
        bozo_call_indirect(ram,ctx,rt,0x200000);
        throw std::runtime_error("SleepThread did not stop");
    }
    check(ctx->pc==0x100008 && ctx->r[2].sd[0]==0 && ctx->r[2].ud[1]==0);
    ctx->pc=0;
}
void worker(uint8_t *ram, R5900Context *ctx, Runtime *rt) {
    if(ctx->pc==0x100100) {
        ctx->r[4].uw[0]=1;
        ctx->r[31].uw[0]=0x100108;
        bozo_call_indirect(ram,ctx,rt,0x200010);
        throw std::runtime_error("WakeupThread did not preempt");
    }
    check(ctx->pc==0x100108 && ctx->r[2].sd[0]==1 && ctx->r[2].ud[1]==0);
    ctx->pc=0;
}
}
int main() {
 try {
    Kernel k; k.boot(0x100000,0x1fff000);
    const int other=k.createThread(0x100100,0x180000,0x1000,1,0,0,0);
    check(k.wakeupThread(0,false)==-406 && k.wakeupThread(1,false)==-406);
    check(k.wakeupThread(999,false)==-407 && k.wakeupThread(other,false)==-413);
    check(k.cancelWakeupThread(999,false)==-407);
    check(k.startThread(other,0,false)==other);
    check(k.wakeupThread(other,false)==other && k.wakeupThread(other,true)==other);
    check(k.cancelWakeupThread(other,false)==2 && k.cancelWakeupThread(other,true)==0);
    check(k.wakeupThread(other,false)==other);
    k.setRunning(other);
    check(k.sleepThread()==0 && k.running()==other && k.thread(other)->wakeupCount==0);
    check(k.sleepThread()==0 && k.running()==0 && k.thread(other)->waitReason==kWaitSleep);
    k.setRunning(1);
    check(k.wakeupThread(other,false)==other && k.thread(other)->state==ThreadState::Ready);
    check(k.thread(other)->top().ctx.r[2].sd[0]==0);
    k.block(other,kWaitSemaphore,7,0,0,false,0,0);
    check(k.wakeupThread(other,true)==other && k.thread(other)->state==ThreadState::Waiting);
    check(k.thread(other)->waitReason==kWaitSemaphore && k.thread(other)->wakeupCount==1);
    k.thread(other)->state=ThreadState::WaitSuspended;
    k.thread(other)->waitReason=kWaitSleep;
    check(k.wakeupThread(other,true)==other && k.thread(other)->state==ThreadState::Suspended);
    check(k.thread(other)->waitReason==kWaitNone);
    check(k.cancelWakeupThread(other,true)==1);
    check(k.cancelWakeupThread(0,false)==0);
    k.thread(other)->wakeupCount=7;
    k.exitThread(other,false);
    check(k.wakeupThread(other,true)==-413);
    check(k.startThread(other,0,false)==other && k.thread(other)->wakeupCount==0);
    // Exercise all SDK bindings and their 128-bit return clearing outside a run.
    TestHost callsHost; Dispatcher calls(callsHost); calls.boot(0x100000,0x1fff000);
    const int target=calls.kernel().createThread(0x100100,0x180000,0x1000,1,0,0,0);
    calls.kernel().startThread(target,0,false);
    calls.bindKernelCall(0x300000,KernelCall::iWakeupThread);
    calls.bindKernelCall(0x300010,KernelCall::iCancelWakeupThread);
    calls.bindKernelCall(0x300020,KernelCall::CancelWakeupThread);
    R5900Context ctx; ctx.r[4].uw[0]=target; ctx.r[31].uw[0]=0x100008;
    ctx.r[2].ud[1]=~0ull;
    bozo_call_indirect(calls.runtime().memory.flat(),&ctx,&calls.runtime(),0x300000);
    check(ctx.r[2].sd[0]==target && ctx.r[2].ud[1]==0 && calls.kernel().running()==1);
    bozo_call_indirect(calls.runtime().memory.flat(),&ctx,&calls.runtime(),0x300010);
    check(ctx.r[2].sd[0]==1 && calls.kernel().thread(target)->wakeupCount==0);
    bozo_call_indirect(calls.runtime().memory.flat(),&ctx,&calls.runtime(),0x300020);
    check(ctx.r[2].sd[0]==0);
    TestHost host; Dispatcher d(host);
    const BozoFunctionEntry functions[]={{0x100000,mainThread},{0x100100,worker}};
    const BozoResumeEntry resumes[]={{0x100000,0x100010,mainThread},{0x100100,0x100110,worker}};
    d.setFunctions(functions,2,resumes,2);
    d.bindKernelCall(0x200000,KernelCall::SleepThread);
    d.bindKernelCall(0x200010,KernelCall::WakeupThread);
    d.boot(0x100000,0x1fff000);
    const int id=d.kernel().createThread(0x100100,0x180000,0x1000,1,0,0,0);
    d.kernel().startThread(id,0,false);
    try { d.run(); check(false); } catch(const Done &) {}
    check(host.ended==2);
    std::puts("native sleep/wakeup credits, states and dispatcher resumes passed");
    return 0;
 } catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
