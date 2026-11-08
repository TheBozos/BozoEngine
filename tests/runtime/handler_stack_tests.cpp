#include "bozo/ee/kernel.h"
#include <cstdio>
#include <stdexcept>
using namespace bozo::ee;
namespace {
void check(bool v) { if (!v) throw std::runtime_error("handler stack assertion failed"); }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::runtime_error &) { rejected = true; }
    check(rejected);
}
}
int main() {
    try {
        Kernel k; k.boot(0x100000, 0x1fffff0);
        auto &main = *k.thread(1);
        k.useOwnedHandlerStacks();
        QueuedHandler h; h.pc=0x300000; h.ra=0x300004; h.a[0]=123;
        // Missing bounds must fail without consuming the callback or adding frames.
        k.queue(h);
        rejects([&] { k.stackHandler(main); });
        check(k.handlerQueued() && main.frames.size()==1);
        k.threadSetup(0x1fefff0, 0x10000, 0);
        main.frames.back()->ctx.r[29].uw[0]=0x1fffd53;
        auto &first=k.stackHandler(main);
        check(first.sp==0x1fffcd0 && first.ctx.r[29].uw[0]==first.sp);
        check(first.a[0]==123 && first.ra==h.ra && !k.handlerQueued());
        check(main.handlerSlot==-1);
        // A callback's actual prologue moves SP: nesting must use that live SP.
        first.ctx.r[29].uw[0]=first.sp-0x60;
        k.queue(h);
        check(k.stackHandler(main).sp==0x1fffbf0);
        k.endHandler(main); k.endHandler(main);
        check(main.frames.back()->ctx.r[29].uw[0]==0x1fffd53);
        k.setRunning(1);
        rejects([&] { k.parkThreadStack(0x1fffd50); });
        k.parkThreadStack(0x1fffd53);
        rejects([&] { k.parkThreadStack(0x1fffd53); });
        main.frames.back()->ctx.r[29].uw[0]=0x70003ea0;
        k.queue(h); check(k.stackHandler(main).sp==0x1fffcd0);
        // The nested callback has its own live RAM stack and parked metadata.
        k.parkThreadStack(0x1fffcd0);
        main.top().ctx.r[29].uw[0]=0x70003d00;
        k.queue(h); check(k.stackHandler(main).sp==0x1fffc50);
        k.endHandler(main);
        k.resumeThreadStack(0x1fffcd0);
        k.endHandler(main);
        rejects([&] { k.resumeThreadStack(0x1fffd50); });
        check(main.top().parkedStack==0x1fffd53);
        auto &parkedIdle=k.pseudoThread();k.queue(h);
        check(k.stackHandler(parkedIdle).sp==0x1fffcd0);
        k.endHandler(parkedIdle);
        k.resumeThreadStack(0x1fffd53);
        rejects([&] { k.resumeThreadStack(0x1fffd53); });
        main.top().ctx.r[29].uw[0]=0x1fffd53;
        // Idle callbacks borrow below the parked main frame, then nest below
        // their own live frame. No allocation in the former slot-six heap area.
        auto &idle=k.pseudoThread(); k.queue(h);
        auto &borrowed=k.stackHandler(idle);
        check(borrowed.sp==0x1fffcd0 && borrowed.sp>0x1fefff0);
        borrowed.ctx.r[29].uw[0]-=0x100;
        k.queue(h); check(k.stackHandler(idle).sp==0x1fffb50);
        k.endHandler(idle); k.endHandler(idle);
        // Caller-provided stacks are unchanged, including stacks outside the
        // thread region. An automatic nested callback uses an owned frame.
        h.sp=0x90000; k.queue(h);
        check(k.stackHandler(main).sp==h.sp);
        h.sp=0; k.queue(h); check(k.stackHandler(main).sp==0x1fffcd0);
        k.endHandler(main); k.endHandler(main);
        // Never fall back above an automatic frame whose live SP escaped.
        k.queue(h); auto &escaped=k.stackHandler(main);
        escaped.ctx.r[29].uw[0]=0x1fe7000;
        k.queue(h); rejects([&] { k.stackHandler(main); });
        check(k.handlerQueued() && main.frames.size()==2);
        k.endHandler(main);
        main.frames.back()->ctx.r[29].uw[0]=0x1fefff0+0x80;
        rejects([&] { k.stackHandler(main); });
        check(k.handlerQueued() && main.frames.size()==1);
        // The smallest accepted space leaves an aligned frame below the home area.
        main.frames.back()->ctx.r[29].uw[0]=0x1fefff0+0x90;
        check(k.stackHandler(main).sp==0x1ff0000);
        k.endHandler(main);
        const auto childId=k.createThread(0x110000,0x140000,0x4000,50,0,0,0);
        check(k.startThread(childId,0,false)==childId);
        auto &child=*k.thread(childId);
        child.frames.back()->ctx.r[29].uw[0]=0x143c00;
        k.queue(h); check(k.stackHandler(child).sp==0x143b80);
        k.endHandler(child);
        // Invalid declarations and an unstarted thread have no usable frame.
        const auto dormantId=k.createThread(0x110000,0x150000,0x4000,50,0,0,0);
        auto &dormant=*k.thread(dormantId);
        k.queue(h); rejects([&] { k.stackHandler(dormant); });
        check(k.handlerQueued() && dormant.frames.size()==1);
        dormant.stack=0xfffffff0; dormant.stackSize=0x100;
        rejects([&] { k.stackHandler(dormant); });
        check(k.handlerQueued());
        dormant.stack=0x1fffff0; dormant.stackSize=0x100;
        rejects([&] { k.stackHandler(dormant); });
        // Restarting a terminated thread clears a stale parked-frame annotation.
        k.setRunning(childId);k.parkThreadStack(child.top().ctx.r[29].uw[0]);
        k.terminateThread(childId,false,false);
        check(k.startThread(childId,0,false)==childId && child.top().parkedStack==0);
        // Recorded hosts retain their historical stack convention by default.
        Kernel recorded; recorded.boot(0x100000,0x90000);
        recorded.queue(h);
        check(recorded.stackHandler(*recorded.thread(1)).sp==kHandlerStackTop);
        auto &other=recorded.pseudoThread(); recorded.queue(h);
        check(recorded.stackHandler(other).sp==kHandlerStackTop-kHandlerStackBytes);
        std::puts("owned handler stack bounds/nesting/idle/explicit/rejection/replay tests passed");
        return 0;
    } catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
