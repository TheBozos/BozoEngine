// BozoEngine's EE kernel. See bozo/ee/kernel.h and docs/specs/kernel.md.
#include "bozo/ee/kernel.h"

#include <algorithm>

namespace bozo::ee
{
    namespace
    {
        constexpr uint64_t kEeClock = 294'912'000ull;
        // The vblank (INTC cause 2) starts every field; its end (cause 3) follows 147,456 EE cycles later (half a
        // millisecond: the reference's model, from the boot log's idle stretches).
        constexpr uint64_t kVblankLength = 147'456ull;
        constexpr uint32_t kIntcVblankStart = 2, kIntcVblankEnd = 3;

        uint64_t fieldCycles(uint32_t numerator, uint32_t denominator)
        {
            return numerator ? kEeClock * denominator / numerator : 0;
        }
    }

    const char *opName(uint32_t op)
    {
        static const char *const names[] = {
            "?", "queue", "block", "wake", "irq", "task", "event", "create", "start", "exit", "terminate", "priority",
            "rotate", "suspend", "resume", "wakeup", "cancel-wakeup", "release-wait", "sema-create", "sema-delete",
            "signal", "poll-sema", "wait-sema", "flag-create", "flag-delete", "flag-set", "flag-clear", "flag-poll",
            "flag-wait", "alarm-set", "alarm-cancel", "handler-add", "handler-remove", "handler-enable", "cause-enable",
            "vsync-flag", "gs-vsync-callback", "field-rate", "thread-setup", "transfer"};
        return op < kOpCount ? names[op] : "?";
    }

    Kernel::Kernel()
    {
        fieldCycles_ = fieldCycles(60000, 1001); // NTSC until the game sets its field rate
        nextStart_ = fieldCycles_;
        nextEnd_ = nextStart_ + kVblankLength;
    }

    void Kernel::emit(uint32_t op, std::initializer_list<uint32_t> args, uint64_t wide)
    {
        OpEvent e;
        e.op = op;
        e.origin = origin_;
        e.depth = depth_;
        e.originCall = origin_ == kOriginCall ? originCall_ : kNoCall;
        e.wide = wide;
        for (uint32_t a : args)
            if (e.argc < 16)
                e.args[e.argc++] = a;
        e.thread = running_;
        (observer_ ? observer_ : &defaultObserver_)->op(e);
    }

    Thread *Kernel::thread(int32_t id)
    {
        auto it = threads_.find(id);
        return it == threads_.end() ? nullptr : it->second.get();
    }

    // The ready queues hold the ready threads; the running thread is outside them (calllog: a thread that gives way
    // goes to the tail of its queue, behind the threads already there).
    int32_t Kernel::firstReady() const
    {
        for (const auto &q : ready_)
            if (!q.empty())
                return q.front();
        return 0;
    }

    int32_t Kernel::best() const
    {
        const int32_t head = firstReady();
        if (running_ > 0)
            if (const Thread *r = threads_.count(running_) ? threads_.at(running_).get() : nullptr;
                r && r->state == ThreadState::Running)
            {
                if (!head || threads_.at(head)->priority >= r->priority)
                    return running_;
            }
        return head;
    }

    bool Kernel::sliceContender() const
    {
        auto it = threads_.find(running_);
        if (running_ <= 0 || it == threads_.end() || it->second->state != ThreadState::Running)
            return false;
        const uint32_t p = it->second->priority % kPriorities;
        for (uint32_t q = 0; q <= p; ++q)
            if (!ready_[q].empty())
                return true;
        return false;
    }

    void Kernel::runOrder(std::vector<int32_t> &out) const
    {
        out.clear();
        out.push_back(running_);
        for (const auto &q : ready_)
            for (int32_t id : q)
                out.push_back(id);
    }

    namespace
    {
        void freshContext(Frame &f)
        {
            f.ctx = R5900Context();
            f.ctx.vu0_q_bits = kOne;
            f.status = kFreshStatus;
            f.last.fill(0);
            f.ran = false;
        }
    }

    void Kernel::boot(uint32_t entry, uint32_t sp)
    {
        auto t = std::make_unique<Thread>();
        t->id = 1;
        t->state = ThreadState::Running;
        t->priority = t->initPriority = 0;
        auto f = std::make_unique<Frame>();
        freshContext(*f);
        f->ctx.r[29].ud[0] = sp;
        f->ctx.pc = entry;
        f->ctx.vu0_r = kOne;
        t->frames.push_back(std::move(f));
        running_ = 1;
        threads_[1] = std::move(t);
    }

    void Kernel::makeReady(Thread &t)
    {
        t.state = ThreadState::Ready;
        ready_[t.priority % kPriorities].push_back(t.id);
    }

    void Kernel::removeFromQueue(Thread &t)
    {
        auto &q = ready_[t.priority % kPriorities];
        q.erase(std::remove(q.begin(), q.end(), t.id), q.end());
    }

    void Kernel::deschedule(bool toTail)
    {
        if (running_ == 0)
            return;
        if (Thread *t = thread(running_); t && t->state == ThreadState::Running)
        {
            t->state = ThreadState::Ready;
            if (!t->pseudo)
            {
                auto &q = ready_[t->priority % kPriorities];
                if (toTail)
                    q.push_back(t->id);
                else
                    q.push_front(t->id);
            }
        }
        running_ = 0;
    }

    void Kernel::setRunning(int32_t id)
    {
        if (running_ == id)
            return;
        deschedule(false); // preempted: it runs again before the threads of its priority
        running_ = id;
        ++picks_;
        if (Thread *t = thread(id); t && (t->state == ThreadState::Ready || t->pseudo))
        {
            removeFromQueue(*t);
            t->state = ThreadState::Running;
        }
    }

    // ---- Threads -----------------------------------------------------------------------------------------------

    int32_t Kernel::createThread(uint32_t entry, uint32_t stack, uint32_t stackSize, uint32_t priority, uint32_t gp,
                                 uint32_t attr, uint32_t option)
    {
        int32_t id = 2;
        if (!threads_.empty())
            id = std::max(id, threads_.rbegin()->first + 1);
        id = std::max(id, nextThreadId_);
        nextThreadId_ = id + 1;
        emit(kOpCreate, {static_cast<uint32_t>(id), entry, stack, stackSize, priority, gp, attr, option});
        auto t = std::make_unique<Thread>();
        t->id = id;
        t->entry = entry;
        t->stack = stack;
        t->stackSize = stackSize;
        t->priority = t->initPriority = priority;
        t->gp = gp;
        t->attr = attr;
        t->option = option;
        t->frames.push_back(std::make_unique<Frame>());
        threads_[id] = std::move(t);
        return id;
    }

    int32_t Kernel::startThread(int32_t id, uint32_t arg, bool interruptSafe)
    {
        Thread *t = thread(id);
        if (!t || t->state != ThreadState::Dormant)
            return -405; // KE_NOT_DORMANT
        emit(kOpStart, {static_cast<uint32_t>(id), arg, interruptSafe ? 1u : 0u});
        t->arg = arg;
        t->priority = t->initPriority;
        t->frames.resize(1);
        Frame &f = *t->frames[0];
        freshContext(f);
        f.ctx.r[4].sd[0] = static_cast<int32_t>(arg);
        f.ctx.r[28].sd[0] = static_cast<int32_t>(t->gp);
        f.ctx.r[29].sd[0] = static_cast<int32_t>(t->stack + t->stackSize);
        f.ctx.pc = t->entry;
        makeReady(*t);
        return id;
    }

    void Kernel::exitThread(int32_t id, bool deleted)
    {
        Thread *t = thread(id);
        if (!t)
            return;
        emit(kOpExit, {static_cast<uint32_t>(id), deleted ? 1u : 0u});
        removeFromQueue(*t);
        t->state = ThreadState::Dormant;
        if (running_ == id)
            running_ = 0;
        if (deleted)
            threads_.erase(id);
    }

    int32_t Kernel::terminateThread(int32_t id, bool deleted, bool interruptSafe)
    {
        Thread *t = thread(id);
        if (!t)
            return -406; // KE_UNKNOWN_THID
        if (t->state == ThreadState::Dormant)
            return -413; // KE_DORMANT (calllog: TerminateThread of a thread that has ended)
        emit(kOpTerminate, {static_cast<uint32_t>(id), deleted ? 1u : 0u, interruptSafe ? 1u : 0u});
        removeFromQueue(*t);
        t->state = ThreadState::Dormant;
        t->frames.resize(1);
        if (running_ == id)
            running_ = 0;
        if (deleted)
            threads_.erase(id);
        return 0; // calllog: TerminateThread returns 0
    }

    int32_t Kernel::deleteThread(int32_t id, bool)
    {
        Thread *t = thread(id);
        if (!t)
            return -406;
        if (t->state != ThreadState::Dormant)
            return -405;
        emit(kOpTerminate, {static_cast<uint32_t>(id), 1u});
        threads_.erase(id);
        return 0; // calllog: DeleteThread returns 0
    }

    int32_t Kernel::changePriority(int32_t id, uint32_t priority, bool interruptSafe)
    {
        emit(kOpPriority, {static_cast<uint32_t>(id), priority, interruptSafe ? 1u : 0u});
        Thread *t = thread(id == 0 ? running_ : id);
        if (!t)
            return -406;
        const int32_t old = static_cast<int32_t>(t->priority);
        if (t->state == ThreadState::Ready)
        {
            removeFromQueue(*t);
            t->priority = priority;
            ready_[priority % kPriorities].push_back(t->id);
        }
        else
            t->priority = priority;
        return old;
    }

    int32_t Kernel::rotateReadyQueue(uint32_t priority, bool interruptSafe)
    {
        emit(kOpRotate, {priority, interruptSafe ? 1u : 0u});
        // Rotating the running thread's own priority: it gives way at the next transfer, to the tail of its queue
        // (even when it is alone there: the dispatcher then picks it again). Another priority: the head of that
        // queue goes to its tail.
        if (Thread *t = thread(running_); t && t->state == ThreadState::Running && t->priority == priority % kPriorities)
            switchRequested_ = true;
        else if (auto &q = ready_[priority % kPriorities]; q.size() > 1)
        {
            q.push_back(q.front());
            q.pop_front();
        }
        // 0 (KE_OK), as the reference answers it (boot #228421: v0 0 after the rotations TMB makes, all through its
        // switchThread override; none has v0 the priority).
        return 0;
    }

    void Kernel::block(int32_t id, uint32_t reason, uint32_t object, uint32_t bits, uint32_t mode, bool completion,
                       uint64_t token, int32_t fixedResult)
    {
        emit(kOpBlock, {static_cast<uint32_t>(id), reason, object, bits, mode, completion ? 1u : 0u}, token);
        Thread *t = thread(id);
        if (!t)
            return;
        removeFromQueue(*t);
        t->state = ThreadState::Waiting;
        t->waitReason = reason;
        t->waitObject = object;
        t->flagBits = bits;
        t->flagMode = mode;
        t->waitToken = token;
        t->fixedResult = fixedResult;
        t->waitCompletion = completion;
        if (running_ == id)
            running_ = 0;
    }

    void Kernel::wake(int32_t id, uint32_t result, bool completion, uint32_t reason)
    {
        emit(kOpWake, {static_cast<uint32_t>(id), result, completion ? 1u : 0u, reason});
        Thread *t = thread(id);
        if (!t || t->state != ThreadState::Waiting)
            return;
        if (reason == kWaitSemaphore)
            if (Semaphore *s = sema(static_cast<int32_t>(t->waitObject)))
                s->waiters.erase(std::remove(s->waiters.begin(), s->waiters.end(), id), s->waiters.end());
        t->top().ctx.r[2].sd[0] = static_cast<int32_t>(result);
        t->waitReason = kWaitNone;
        t->completionDue = completion;
        makeReady(*t);
    }

    // ---- Semaphores ----------------------------------------------------------------------------------------------

    Semaphore *Kernel::sema(int32_t id)
    {
        auto it = semas_.find(id);
        return it == semas_.end() ? nullptr : &it->second;
    }

    int32_t Kernel::createSema(int32_t init, int32_t max, uint32_t attr, uint32_t option)
    {
        emit(kOpSemaCreate, {static_cast<uint32_t>(init), static_cast<uint32_t>(max), attr, option});
        const int32_t id = nextSemaId_++;
        Semaphore &s = semas_[id];
        s.id = id;
        s.count = s.init = init;
        s.max = max;
        s.attr = attr;
        s.option = option;
        return id;
    }

    int32_t Kernel::deleteSema(int32_t id, bool interruptSafe)
    {
        Semaphore *s = sema(id);
        if (!s)
            return -408; // KE_UNKNOWN_SEMID
        emit(kOpSemaDelete, {static_cast<uint32_t>(id), interruptSafe ? 1u : 0u});
        const std::deque<int32_t> waiters = s->waiters;
        semas_.erase(id);
        Nested n(*this);
        for (int32_t w : waiters)
            wake(w, static_cast<uint32_t>(-425), false, kWaitSemaphore); // KE_WAIT_DELETE
        return id;
    }

    int32_t Kernel::signalSema(int32_t id, bool interruptSafe)
    {
        Semaphore *s = sema(id);
        if (!s)
            return -408;
        emit(kOpSignal, {static_cast<uint32_t>(id), interruptSafe ? 1u : 0u});
        if (!s->waiters.empty())
        {
            const int32_t w = s->waiters.front();
            Nested n(*this);
            wake(w, static_cast<uint32_t>(id), false, kWaitSemaphore);
        }
        else
            ++s->count;
        return id;
    }

    int32_t Kernel::waitSema(int32_t id)
    {
        Semaphore *s = sema(id);
        if (!s)
            return -408;
        emit(kOpWaitSema, {static_cast<uint32_t>(id)});
        if (s->count > 0)
        {
            --s->count;
            return id;
        }
        s->waiters.push_back(running_);
        Nested n(*this);
        block(running_, kWaitSemaphore, static_cast<uint32_t>(id), 0, 0, false, 0, 0);
        return id;
    }

    int32_t Kernel::pollSema(int32_t id)
    {
        Semaphore *s = sema(id);
        if (!s)
            return -408;
        emit(kOpPollSema, {static_cast<uint32_t>(id)});
        if (s->count > 0)
        {
            --s->count;
            return id;
        }
        return -419; // KE_SEMA_ZERO
    }

    // ---- Interrupts --------------------------------------------------------------------------------------------

    void Kernel::irq(bool dmac, uint32_t cause)
    {
        const uint32_t bit = 1u << (cause & 31u);
        const bool enabled = ((dmac ? dmacMask_ : intcMask_) & bit) != 0;
        emit(kOpIrq, {dmac ? 1u : 0u, cause, enabled ? 1u : 0u});
        if (!enabled)
        {
            (dmac ? dmacPending_ : intcPending_) |= bit;
            return;
        }
        Nested n(*this);
        for (const InterruptHandler &h : (dmac ? dmac_ : intc_)[cause & 15u])
            if (h.enabled)
            {
                QueuedHandler q;
                q.kind = kInvokeInterrupt;
                q.pc = h.pc;
                q.a[0] = cause;
                q.a[1] = h.arg;
                q.gp = h.gp;
                queue(q);
            }
    }

    int32_t Kernel::addHandler(bool dmac, uint32_t cause, uint32_t pc, int32_t next, uint32_t arg, uint32_t gp, uint32_t sp)
    {
        emit(kOpHandlerAdd, {dmac ? 1u : 0u, cause, pc, static_cast<uint32_t>(next), arg, gp, sp});
        InterruptHandler h;
        h.id = dmac ? nextDmacHandler_++ : nextIntcHandler_++;
        h.cause = cause;
        h.pc = pc;
        h.arg = arg;
        h.gp = gp;
        h.sp = sp;
        auto &list = (dmac ? dmac_ : intc_)[cause & 15u];
        if (next == 0)
            list.insert(list.begin(), h);
        else
            list.push_back(h);
        return h.id;
    }

    int32_t Kernel::removeHandler(bool dmac, uint32_t cause, int32_t id)
    {
        emit(kOpHandlerRemove, {dmac ? 1u : 0u, cause, static_cast<uint32_t>(id)});
        auto &list = (dmac ? dmac_ : intc_)[cause & 15u];
        const auto before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const InterruptHandler &h) { return h.id == id; }),
                   list.end());
        return list.size() != before ? 0 : -1;
    }

    int32_t Kernel::causeEnable(bool dmac, uint32_t cause, bool on)
    {
        const uint32_t bit = 1u << (cause & 31u);
        uint32_t &mask = dmac ? dmacMask_ : intcMask_;
        const bool was = (mask & bit) != 0;
        emit(kOpCauseEnable, {dmac ? 1u : 0u, cause, on ? 1u : 0u});
        mask = on ? (mask | bit) : (mask & ~bit);
        uint32_t &pending = dmac ? dmacPending_ : intcPending_;
        if (on && !was && (pending & bit))
        {
            pending &= ~bit;
            KernelObserver *o = observer_ ? observer_ : &defaultObserver_;
            if (!dmac || o->dmacStatusSet(cause))
            {
                Nested n(*this);
                irq(dmac, cause);
            }
        }
        (void)was;
        return 0; // calllog: EnableIntc/Dmac and DisableIntc/Dmac return 0 in every TMB record
    }

    void Kernel::queue(const QueuedHandler &h)
    {
        QueuedHandler q = h;
        q.sequence = ++sequence_; // counted from 1
        emit(kOpQueue,
             {q.kind, q.pc, q.a[0], q.a[1], q.a[2], q.a[3], q.gp, q.sp, q.ra, q.completion ? 1u : 0u,
              static_cast<uint32_t>(q.sequence), static_cast<uint32_t>(q.tag)},
             q.tag);
        queued_.push_back(q);
    }

    Frame &Kernel::stackOn(Thread &on, const QueuedHandler &h)
    {
        if (on.handlerSlot < 0)
            on.handlerSlot = handlerSlots_++;
        auto f = std::make_unique<Frame>();
        freshContext(*f);
        f->kind = h.kind;
        f->pc = h.pc;
        for (int i = 0; i < 4; ++i)
            f->a[i] = h.a[i];
        f->gp = h.gp;
        f->sp = h.sp ? h.sp : kHandlerStackTop - static_cast<uint32_t>(on.handlerSlot) * kHandlerStackBytes;
        f->ra = h.ra;
        f->completion = h.completion;
        f->sequence = h.sequence;
        f->tag = h.tag;
        for (int i = 0; i < 4; ++i)
            f->ctx.r[4 + i].sd[0] = static_cast<int32_t>(f->a[i]);
        f->ctx.r[28].sd[0] = static_cast<int32_t>(f->gp);
        f->ctx.r[29].sd[0] = static_cast<int32_t>(f->sp);
        f->ctx.r[31].sd[0] = static_cast<int32_t>(f->ra);
        f->ctx.pc = f->pc;
        on.frames.push_back(std::move(f));
        return *on.frames.back();
    }

    Frame &Kernel::stackHandler(Thread &on)
    {
        const QueuedHandler h = queued_.front();
        queued_.pop_front();
        return stackOn(on, h);
    }

    Thread &Kernel::pseudoThread()
    {
        for (auto &[id, t] : threads_)
            if (t->pseudo && t->frames.size() == 1)
                return *t;
        auto t = std::make_unique<Thread>();
        t->id = nextPseudo_--;
        t->pseudo = true;
        t->state = ThreadState::Dormant;
        t->frames.push_back(std::make_unique<Frame>());
        Thread &r = *t;
        threads_[r.id] = std::move(t);
        return r;
    }

    void Kernel::endHandler(Thread &t)
    {
        if (t.frames.size() > 1)
            t.frames.pop_back();
        if (t.pseudo && t.frames.size() == 1 && running_ == t.id)
            running_ = 0;
    }

    // ---- Setup ---------------------------------------------------------------------------------------------------

    void Kernel::fieldRate(uint32_t numerator, uint32_t denominator)
    {
        emit(kOpFieldRate, {numerator, denominator});
        if (const uint64_t c = fieldCycles(numerator, denominator))
            fieldCycles_ = c;
    }

    uint32_t Kernel::threadSetup(uint32_t stack, uint32_t stackSize, uint32_t gp)
    {
        emit(kOpThreadSetup, {stack, stackSize, gp});
        return stack + stackSize;
    }

    bool Kernel::transfer(bool interruptSafe)
    {
        const bool give = !interruptSafe && running_ > 0 && (switchRequested_ || best() != running_);
        if (!interruptSafe)
            switchRequested_ = false;
        if (give)
            deschedule(true);
        emit(kOpTransfer, {interruptSafe ? 1u : 0u});
        return give;
    }

    void Kernel::replay(uint32_t op, const uint32_t *a, uint32_t argc, uint64_t wide)
    {
        auto arg = [&](uint32_t i) { return i < argc ? a[i] : 0u; };
        switch (op)
        {
        case kOpQueue:
        {
            QueuedHandler q;
            q.kind = static_cast<uint16_t>(arg(0));
            q.pc = arg(1);
            for (uint32_t i = 0; i < 4; ++i)
                q.a[i] = arg(2 + i);
            q.gp = arg(6);
            q.sp = arg(7);
            q.ra = arg(8);
            q.completion = arg(9) != 0;
            q.tag = wide;
            queue(q);
            break;
        }
        case kOpBlock:
            block(static_cast<int32_t>(arg(0)), arg(1), arg(2), arg(3), arg(4), arg(5) != 0, wide,
                  static_cast<int32_t>(arg(3)));
            break;
        case kOpWake: wake(static_cast<int32_t>(arg(0)), arg(1), arg(2) != 0, arg(3)); break;
        case kOpIrq: irq(arg(0) != 0, arg(1)); break;
        case kOpCreate: createThread(arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7)); break;
        case kOpStart: startThread(static_cast<int32_t>(arg(0)), arg(1), arg(2) != 0); break;
        case kOpExit: exitThread(static_cast<int32_t>(arg(0)), arg(1) != 0); break;
        case kOpTerminate:
            if (arg(1))
                deleteThread(static_cast<int32_t>(arg(0)), false);
            else
                terminateThread(static_cast<int32_t>(arg(0)), false, arg(2) != 0);
            break;
        case kOpPriority: changePriority(static_cast<int32_t>(arg(0)), arg(1), arg(2) != 0); break;
        case kOpRotate: rotateReadyQueue(arg(0), arg(1) != 0); break;
        case kOpSemaCreate:
            createSema(static_cast<int32_t>(arg(0)), static_cast<int32_t>(arg(1)), arg(2), arg(3));
            break;
        case kOpSemaDelete: deleteSema(static_cast<int32_t>(arg(0)), arg(1) != 0); break;
        case kOpSignal: signalSema(static_cast<int32_t>(arg(0)), arg(1) != 0); break;
        case kOpPollSema: pollSema(static_cast<int32_t>(arg(0))); break;
        case kOpWaitSema: waitSema(static_cast<int32_t>(arg(0))); break;
        case kOpHandlerAdd:
            addHandler(arg(0) != 0, arg(1), arg(2), static_cast<int32_t>(arg(3)), arg(4), arg(5), arg(6));
            break;
        case kOpHandlerRemove: removeHandler(arg(0) != 0, arg(1), static_cast<int32_t>(arg(2))); break;
        case kOpCauseEnable: causeEnable(arg(0) != 0, arg(1), arg(2) != 0); break;
        case kOpFieldRate: fieldRate(arg(0), arg(1)); break;
        case kOpThreadSetup: threadSetup(arg(0), arg(1), arg(2)); break;
        case kOpTransfer: transfer(arg(0) != 0); break;
        default:
            // Not used by TMB (alarms, event flags, suspend/resume...): recorded as made, no effect yet.
            {
                OpEvent e;
                e.op = op;
                e.origin = origin_;
                e.depth = depth_;
                e.originCall = origin_ == kOriginCall ? originCall_ : kNoCall;
                e.wide = wide;
                e.argc = std::min<uint32_t>(argc, 16);
                for (uint32_t i = 0; i < e.argc; ++i)
                    e.args[i] = a[i];
                e.thread = running_;
                (observer_ ? observer_ : &defaultObserver_)->op(e);
            }
            break;
        }
    }

    // ---- Events --------------------------------------------------------------------------------------------------

    void Kernel::raise(bool dmac, uint32_t cause) { irq(dmac, cause); }

    void Kernel::vblankStart()
    {
        ++vblankCount_;
        nextStart_ = static_cast<uint64_t>(vblankCount_ + 1) * fieldCycles_;
        // Threads waiting for this vblank, by thread id (calllog: not in the order they blocked).
        std::vector<int32_t> woken;
        for (const auto &[id, t] : threads_)
            if (t->state == ThreadState::Waiting && t->waitReason == kWaitVblank && vblankCount_ > t->waitToken)
                woken.push_back(id);
        for (int32_t id : woken)
        {
            Thread *t = thread(id);
            const uint32_t result =
                t->fixedResult == -1 ? ((vblankCount_ & 1u) ^ 1u) : static_cast<uint32_t>(t->fixedResult);
            wake(id, result, t->waitCompletion, kWaitVblank);
        }
        raise(false, kIntcVblankStart);
    }

    void Kernel::vblankEnd()
    {
        nextEnd_ = nextStart_ + kVblankLength;
        raise(false, kIntcVblankEnd);
    }

    void Kernel::processEvents(uint64_t cycle)
    {
        while (nextEvent() <= cycle)
            processNextEvent();
    }

    void Kernel::processNextEvent()
    {
        if (nextStart_ <= nextEnd_)
            vblankStart();
        else
            vblankEnd();
    }
}
