// THREADMAN: the IOP's threads, event flags, semaphores and message boxes.
//
// docs/spec/06-iop-kernel.md IOP-3. Four libraries out of one module --
// `thbase`, `thevent`, `thsemap`, `thmsgbx` -- over one scheduler: 128 priorities of
// ready queues, a "current" and a "pending next" thread, and the two hooks
// INTRMAN calls at the tail of an interrupt (IOP-2j): `shouldPreempt` says
// whether the two differ, `newContext` swaps the frames. A thread that has
// to wait sets itself waiting, picks the next thread into "pending", and
// traps into INTRMAN's reschedule syscall, which pushes its frame and lets
// the same two hooks switch; whoever wakes it writes the call's answer into
// that frame's $v0 slot before making it ready (src/iop/context.hpp).
//
// What is ours rather than the reference's (docs/implementation.md): the
// records come from fixed pools rather than a heap, and an id is a pool
// index with a generation rather than a tagged pointer -- a stale id is
// refused just the same (IOP-3c). DelayThread and the alarms need a timer
// manager that does not exist yet and answer -1 until it does (IOP-3j).

#include "context.hpp"
#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;
namespace context = ps2::context;

constexpr uint32_t kThreads = 48;
constexpr uint32_t kEvents = 48;
constexpr uint32_t kSemaphores = 48;
constexpr uint32_t kMailboxes = 32;
constexpr uint32_t kPriorities = 128;
constexpr uint32_t kIdlePriority = 127;         // IOP-3h: below everything creatable
constexpr uint32_t kBootPriority = 1;           // IOP-3i: the boot's current priority
constexpr uint32_t kBootInitialPriority = 8;
constexpr uint32_t kIdleStackBytes = 0x200;
constexpr uint16_t kNone = 0xFFFF;
constexpr uint32_t kNewStatus = 0x404;          // IEp and Im2: interrupts on after rfe

// kerr.h [header]
constexpr int kOk = 0;
constexpr int kError = -1;
constexpr int kIllegalContext = -100;
constexpr int kNoMemory = -400;
constexpr int kIllegalAttr = -401;
constexpr int kIllegalEntry = -402;
constexpr int kIllegalPriority = -403;
constexpr int kIllegalStackSize = -404;
constexpr int kIllegalMode = -405;
constexpr int kIllegalThid = -406;
constexpr int kUnknownThid = -407;
constexpr int kUnknownSemid = -408;
constexpr int kUnknownEvfid = -409;
constexpr int kUnknownMbxid = -410;
constexpr int kDormant = -413;
constexpr int kNotDormant = -414;
constexpr int kNotWait = -416;
constexpr int kReleaseWait = -418;
constexpr int kSemaZero = -419;
constexpr int kEvfCond = -421;
constexpr int kEvfMulti = -422;
constexpr int kEvfIlpat = -423;
constexpr int kMbxNoMsg = -424;
constexpr int kWaitDelete = -425;

// IOP-3d
enum State : uint8_t { Free = 0, Run = 1, Ready = 2, Wait = 4, Dormant = 0x10 };
enum WaitType : uint8_t {
    NotWaiting = 0, Sleep = 1, Delay = 2, OnSema = 3, OnEvent = 4, OnMbx = 5
};

constexpr uint32_t kAttrMask = 0xE3000008;      // IOP-3b
constexpr uint32_t kEventMulti = 2;             // EA_MULTI [header]
constexpr uint32_t kSemaAttrMask = 0x101;
constexpr uint32_t kSemaPriority = 1;           // SA_THPRI [header]
constexpr uint32_t kWaitOr = 1;                 // WEF_OR [header]
constexpr uint32_t kWaitClear = 0x10;           // WEF_CLEAR [header]
constexpr uint32_t kMbxAttrMask = 0x5;
constexpr uint32_t kMbxThreadPriority = 1;      // MBA_THPRI [header]
constexpr uint32_t kMbxMessagePriority = 4;     // MBA_MSPRI [header]

struct Thread {
    uint8_t state;
    uint8_t wait_type;
    uint8_t generation;
    uint8_t in_use;
    int16_t initial_priority;
    int16_t current_priority;
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack;
    uint32_t stack_size;
    uint32_t gp;
    uint32_t *frame;        // where its registers are while it is out
    uint32_t wait_id;       // the id of what it waits on (IOP-3e)
    uint32_t wakeup_count;
    uint32_t *wait_out;     // the blocked call's out-parameter: an event
                            // flag's resulting bits, or a message box's
                            // message. Written only where the wait ends
                            // normally, never when it is broken (IOP-3l).
    uint32_t wait_bits;
    uint32_t wait_mode;
    uint16_t next;          // the queue it is in: ready or a wait list
    uint16_t prev;
};

struct Queue {
    uint16_t head;
    uint16_t tail;
};

struct Event {
    uint8_t in_use;
    uint8_t generation;
    uint32_t attr;
    uint32_t option;
    uint32_t initial_bits;
    uint32_t bits;
    Queue waiters;
    uint32_t waiter_count;
};

struct Semaphore {
    uint8_t in_use;
    uint8_t generation;
    uint32_t attr;
    uint32_t option;
    int32_t initial;
    int32_t max;
    int32_t count;
    Queue waiters;
    uint32_t waiter_count;
};

// IOP-3l: a message is the sender's own memory and the kernel never copies
// it. The queue is threaded through this header's first word, so a client's
// payload follows it and comes back untouched through ReceiveMbx.
struct Message {
    Message *next;
    uint8_t priority;
};

struct Mailbox {
    uint8_t in_use;
    uint8_t generation;
    uint32_t attr;
    uint32_t option;
    Queue waiters;
    uint32_t waiter_count;
    Message *tail;              // the last queued message; the list is circular
    uint32_t message_count;
};

Thread threads[kThreads];
Event events[kEvents];
Semaphore semaphores[kSemaphores];
Mailbox mailboxes[kMailboxes];
Queue ready[kPriorities];
uint32_t ready_bits[kPriorities / 32];
uint32_t current = kNone;
uint32_t pending = kNone;                       // IOP-3h: the "pending next"
alignas(16) uint8_t idle_stack[kIdleStackBytes];

// --- imports ---------------------------------------------------------------

extern "C" {
int _import_sysmem_allocate(uint32_t mode, uint32_t size, uint32_t address);
int _import_intrman_cpu_enable();
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_intrman_query_context();
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_enable(uint32_t irq);
int _import_timrman_alloc(uint32_t source, uint32_t size, uint32_t prescale);
void _import_timrman_set_mode(uint32_t timer_id, uint32_t mode);
uint32_t _import_timrman_get_status(uint32_t timer_id);
void _import_timrman_set_counter(uint32_t timer_id, uint32_t count);
uint32_t _import_timrman_get_counter(uint32_t timer_id);
void _import_timrman_set_compare(uint32_t timer_id, uint32_t compare);
int _import_timrman_intr_code(uint32_t timer_id);
void _import_intrman_set_new_ctx(uint32_t *(*callback)(uint32_t *));
void _import_intrman_set_should_preempt(int (*callback)());
int _import_loadcore_register(void *table);
}

struct Critical {
    uint32_t state;
    Critical() { _import_intrman_suspend(&state); }
    ~Critical() { _import_intrman_resume(state); }
};

// --- ids (IOP-3c) -------------------------------------------------------------

// An id names a pool slot and the generation that slot had when the object
// was made, so that a released slot's old ids are refused.
[[nodiscard]] uint32_t makeId(uint32_t index, uint8_t generation) {
    return ((index + 1) << 8) | generation;
}

[[nodiscard]] Thread *threadOf(uint32_t id) {
    const uint32_t index = (id >> 8) - 1;
    if (id == 0 || index >= kThreads || !threads[index].in_use
        || threads[index].generation != (id & 0xFF)) {
        return nullptr;
    }
    return &threads[index];
}

[[nodiscard]] uint32_t indexOf(const Thread *thread) {
    return static_cast<uint32_t>(thread - threads);
}

[[nodiscard]] uint32_t idOf(const Thread *thread) {
    return makeId(indexOf(thread), thread->generation);
}

[[nodiscard]] Event *eventOf(uint32_t id) {
    const uint32_t index = (id >> 8) - 1;
    if (id == 0 || index >= kEvents || !events[index].in_use
        || events[index].generation != (id & 0xFF)) {
        return nullptr;
    }
    return &events[index];
}

[[nodiscard]] Mailbox *mailboxOf(uint32_t id) {
    const uint32_t index = (id >> 8) - 1;
    if (id == 0 || index >= kMailboxes || !mailboxes[index].in_use
        || mailboxes[index].generation != (id & 0xFF)) {
        return nullptr;
    }
    return &mailboxes[index];
}

[[nodiscard]] Semaphore *semaphoreOf(uint32_t id) {
    const uint32_t index = (id >> 8) - 1;
    if (id == 0 || index >= kSemaphores || !semaphores[index].in_use
        || semaphores[index].generation != (id & 0xFF)) {
        return nullptr;
    }
    return &semaphores[index];
}

// --- queues -----------------------------------------------------------------

void enqueue(Queue &queue, uint32_t index) {
    Thread &thread = threads[index];
    thread.next = kNone;
    thread.prev = queue.tail;
    if (queue.tail == kNone) {
        queue.head = static_cast<uint16_t>(index);
    } else {
        threads[queue.tail].next = static_cast<uint16_t>(index);
    }
    queue.tail = static_cast<uint16_t>(index);
}

// SA_THPRI (IOP-3g): in front of the first waiter of a worse priority.
void enqueueByPriority(Queue &queue, uint32_t index) {
    Thread &thread = threads[index];
    uint16_t at = queue.head;
    while (at != kNone && threads[at].current_priority <= thread.current_priority) {
        at = threads[at].next;
    }
    if (at == kNone) {
        enqueue(queue, index);
        return;
    }
    thread.next = at;
    thread.prev = threads[at].prev;
    if (threads[at].prev == kNone) {
        queue.head = static_cast<uint16_t>(index);
    } else {
        threads[threads[at].prev].next = static_cast<uint16_t>(index);
    }
    threads[at].prev = static_cast<uint16_t>(index);
}

void unlink(Queue &queue, uint32_t index) {
    Thread &thread = threads[index];
    if (thread.prev == kNone) {
        queue.head = thread.next;
    } else {
        threads[thread.prev].next = thread.next;
    }
    if (thread.next == kNone) {
        queue.tail = thread.prev;
    } else {
        threads[thread.next].prev = thread.prev;
    }
    thread.next = kNone;
    thread.prev = kNone;
}

[[nodiscard]] uint32_t popFront(Queue &queue) {
    const uint16_t index = queue.head;
    if (index != kNone) {
        unlink(queue, index);
    }
    return index;
}

void makeReady(uint32_t index) {
    Thread &thread = threads[index];
    thread.state = Ready;
    thread.wait_type = NotWaiting;
    const auto priority = static_cast<uint32_t>(thread.current_priority);
    enqueue(ready[priority], index);
    ready_bits[priority / 32] |= 1u << (priority % 32);
}

// A thread preempted by an interrupt's wake-up gave nothing up: it goes back
// to the head of its queue, ahead of its peers, not behind them.
void makeReadyFirst(uint32_t index) {
    Thread &thread = threads[index];
    thread.state = Ready;
    thread.wait_type = NotWaiting;
    const auto priority = static_cast<uint32_t>(thread.current_priority);
    Queue &queue = ready[priority];
    thread.prev = kNone;
    thread.next = queue.head;
    if (queue.head == kNone) {
        queue.tail = static_cast<uint16_t>(index);
    } else {
        threads[queue.head].prev = static_cast<uint16_t>(index);
    }
    queue.head = static_cast<uint16_t>(index);
    ready_bits[priority / 32] |= 1u << (priority % 32);
}

void takeOffReady(uint32_t index) {
    const auto priority = static_cast<uint32_t>(threads[index].current_priority);
    unlink(ready[priority], index);
    if (ready[priority].head == kNone) {
        ready_bits[priority / 32] &= ~(1u << (priority % 32));
    }
}

// IOP-3d: the lowest set bit of the bitmap is the best non-empty queue.
[[nodiscard]] uint32_t pick() {
    for (uint32_t word = 0; word < kPriorities / 32; word++) {
        if (ready_bits[word] == 0) {
            continue;
        }
        uint32_t bit = 0;
        while ((ready_bits[word] & (1u << bit)) == 0) {
            bit++;
        }
        const uint32_t priority = word * 32 + bit;
        const uint32_t index = popFront(ready[priority]);
        if (ready[priority].head == kNone) {
            ready_bits[word] &= ~(1u << bit);
        }
        return index;
    }
    return kNone;                               // never, once the idle thread exists
}

// The thread's wait object: the list it is linked into and the count that
// list keeps, for taking it back off wherever it waits.
struct WaitObject {
    Queue *queue;
    uint32_t *count;
};

WaitObject waitObjectOf(Thread &thread) {
    switch (thread.wait_type) {
    case OnSema: {
        Semaphore *sema = semaphoreOf(thread.wait_id);
        return sema != nullptr ? WaitObject{&sema->waiters, &sema->waiter_count}
                               : WaitObject{nullptr, nullptr};
    }
    case OnEvent: {
        Event *event = eventOf(thread.wait_id);
        return event != nullptr ? WaitObject{&event->waiters, &event->waiter_count}
                                : WaitObject{nullptr, nullptr};
    }
    case OnMbx: {
        Mailbox *mailbox = mailboxOf(thread.wait_id);
        return mailbox != nullptr ? WaitObject{&mailbox->waiters, &mailbox->waiter_count}
                                  : WaitObject{nullptr, nullptr};
    }
    default:
        return {nullptr, nullptr};
    }
}

void cancelDelay(Thread &thread);

void leaveWait(uint32_t index) {
    Thread &thread = threads[index];
    if (thread.wait_type == Delay) {
        cancelDelay(thread);                    // IOP-3k: its alarm goes with it
    }
    const WaitObject object = waitObjectOf(thread);
    if (object.queue != nullptr) {
        unlink(*object.queue, index);
        (*object.count)--;
    }
    thread.wait_type = NotWaiting;
}

// --- the switch (IOP-3h) ------------------------------------------------------

// What a blocked call answers when its thread is resumed: the frame's $v0.
void setAnswer(Thread &thread, int32_t value) {
    if (thread.frame != nullptr) {
        thread.frame[context::slotOf(2)] = static_cast<uint32_t>(value);
    }
}

// IOP-2j/IOP-3h: the hooks. `newContext` runs inside INTRMAN's exception,
// with the outgoing frame just pushed; it keeps that frame, resolves the
// pending choice, and hands back the frame to resume.
int shouldPreempt() {
    return pending != current;
}

uint32_t *newContext(uint32_t *frame) {
    if (current != kNone) {
        threads[current].frame = frame;
    }
    if (pending == kNone || pending == current) {
        pending = pick();
        if (pending == kNone) {
            pending = current;              // nothing ready: IOP-3h, stay
        }
    }
    current = pending;
    Thread &next = threads[current];
    next.state = Run;
    return next.frame;
}

// Trap into INTRMAN's reschedule syscall (IOP-3h). The frame it pushes is
// this thread's; what comes back in $v0 is whatever the waker wrote there.
[[nodiscard]] int32_t switchNow() {
    int32_t answer;
    asm volatile(
        // IOP-3h: the number goes in $v0 and the `syscall` carries no code.
        "addiu $v0, $zero, 0x20\n\t"
        "syscall\n\t"
        "move %0, $v0"
        : "=r"(answer)
        :
        : "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12",
          "$13", "$14", "$15", "$24", "$25", "$31", "memory");
    return answer;
}

// The caller blocks on its wait object; the pick decides who runs meanwhile.
// Interrupts are held off by the caller's critical section up to the trap,
// whose exception closes them for the rest.
[[nodiscard]] int32_t blockCurrent(uint8_t wait_type, uint32_t wait_id) {
    Thread &thread = threads[current];
    thread.state = Wait;
    thread.wait_type = wait_type;
    thread.wait_id = wait_id;
    pending = pick();
    return switchNow();
}

// A thread has been made ready by the running one (never from an interrupt,
// whose exit asks the hooks itself): hand over at once if it is better
// (IOP-3h's "immediate hand-off on enqueue").
void handOffIfBetter(uint32_t index) {
    if (current == kNone || threads[index].current_priority >= threads[current].current_priority) {
        return;
    }
    Thread &running = threads[current];
    running.state = Ready;
    const auto priority = static_cast<uint32_t>(running.current_priority);
    enqueue(ready[priority], current);
    ready_bits[priority / 32] |= 1u << (priority % 32);
    takeOffReady(index);
    pending = index;
    (void)switchNow();
}

// Wake `index` with `answer`: off its wait list, onto the ready queue, and
// -- from an interrupt -- noted as the pending next if it is better than the
// interrupted thread, for the interrupt's exit to act on.
void wake(uint32_t index, int32_t answer, bool from_interrupt) {
    Thread &thread = threads[index];
    leaveWait(index);
    setAnswer(thread, answer);
    makeReady(index);
    if (from_interrupt) {
        if (current != kNone && thread.current_priority < threads[current].current_priority
            && (pending == kNone || pending == current
                || thread.current_priority < threads[pending].current_priority)) {
            if (pending == kNone || pending == current) {
                makeReadyFirst(current);    // preempted, not blocked
            } else {
                makeReady(pending);
            }
            takeOffReady(index);
            pending = index;
        }
    } else {
        handOffIfBetter(index);
    }
}

// --- the idle thread, and a thread's end ------------------------------------

[[noreturn]] void idleLoop(void *) {
    for (;;) {
        asm volatile("" ::: "memory");
    }
}

int exitThread();

// A frame for a thread about to run from its entry (IOP-3e): EPC at the
// entry, the argument in $a0, its $gp and stack, and `ExitThread` as what a
// returning function lands in.
void primeFrame(Thread &thread, uint32_t arg) {
    const uint32_t top = (thread.stack + thread.stack_size) & ~uint32_t{15};
    auto *frame = reinterpret_cast<uint32_t *>(top - context::kBytes);
    for (uint32_t k = 0; k < context::kWords; k++) {
        frame[k] = 0;
    }
    frame[context::kTag] = context::kTagFull;
    frame[context::kEpc] = reinterpret_cast<uintptr_t>(thread.entry);
    frame[context::kStatus] = kNewStatus;
    frame[context::kIntCtrl] = 1;           // IOP-2e: the gate open, as THREADMAN 2.03 primes it
    frame[context::slotOf(4)] = arg;
    frame[context::slotOf(28)] = thread.gp;
    frame[context::slotOf(29)] = top - 16;
    frame[context::slotOf(30)] = top - 16;
    frame[context::slotOf(31)] = reinterpret_cast<uintptr_t>(exitThread);
    thread.frame = frame;
}

[[nodiscard]] uint32_t currentGp() {
    uint32_t gp;
    asm volatile("move %0, $gp" : "=r"(gp));
    return gp;
}

// --- thbase ---------------------------------------------------------------------

struct ThreadParameters {
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack_size;
    uint32_t priority;
};

// IOP-3b: validated in the reference's order, each failure returning at once.
int createThread(ThreadParameters *parameters) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    if ((parameters->attr & ~kAttrMask) != 0) {
        return kIllegalAttr;
    }
    if (parameters->priority < 1 || parameters->priority > 126) {
        return kIllegalPriority;
    }
    if ((reinterpret_cast<uintptr_t>(parameters->entry) & 3) != 0) {
        return kIllegalEntry;
    }
    if (parameters->stack_size < 0x130) {
        return kIllegalStackSize;
    }
    parameters->stack_size = (parameters->stack_size + 0xFF) & ~uint32_t{0xFF};

    Critical critical;
    uint32_t index = kNone;
    for (uint32_t k = 0; k < kThreads; k++) {
        if (!threads[k].in_use) {
            index = k;
            break;
        }
    }
    if (index == kNone) {
        return kNoMemory;
    }
    const auto stack = static_cast<uint32_t>(
        _import_sysmem_allocate(1, parameters->stack_size, 0));
    if (stack == 0) {
        return kNoMemory;
    }
    Thread &thread = threads[index];
    thread.in_use = 1;
    thread.generation++;
    thread.state = Dormant;
    thread.wait_type = NotWaiting;
    thread.attr = parameters->attr;
    thread.option = parameters->option;
    thread.entry = parameters->entry;
    thread.stack = stack;
    thread.stack_size = parameters->stack_size;
    thread.gp = currentGp();
    thread.initial_priority = static_cast<int16_t>(parameters->priority);
    thread.current_priority = thread.initial_priority;
    thread.frame = nullptr;
    thread.wakeup_count = 0;
    thread.next = kNone;
    thread.prev = kNone;
    return static_cast<int>(idOf(&thread));
}

int deleteThread(uint32_t id) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Thread *thread = threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    if (thread->state != Dormant) {
        return kNotDormant;
    }
    thread->in_use = 0;
    thread->state = Free;
    return kOk;
}

int startThread(uint32_t id, uint32_t arg) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Thread *thread = threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    if (thread->state != Dormant) {
        return kNotDormant;
    }
    thread->current_priority = thread->initial_priority;
    primeFrame(*thread, arg);
    const uint32_t index = indexOf(thread);
    makeReady(index);
    handOffIfBetter(index);
    return kOk;
}

int startThreadArgs(uint32_t id, uint32_t argc, void *argv) {
    (void)argc;
    return startThread(id, reinterpret_cast<uintptr_t>(argv));
}

// IOP-3e: never returns. The thread goes dormant and the pick decides.
int exitThread() {
    Critical critical;
    Thread &thread = threads[current];
    thread.state = Dormant;
    thread.wait_type = NotWaiting;
    pending = pick();
    (void)switchNow();
    for (;;) {
    }
}

int terminateThread(uint32_t id, bool from_interrupt) {
    if (id == 0) {
        return kIllegalThid;
    }
    Critical critical;
    Thread *thread = threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    const uint32_t index = indexOf(thread);
    if (index == current) {
        return kIllegalThid;
    }
    if (thread->state == Dormant) {
        return kDormant;
    }
    if (thread->state == Ready) {
        takeOffReady(index);
    } else if (thread->state == Wait) {
        leaveWait(index);
    }
    if (pending == index) {
        pending = current;
    }
    thread->state = Dormant;
    (void)from_interrupt;
    return kOk;
}

int terminateThreadNormal(uint32_t id) {
    return terminateThread(id, false);
}

int terminateThreadInterrupt(uint32_t id) {
    return terminateThread(id, true);
}

int stubError() {
    return kError;                              // IOP-3a: the reference's stubs
}

int changeThreadPriority(uint32_t id, uint32_t priority, bool from_interrupt) {
    if (priority < 1 || priority > 126) {
        return kIllegalPriority;
    }
    Critical critical;
    Thread *thread = id == 0 ? &threads[current] : threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    if (thread->state == Dormant) {
        return kDormant;
    }
    const int previous = thread->current_priority;
    const uint32_t index = indexOf(thread);
    if (thread->state == Ready) {
        takeOffReady(index);
        thread->current_priority = static_cast<int16_t>(priority);
        makeReady(index);
        if (!from_interrupt) {
            handOffIfBetter(index);
        }
    } else {
        thread->current_priority = static_cast<int16_t>(priority);
        if (index == current && !from_interrupt) {
            // A running thread that lowered itself yields to a better one.
            const uint32_t best = pick();
            if (best != kNone) {
                if (threads[best].current_priority < thread->current_priority) {
                    thread->state = Ready;
                    makeReady(index);
                    pending = best;
                    (void)switchNow();
                } else {
                    makeReady(best);
                }
            }
        }
    }
    return previous;
}

int changeThreadPriorityNormal(uint32_t id, uint32_t priority) {
    return changeThreadPriority(id, priority, false);
}

int changeThreadPriorityInterrupt(uint32_t id, uint32_t priority) {
    return changeThreadPriority(id, priority, true);
}

int rotateThreadReadyQueue(uint32_t priority, bool from_interrupt) {
    if (priority > 127) {
        return kIllegalPriority;
    }
    Critical critical;
    if (priority == 0) {
        priority = static_cast<uint32_t>(threads[current].current_priority);
    }
    if (!from_interrupt && current != kNone
        && static_cast<uint32_t>(threads[current].current_priority) == priority
        && ready[priority].head != kNone) {
        // The running thread goes to its queue's tail and the head runs.
        Thread &running = threads[current];
        running.state = Ready;
        makeReady(current);
        pending = pick();
        (void)switchNow();
        return kOk;
    }
    const uint32_t head = popFront(ready[priority]);
    if (head != kNone) {
        enqueue(ready[priority], head);
    }
    return kOk;
}

int rotateThreadReadyQueueNormal(uint32_t priority) {
    return rotateThreadReadyQueue(priority, false);
}

int rotateThreadReadyQueueInterrupt(uint32_t priority) {
    return rotateThreadReadyQueue(priority, true);
}

int releaseWaitThread(uint32_t id, bool from_interrupt) {
    if (id == 0) {
        return kIllegalThid;
    }
    Critical critical;
    Thread *thread = threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    if (thread->state == Dormant) {
        return kDormant;
    }
    if (thread->state != Wait) {
        return kNotWait;
    }
    wake(indexOf(thread), kReleaseWait, from_interrupt);
    return kOk;
}

int releaseWaitThreadNormal(uint32_t id) {
    return releaseWaitThread(id, false);
}

int releaseWaitThreadInterrupt(uint32_t id) {
    return releaseWaitThread(id, true);
}

int getThreadId() {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    return static_cast<int>(idOf(&threads[current]));
}

int checkThreadStack() {
    uint32_t sp;
    asm volatile("move %0, $sp" : "=r"(sp));
    const Thread &thread = threads[current];
    return static_cast<int>(sp - thread.stack);
}

// IOP-3e: the SDK's iop_thread_info_t [header], seventeen words.
struct ThreadInfo {
    uint32_t attr;
    uint32_t option;
    uint32_t status;
    void (*entry)(void *);
    uint32_t stack;
    uint32_t stack_size;
    uint32_t gp;
    uint32_t initial_priority;
    uint32_t current_priority;
    uint32_t wait_type;
    uint32_t wait_id;
    uint32_t wakeup_count;
    uint32_t reg_context;
    uint32_t reserved[4];
};

int referThreadStatus(uint32_t id, ThreadInfo *info) {
    Critical critical;
    Thread *thread = id == 0 ? &threads[current] : threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    auto *words = reinterpret_cast<uint32_t *>(info);
    for (uint32_t k = 0; k < sizeof(ThreadInfo) / 4; k++) {
        words[k] = 0;
    }
    info->attr = thread->attr;
    info->option = thread->option;
    info->status = thread->state;
    info->entry = thread->entry;
    info->stack = thread->stack;
    info->stack_size = thread->stack_size;
    info->gp = thread->gp;
    info->initial_priority = static_cast<uint32_t>(thread->initial_priority);
    info->current_priority = static_cast<uint32_t>(thread->current_priority);
    if (thread->state == Wait) {
        info->wait_type = thread->wait_type;
        info->wait_id = thread->wait_id;
    }
    info->wakeup_count = thread->wakeup_count;
    info->reg_context = reinterpret_cast<uintptr_t>(thread->frame);
    return kOk;
}

// IOP-3e: a pending wakeup is consumed without blocking.
int sleepThread() {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Thread &thread = threads[current];
    if (thread.wakeup_count > 0) {
        thread.wakeup_count--;
        return kOk;
    }
    return blockCurrent(Sleep, 0);
}

int wakeupThread(uint32_t id, bool from_interrupt) {
    if (id == 0) {
        return kIllegalThid;
    }
    Critical critical;
    Thread *thread = threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    if (thread->state == Dormant) {
        return kDormant;
    }
    if (thread->state == Wait && thread->wait_type == Sleep) {
        wake(indexOf(thread), kOk, from_interrupt);
    } else {
        thread->wakeup_count++;
    }
    return kOk;
}

int wakeupThreadNormal(uint32_t id) {
    return wakeupThread(id, false);
}

int wakeupThreadInterrupt(uint32_t id) {
    return wakeupThread(id, true);
}

int cancelWakeupThread(uint32_t id) {
    Critical critical;
    Thread *thread = id == 0 ? &threads[current] : threadOf(id);
    if (thread == nullptr) {
        return kUnknownThid;
    }
    const int previous = static_cast<int>(thread->wakeup_count);
    thread->wakeup_count = 0;
    return previous;
}

// --- IOP-3j and IOP-3k: the clock and the alarms (docs/analysis/44) ---------

// The clock is timer 5's 32-bit counter, which the reference's allocation of
// a 32-bit SYSCLOCK timer yields (IOP-7c), counting at 36.864 MHz, with a
// software high word advanced whenever the counter is seen to have wrapped.
// The overflow interrupt (mode 0x70) guarantees that it is seen at least once
// per wrap.
constexpr uint32_t kClockSource = 1;            // TC_SYSCLOCK [header]
constexpr uint32_t kClockNumerator = 4608;      // ticks = usec * 4608 / 125
constexpr uint32_t kClockDenominator = 125;
constexpr uint32_t kTimerMode = 0x70;           // interrupt on target and overflow, no reset
constexpr uint32_t kStatusTarget = 0x800;
constexpr uint32_t kGranularityUsec = 100;      // IOP-3k: the shortest alarm
constexpr uint32_t kSeedUsec = 2000;            // the permanent record starts overdue
constexpr uint32_t kAlarms = 32;
constexpr uint16_t kAlarmNone = 0xFFFF;
constexpr int kAlarmDuplicate = -104;           // KE_FOUND_HANDLER [header]
constexpr int kAlarmUnknown = -105;             // KE_NOTFOUND_HANDLER [header]

struct Clock {
    uint32_t lo;
    uint32_t hi;
};

[[nodiscard]] bool clockBefore(const Clock &a, const Clock &b) {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

[[nodiscard]] Clock clockAdd(const Clock &a, const Clock &b) {
    Clock sum{a.lo + b.lo, a.hi + b.hi};
    if (sum.lo < a.lo) {
        sum.hi++;
    }
    return sum;
}

[[nodiscard]] Clock clockSubtract(const Clock &a, const Clock &b) {
    Clock difference{a.lo - b.lo, a.hi - b.hi};
    if (a.lo < b.lo) {
        difference.hi--;
    }
    return difference;
}

// (hi:lo) / divisor, a bit at a time: the R3000 has no 64-bit divide, and
// the library one is not linked here.
[[nodiscard]] Clock clockDivide(Clock value, uint32_t divisor, uint32_t *remainder_out) {
    Clock quotient{0, 0};
    uint32_t remainder = 0;
    for (int bit = 63; bit >= 0; bit--) {
        const uint32_t top = bit >= 32 ? (value.hi >> (bit - 32)) & 1 : (value.lo >> bit) & 1;
        remainder = (remainder << 1) | top;
        if (remainder >= divisor) {
            remainder -= divisor;
            if (bit >= 32) {
                quotient.hi |= 1u << (bit - 32);
            } else {
                quotient.lo |= 1u << bit;
            }
        }
    }
    if (remainder_out != nullptr) {
        *remainder_out = remainder;
    }
    return quotient;
}

[[nodiscard]] Clock clockMultiply(const Clock &value, uint32_t factor) {
    const unsigned long long low = static_cast<unsigned long long>(value.lo) * factor;
    Clock product{static_cast<uint32_t>(low), static_cast<uint32_t>(low >> 32)};
    product.hi += value.hi * factor;
    return product;
}

// Ordinal 39: usec * 4608 / 125, in 64 bits.
void usecToSysClock(uint32_t usec, uint32_t *clock) {
    const Clock ticks = clockDivide(clockMultiply({usec, 0}, kClockNumerator),
                                    kClockDenominator, nullptr);
    clock[0] = ticks.lo;
    clock[1] = ticks.hi;
}

// Ordinal 40: the inverse, split into seconds and the rest.
void sysClockToUsec(const uint32_t *clock, uint32_t *sec, uint32_t *usec) {
    const Clock total = clockDivide(clockMultiply({clock[0], clock[1]}, kClockDenominator),
                                    kClockNumerator, nullptr);
    uint32_t rest;
    const Clock seconds = clockDivide(total, 1000000, &rest);
    if (sec != nullptr) {
        *sec = seconds.lo;
    }
    if (usec != nullptr) {
        *usec = rest;
    }
}

struct Alarm {
    Clock deadline;
    int (*callback)(void *);
    void *arg;
    uint16_t next;
    uint16_t prev;
    bool in_use;
    bool permanent;         // IOP-3k: the record that keeps the clock fresh
};

Alarm alarms[kAlarms];
uint16_t alarm_head = kAlarmNone;
uint32_t timer_id;
Clock clock_seen;                               // the last reading: its high word is ours
uint32_t ticks_granularity;                     // 100 us of ticks
uint32_t ticks_coalesce;                        // 200 us: alarms closer than this share one target

// Now, as (hi:lo): the counter and the wraps counted so far.
[[nodiscard]] Clock readClock() {
    const uint32_t low = _import_timrman_get_counter(timer_id);
    if (low < clock_seen.lo) {
        clock_seen.hi++;
    }
    clock_seen.lo = low;
    return clock_seen;
}

// Ordinal 34: GetSystemTime(out). The reference's own callers all pass the
// pointer and take the value from it; the return is 0 (docs/analysis/44 §7).
int getSystemTime(uint32_t *out) {
    Critical critical;
    const Clock now = readClock();
    if (out != nullptr) {
        out[0] = now.lo;
        out[1] = now.hi;
    }
    return kOk;
}

void unlinkAlarm(uint16_t index) {
    Alarm &alarm = alarms[index];
    if (alarm.prev == kAlarmNone) {
        alarm_head = alarm.next;
    } else {
        alarms[alarm.prev].next = alarm.next;
    }
    if (alarm.next != kAlarmNone) {
        alarms[alarm.next].prev = alarm.prev;
    }
    alarm.next = kAlarmNone;
    alarm.prev = kAlarmNone;
}

// IOP-3k: ascending by deadline, after any equal one.
void insertAlarm(uint16_t index) {
    Alarm &alarm = alarms[index];
    uint16_t after = kAlarmNone;
    for (uint16_t at = alarm_head; at != kAlarmNone; at = alarms[at].next) {
        if (clockBefore(alarm.deadline, alarms[at].deadline)) {
            break;
        }
        after = at;
    }
    alarm.prev = after;
    if (after == kAlarmNone) {
        alarm.next = alarm_head;
        alarm_head = index;
    } else {
        alarm.next = alarms[after].next;
        alarms[after].next = index;
    }
    if (alarm.next != kAlarmNone) {
        alarms[alarm.next].prev = index;
    }
}

[[nodiscard]] uint16_t findAlarm(int (*callback)(void *), void *arg) {
    for (uint16_t at = alarm_head; at != kAlarmNone; at = alarms[at].next) {
        if (alarms[at].callback == callback && alarms[at].arg == arg) {
            return at;
        }
    }
    return kAlarmNone;
}

[[nodiscard]] uint16_t allocateAlarm() {
    for (uint16_t k = 0; k < kAlarms; k++) {
        if (!alarms[k].in_use) {
            alarms[k].in_use = true;
            alarms[k].permanent = false;
            return k;
        }
    }
    return kAlarmNone;
}

// IOP-3k: the compare register follows the queue's head -- or the last of a
// run of heads within 200 us of each other, so that they share one
// interrupt -- and a deadline already passed is replaced by 200 us from now.
void armTimer() {
    if (alarm_head == kAlarmNone) {
        return;
    }
    uint16_t target = alarm_head;
    const Clock window{ticks_coalesce, 0};
    while (alarms[target].next != kAlarmNone
           && !clockBefore(clockAdd(alarms[target].deadline, window),
                           alarms[alarms[target].next].deadline)) {
        target = alarms[target].next;
    }
    const Clock now = readClock();
    uint32_t compare;
    if (clockBefore(now, alarms[target].deadline)) {
        compare = alarms[target].deadline.lo;
    } else {
        compare = _import_timrman_get_counter(timer_id) + ticks_coalesce;
    }
    _import_timrman_set_compare(timer_id, compare);
}

// The shared insert: `delta` from now, the 100 us floor applied.
[[nodiscard]] int addAlarm(const uint32_t *delta, int (*callback)(void *), void *arg) {
    if (findAlarm(callback, arg) != kAlarmNone) {
        return kAlarmDuplicate;
    }
    const uint16_t index = allocateAlarm();
    if (index == kAlarmNone) {
        return kNoMemory;
    }
    Clock wait{delta[0], delta[1]};
    if (wait.hi == 0 && wait.lo < ticks_granularity) {
        wait.lo = ticks_granularity;
    }
    Alarm &alarm = alarms[index];
    alarm.deadline = clockAdd(readClock(), wait);
    alarm.callback = callback;
    alarm.arg = arg;
    insertAlarm(index);
    armTimer();
    return kOk;
}

[[nodiscard]] int removeAlarm(int (*callback)(void *), void *arg) {
    const uint16_t index = findAlarm(callback, arg);
    if (index == kAlarmNone) {
        return kAlarmUnknown;
    }
    unlinkAlarm(index);
    alarms[index].in_use = false;
    return kOk;
}

// Ordinals 35-38. SetAlarm and CancelAlarm want a thread; their i-forms
// want an interrupt (IOP-3k), and skip the critical section their caller
// already holds.
int setAlarm(const uint32_t *clock, int (*callback)(void *), void *arg) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    return addAlarm(clock, callback, arg);
}

int setAlarmInterrupt(const uint32_t *clock, int (*callback)(void *), void *arg) {
    if (!_import_intrman_query_context()) {
        return kIllegalContext;
    }
    return addAlarm(clock, callback, arg);
}

int cancelAlarm(int (*callback)(void *), void *arg) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    return removeAlarm(callback, arg);
}

int cancelAlarmInterrupt(int (*callback)(void *), void *arg) {
    if (!_import_intrman_query_context()) {
        return kIllegalContext;
    }
    return removeAlarm(callback, arg);
}

// IOP-3j: the delayed thread's alarm, and what expires it.
int delayExpired(void *arg) {
    auto *thread = static_cast<Thread *>(arg);
    if (thread->state == Wait && thread->wait_type == Delay) {
        wake(static_cast<uint32_t>(thread - threads), kOk, true);
    }
    return 0;                                   // one shot: the record is freed
}

void cancelDelay(Thread &thread) {
    (void)removeAlarm(delayExpired, &thread);
}

int delayThread(uint32_t usec) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    uint32_t clock[2];
    usecToSysClock(usec, clock);
    const int added = addAlarm(clock, delayExpired, &threads[current]);
    if (added != kOk) {
        return added;
    }
    return blockCurrent(Delay, 0);
}

// IOP-3k: the timer's interrupt. The overflow half is answered by reading
// the clock; the target half walks the due alarms in order, re-arming the
// permanent record a counter period on, calling the others and keeping
// those whose callback asks for more -- at most 200 us more per call.
int timerHandler(void *) {
    const uint32_t status = _import_timrman_get_status(timer_id);
    (void)readClock();
    if ((status & kStatusTarget) == 0) {
        return 1;
    }
    while (alarm_head != kAlarmNone) {
        const uint16_t index = alarm_head;
        Alarm &alarm = alarms[index];
        if (clockBefore(readClock(), alarm.deadline)) {
            break;
        }
        unlinkAlarm(index);
        if (alarm.permanent) {
            alarm.deadline.hi++;
            insertAlarm(index);
            continue;
        }
        const int again = alarm.callback(alarm.arg);
        if (again == 0) {
            alarm.in_use = false;
            continue;
        }
        const uint32_t more = static_cast<uint32_t>(again) < ticks_coalesce
                                  ? static_cast<uint32_t>(again)
                                  : ticks_coalesce;
        alarm.deadline = clockAdd(alarm.deadline, {more, 0});
        insertAlarm(index);
    }
    armTimer();
    return 1;
}

// IOP-3j: at the entry -- the timer THREADMAN's own boot takes, programmed
// as the reference programs it: counter 0, the first target 100 us out,
// mode 0x70, and the permanent record seeded 2 ms overdue so the first
// interrupt walks the queue.
void startClock() {
    timer_id = static_cast<uint32_t>(_import_timrman_alloc(kClockSource, 32, 1));
    uint32_t clock[2];
    usecToSysClock(kGranularityUsec, clock);
    ticks_granularity = clock[0];
    ticks_coalesce = clock[0] * 2;
    clock_seen = {0, 0};
    for (Alarm &alarm : alarms) {
        alarm.in_use = false;
    }
    alarm_head = kAlarmNone;
    const uint16_t permanent = allocateAlarm();
    alarms[permanent].permanent = true;
    alarms[permanent].callback = nullptr;
    alarms[permanent].arg = nullptr;
    usecToSysClock(kSeedUsec, clock);
    alarms[permanent].deadline = clockSubtract({0, 0}, {clock[0], clock[1]});
    insertAlarm(permanent);
    const int irq = _import_timrman_intr_code(timer_id);
    _import_intrman_register(static_cast<uint32_t>(irq), 1, timerHandler, nullptr);
    _import_timrman_set_counter(timer_id, 0);
    _import_timrman_set_compare(timer_id, ticks_granularity);
    _import_timrman_set_mode(timer_id, kTimerMode);
    _import_intrman_enable(static_cast<uint32_t>(irq));
}

uint32_t getSystemStatusFlag() {
    return 0;
}

// --- thevent (IOP-3f) -----------------------------------------------------------

struct EventParameters {
    uint32_t attr;
    uint32_t option;
    uint32_t bits;
};

struct EventInfo {
    uint32_t attr;
    uint32_t option;
    uint32_t initial_bits;
    uint32_t bits;
    uint32_t waiters;
};

int createEventFlag(const EventParameters *parameters) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    if ((parameters->attr & ~kEventMulti) != 0) {
        return kIllegalAttr;
    }
    Critical critical;
    for (uint32_t k = 0; k < kEvents; k++) {
        Event &event = events[k];
        if (event.in_use) {
            continue;
        }
        event.in_use = 1;
        event.generation++;
        event.attr = parameters->attr;
        event.option = parameters->option;
        event.initial_bits = parameters->bits;
        event.bits = parameters->bits;
        event.waiters = {kNone, kNone};
        event.waiter_count = 0;
        return static_cast<int>(makeId(k, event.generation));
    }
    return kNoMemory;
}

// Every waiter is released with the delete answer (IOP-3f).
int deleteEventFlag(uint32_t id) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Event *event = eventOf(id);
    if (event == nullptr) {
        return kUnknownEvfid;
    }
    while (event->waiters.head != kNone) {
        wake(event->waiters.head, kWaitDelete, true);
    }
    event->in_use = 0;
    return kOk;
}

[[nodiscard]] bool eventSatisfied(uint32_t bits, uint32_t wanted, uint32_t mode) {
    return (mode & kWaitOr) != 0 ? (bits & wanted) != 0 : (bits & wanted) == wanted;
}

int setEventFlag(uint32_t id, uint32_t bits, bool from_interrupt) {
    Critical critical;
    Event *event = eventOf(id);
    if (event == nullptr) {
        return kUnknownEvfid;
    }
    if (bits == 0) {
        return kOk;
    }
    event->bits |= bits;
    uint16_t at = event->waiters.head;
    while (at != kNone) {
        const uint16_t following = threads[at].next;
        Thread &waiter = threads[at];
        if (eventSatisfied(event->bits, waiter.wait_bits, waiter.wait_mode)) {
            if (waiter.wait_out != nullptr) {
                *waiter.wait_out = event->bits;
            }
            if (waiter.wait_mode & kWaitClear) {
                event->bits = 0;
            }
            wake(at, kOk, from_interrupt);
        }
        at = following;
    }
    return kOk;
}

int setEventFlagNormal(uint32_t id, uint32_t bits) {
    return setEventFlag(id, bits, false);
}

int setEventFlagInterrupt(uint32_t id, uint32_t bits) {
    return setEventFlag(id, bits, true);
}

// IOP-3f: the argument is a keep-mask.
int clearEventFlag(uint32_t id, uint32_t keep) {
    Critical critical;
    Event *event = eventOf(id);
    if (event == nullptr) {
        return kUnknownEvfid;
    }
    event->bits &= keep;
    return kOk;
}

int waitEventFlag(uint32_t id, uint32_t bits, uint32_t mode, uint32_t *result, bool poll) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    if ((mode & ~(kWaitOr | kWaitClear)) != 0) {
        return kIllegalMode;
    }
    if (bits == 0) {
        return kEvfIlpat;
    }
    Critical critical;
    Event *event = eventOf(id);
    if (event == nullptr) {
        return kUnknownEvfid;
    }
    if ((event->attr & kEventMulti) == 0 && event->waiter_count != 0) {
        return kEvfMulti;
    }
    if (eventSatisfied(event->bits, bits, mode)) {
        if (result != nullptr) {
            *result = event->bits;
        }
        if (mode & kWaitClear) {
            event->bits = 0;
        }
        return kOk;
    }
    if (poll) {
        return kEvfCond;
    }
    Thread &thread = threads[current];
    thread.wait_bits = bits;
    thread.wait_mode = mode;
    thread.wait_out = result;
    enqueue(event->waiters, current);
    event->waiter_count++;
    return blockCurrent(OnEvent, id);
}

int waitEventFlagBlocking(uint32_t id, uint32_t bits, uint32_t mode, uint32_t *result) {
    return waitEventFlag(id, bits, mode, result, false);
}

int pollEventFlag(uint32_t id, uint32_t bits, uint32_t mode, uint32_t *result) {
    return waitEventFlag(id, bits, mode, result, true);
}

int referEventFlagStatus(uint32_t id, EventInfo *info) {
    Critical critical;
    Event *event = eventOf(id);
    if (event == nullptr) {
        return kUnknownEvfid;
    }
    info->attr = event->attr;
    info->option = event->option;
    info->initial_bits = event->initial_bits;
    info->bits = event->bits;
    info->waiters = event->waiter_count;
    return kOk;
}

// --- thsemap (IOP-3g) ---------------------------------------------------------------

struct SemaParameters {
    uint32_t attr;
    uint32_t option;
    int32_t initial;
    int32_t max;
};

struct SemaInfo {
    uint32_t attr;
    uint32_t option;
    int32_t initial;
    int32_t max;
    int32_t current;
    uint32_t waiters;
};

int createSema(const SemaParameters *parameters) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    if ((parameters->attr & ~kSemaAttrMask) != 0) {
        return kIllegalAttr;
    }
    Critical critical;
    for (uint32_t k = 0; k < kSemaphores; k++) {
        Semaphore &sema = semaphores[k];
        if (sema.in_use) {
            continue;
        }
        sema.in_use = 1;
        sema.generation++;
        sema.attr = parameters->attr;
        sema.option = parameters->option;
        sema.initial = parameters->initial;
        sema.max = parameters->max;
        sema.count = parameters->initial;
        sema.waiters = {kNone, kNone};
        sema.waiter_count = 0;
        return static_cast<int>(makeId(k, sema.generation));
    }
    return kNoMemory;
}

int deleteSema(uint32_t id) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Semaphore *sema = semaphoreOf(id);
    if (sema == nullptr) {
        return kUnknownSemid;
    }
    while (sema->waiters.head != kNone) {
        wake(sema->waiters.head, kWaitDelete, true);
    }
    sema->in_use = 0;
    return kOk;
}

// IOP-3g: at the maximum the count stays, with no error.
int signalSema(uint32_t id, bool from_interrupt) {
    Critical critical;
    Semaphore *sema = semaphoreOf(id);
    if (sema == nullptr) {
        return kUnknownSemid;
    }
    if (sema->waiters.head != kNone) {
        wake(sema->waiters.head, kOk, from_interrupt);
        return kOk;
    }
    if (sema->count < sema->max) {
        sema->count++;
    }
    return kOk;
}

int signalSemaNormal(uint32_t id) {
    return signalSema(id, false);
}

int signalSemaInterrupt(uint32_t id) {
    return signalSema(id, true);
}

int waitSema(uint32_t id) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Semaphore *sema = semaphoreOf(id);
    if (sema == nullptr) {
        return kUnknownSemid;
    }
    if (sema->count > 0) {
        sema->count--;
        return kOk;
    }
    if (sema->attr & kSemaPriority) {
        enqueueByPriority(sema->waiters, current);
    } else {
        enqueue(sema->waiters, current);
    }
    sema->waiter_count++;
    return blockCurrent(OnSema, id);
}

int pollSema(uint32_t id) {
    Critical critical;
    Semaphore *sema = semaphoreOf(id);
    if (sema == nullptr) {
        return kUnknownSemid;
    }
    if (sema->count > 0) {
        sema->count--;
        return kOk;
    }
    return kSemaZero;
}

int referSemaStatus(uint32_t id, SemaInfo *info) {
    Critical critical;
    Semaphore *sema = semaphoreOf(id);
    if (sema == nullptr) {
        return kUnknownSemid;
    }
    info->attr = sema->attr;
    info->option = sema->option;
    info->initial = sema->initial;
    info->max = sema->max;
    info->current = sema->count;
    info->waiters = sema->waiter_count;
    return kOk;
}

// --- thmsgbx (IOP-3l) ---------------------------------------------------------------

struct MailboxParameters {
    uint32_t attr;
    uint32_t option;
};

struct MailboxInfo {
    uint32_t attr;
    uint32_t option;
    uint32_t waiters;
    uint32_t messages;
    Message *top;
};

int createMbx(const MailboxParameters *parameters) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    if ((parameters->attr & ~kMbxAttrMask) != 0) {
        return kIllegalAttr;
    }
    Critical critical;
    for (uint32_t k = 0; k < kMailboxes; k++) {
        Mailbox &mailbox = mailboxes[k];
        if (mailbox.in_use) {
            continue;
        }
        mailbox.in_use = 1;
        mailbox.generation++;
        mailbox.attr = parameters->attr;
        mailbox.option = parameters->option;
        mailbox.waiters = {kNone, kNone};
        mailbox.waiter_count = 0;
        mailbox.tail = nullptr;
        mailbox.message_count = 0;
        return static_cast<int>(makeId(k, mailbox.generation));
    }
    return kNoMemory;
}

// IOP-3l: whatever is queued is the senders' own memory, so a deleted box
// abandons it; only the waiting threads have to be answered.
int deleteMbx(uint32_t id) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Mailbox *mailbox = mailboxOf(id);
    if (mailbox == nullptr) {
        return kUnknownMbxid;
    }
    while (mailbox->waiters.head != kNone) {
        wake(mailbox->waiters.head, kWaitDelete, true);
    }
    mailbox->in_use = 0;
    return kOk;
}

// The queue is circular and `tail` names its last message, so the first is
// `tail->next` and an append is two stores (IOP-3l).
void queueMessage(Mailbox &mailbox, Message *message) {
    if (mailbox.tail == nullptr) {
        message->next = message;
        mailbox.tail = message;
        return;
    }
    if ((mailbox.attr & kMbxMessagePriority) == 0) {
        message->next = mailbox.tail->next;
        mailbox.tail->next = message;
        mailbox.tail = message;
        return;
    }
    // Ahead of the first message of strictly lower urgency -- a greater
    // byte -- so ties go behind. Walking from the tail keeps `previous->next`
    // as the candidate; the position is counted because a full lap comes
    // back to the tail, which is an append rather than an insertion at the
    // head, and the two differ only in whether the tail moves.
    Message *previous = mailbox.tail;
    uint32_t position = 0;
    while (position < mailbox.message_count
           && previous->next->priority <= message->priority) {
        previous = previous->next;
        position++;
    }
    message->next = previous->next;
    previous->next = message;
    if (position == mailbox.message_count) {
        mailbox.tail = message;
    }
}

[[nodiscard]] Message *dequeueMessage(Mailbox &mailbox) {
    Message *first = mailbox.tail->next;
    if (first == mailbox.tail) {
        mailbox.tail = nullptr;
    } else {
        mailbox.tail->next = first->next;
    }
    mailbox.message_count--;
    return first;
}

int sendMbx(uint32_t id, Message *message, bool from_interrupt) {
    Critical critical;
    Mailbox *mailbox = mailboxOf(id);
    if (mailbox == nullptr) {
        return kUnknownMbxid;
    }
    if (mailbox->waiters.head != kNone) {
        Thread &waiter = threads[mailbox->waiters.head];
        if (waiter.wait_out != nullptr) {
            *waiter.wait_out = reinterpret_cast<uintptr_t>(message);
        }
        wake(mailbox->waiters.head, kOk, from_interrupt);
        return kOk;
    }
    queueMessage(*mailbox, message);
    mailbox->message_count++;
    return kOk;
}

int sendMbxNormal(uint32_t id, Message *message) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    return sendMbx(id, message, false);
}

// IOP-3l: the `i` form's gate is the other way round -- it refuses a call
// from thread context, where its sibling refuses one from an interrupt.
int sendMbxInterrupt(uint32_t id, Message *message) {
    if (!_import_intrman_query_context()) {
        return kIllegalContext;
    }
    return sendMbx(id, message, true);
}

int receiveMbx(Message **received, uint32_t id) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    Critical critical;
    Mailbox *mailbox = mailboxOf(id);
    if (mailbox == nullptr) {
        return kUnknownMbxid;
    }
    if (mailbox->message_count > 0) {
        *received = dequeueMessage(*mailbox);
        return kOk;
    }
    threads[current].wait_out = reinterpret_cast<uint32_t *>(received);
    if (mailbox->attr & kMbxThreadPriority) {
        enqueueByPriority(mailbox->waiters, current);
    } else {
        enqueue(mailbox->waiters, current);
    }
    mailbox->waiter_count++;
    return blockCurrent(OnMbx, id);
}

int pollMbx(Message **received, uint32_t id) {
    Critical critical;
    Mailbox *mailbox = mailboxOf(id);
    if (mailbox == nullptr) {
        return kUnknownMbxid;
    }
    if (mailbox->message_count == 0) {
        return kMbxNoMsg;
    }
    *received = dequeueMessage(*mailbox);
    return kOk;
}

int referMbxStatus(uint32_t id, MailboxInfo *info) {
    Critical critical;
    Mailbox *mailbox = mailboxOf(id);
    if (mailbox == nullptr) {
        return kUnknownMbxid;
    }
    info->attr = mailbox->attr;
    info->option = mailbox->option;
    info->waiters = mailbox->waiter_count;
    info->messages = mailbox->message_count;
    // The reference reads this through the tail unguarded and faults on an
    // empty box; answering null is the same information without the fault.
    info->top = mailbox->tail != nullptr ? mailbox->tail->next : nullptr;
    return kOk;
}

void *getThreadmanData() {
    return nullptr;
}

// --- the tables (IOP-3a) ----------------------------------------------------------

PS2_EXPORT_TABLE ExportTable<42> thbase_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'t', 'h', 'b', 'a', 's', 'e', 0, 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(getThreadmanData),         // 3
        slot(createThread),             // 4
        slot(deleteThread),             // 5
        slot(startThread),              // 6
        slot(startThreadArgs),          // 7
        slot(exitThread),               // 8
        slot(stubError),                // 9  ExitDeleteThread: the reference's stub
        slot(terminateThreadNormal),    // 10
        slot(terminateThreadInterrupt), // 11
        slot(stubError),                // 12 DisableDispatchThread
        slot(stubError),                // 13 EnableDispatchThread
        slot(changeThreadPriorityNormal),      // 14
        slot(changeThreadPriorityInterrupt),   // 15
        slot(rotateThreadReadyQueueNormal),    // 16
        slot(rotateThreadReadyQueueInterrupt), // 17
        slot(releaseWaitThreadNormal),  // 18
        slot(releaseWaitThreadInterrupt),      // 19
        slot(getThreadId),              // 20
        slot(checkThreadStack),         // 21
        slot(referThreadStatus),        // 22
        slot(referThreadStatus),        // 23 iReferThreadStatus
        slot(sleepThread),              // 24
        slot(wakeupThreadNormal),       // 25
        slot(wakeupThreadInterrupt),    // 26
        slot(cancelWakeupThread),       // 27
        slot(cancelWakeupThread),       // 28 iCancelWakeupThread
        slot(stubError),                // 29 SuspendThread
        slot(stubError),                // 30 iSuspendThread
        slot(stubError),                // 31 ResumeThread
        slot(stubError),                // 32 iResumeThread
        slot(delayThread),              // 33 (IOP-3j)
        slot(getSystemTime),            // 34
        slot(setAlarm),                 // 35 (IOP-3k)
        slot(setAlarmInterrupt),        // 36
        slot(cancelAlarm),              // 37
        slot(cancelAlarmInterrupt),     // 38
        slot(usecToSysClock),           // 39
        slot(sysClockToUsec),           // 40
        slot(getSystemStatusFlag),      // 41
        nullptr,
    },
};

PS2_EXPORT_TABLE ExportTable<15> thevent_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'t', 'h', 'e', 'v', 'e', 'n', 't', 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(createEventFlag),          // 4
        slot(deleteEventFlag),          // 5
        slot(setEventFlagNormal),       // 6
        slot(setEventFlagInterrupt),    // 7
        slot(clearEventFlag),           // 8
        slot(clearEventFlag),           // 9  iClearEventFlag
        slot(waitEventFlagBlocking),    // 10
        slot(pollEventFlag),            // 11
        slot(reservedHook),             // 12
        slot(referEventFlagStatus),     // 13
        slot(referEventFlagStatus),     // 14 iReferEventFlagStatus
        nullptr,
    },
};

PS2_EXPORT_TABLE ExportTable<13> thsemap_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'t', 'h', 's', 'e', 'm', 'a', 'p', 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(createSema),               // 4
        slot(deleteSema),               // 5
        slot(signalSemaNormal),         // 6
        slot(signalSemaInterrupt),      // 7
        slot(waitSema),                 // 8
        slot(pollSema),                 // 9
        slot(reservedHook),             // 10
        slot(referSemaStatus),          // 11
        slot(referSemaStatus),          // 12 iReferSemaStatus
        nullptr,
    },
};

PS2_EXPORT_TABLE ExportTable<13> thmsgbx_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'t', 'h', 'm', 's', 'g', 'b', 'x', 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(createMbx),                // 4
        slot(deleteMbx),                // 5
        slot(sendMbxNormal),            // 6
        slot(sendMbxInterrupt),         // 7
        slot(receiveMbx),               // 8
        slot(pollMbx),                  // 9
        slot(reservedHook),             // 10
        slot(referMbxStatus),           // 11
        slot(referMbxStatus),           // 12 iReferMbxStatus
        nullptr,
    },
};

// IRX-10a: four libraries, so residency requires every one to register --
// the same shape sysclib's `registerLibraries` uses for its two.
bool registerLibraries() {
    const int thbase_result = _import_loadcore_register(&thbase_exports);
    const int thevent_result = _import_loadcore_register(&thevent_exports);
    const int thsemap_result = _import_loadcore_register(&thsemap_exports);
    const int thmsgbx_result = _import_loadcore_register(&thmsgbx_exports);
    return thbase_result >= 0 && thevent_result >= 0 && thsemap_result >= 0 &&
           thmsgbx_result >= 0;
}

}  // namespace

PS2_IMPORTS_BEGIN("sysmem\0\0", 0x0101)
PS2_IMPORT(_import_sysmem_allocate, 4)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_register, 4)
PS2_IMPORT(_import_intrman_enable, 6)
PS2_IMPORT(_import_intrman_cpu_enable, 9)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORT(_import_intrman_query_context, 23)
PS2_IMPORT(_import_intrman_set_new_ctx, 28)
PS2_IMPORT(_import_intrman_set_should_preempt, 30)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("timrman\0", 0x0101)
PS2_IMPORT(_import_timrman_alloc, 4)
PS2_IMPORT(_import_timrman_set_mode, 7)
PS2_IMPORT(_import_timrman_get_status, 8)
PS2_IMPORT(_import_timrman_set_counter, 9)
PS2_IMPORT(_import_timrman_get_counter, 10)
PS2_IMPORT(_import_timrman_set_compare, 11)
PS2_IMPORT(_import_timrman_intr_code, 16)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// IRX-12: the entry. IOP-3i: the idle thread and a record for the code that
// is running -- the boot, which goes on to the rest of the list on this
// thread -- then the hooks, and interrupts on as the very last thing.
int _module_start(int, char **) {
    if (!registerLibraries()) {
        return 1;
    }
    for (Queue &queue : ready) {
        queue = {kNone, kNone};
    }
    for (uint32_t &word : ready_bits) {
        word = 0;
    }

    Thread &idle = threads[0];
    idle.in_use = 1;
    idle.generation = 1;
    idle.attr = 0x02000000;                     // TH_C [header]
    idle.entry = idleLoop;
    idle.stack = reinterpret_cast<uintptr_t>(idle_stack);
    idle.stack_size = sizeof(idle_stack);
    idle.gp = currentGp();
    idle.initial_priority = kIdlePriority;
    idle.current_priority = kIdlePriority;
    primeFrame(idle, 0);
    makeReady(0);

    Thread &boot = threads[1];
    boot.in_use = 1;
    boot.generation = 1;
    boot.attr = 0x02000000;
    boot.entry = nullptr;
    uint32_t sp;
    asm volatile("move %0, $sp" : "=r"(sp));
    boot.stack = sp & ~uint32_t{0xFFF};
    boot.stack_size = 0x1000;
    boot.gp = currentGp();
    boot.initial_priority = kBootInitialPriority;
    boot.current_priority = kBootPriority;
    boot.state = Run;
    boot.frame = nullptr;
    current = 1;
    pending = 1;

    _import_intrman_set_new_ctx(newContext);
    _import_intrman_set_should_preempt(shouldPreempt);
    startClock();
    _import_intrman_cpu_enable();
    return 0;                                   // resident
}

}  // extern "C"
