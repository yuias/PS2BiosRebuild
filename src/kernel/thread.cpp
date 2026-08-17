// Threads, semaphores and the context switch: spec/05 SYS-8, SYS-9, SYS-10.
//
// docs/spec/05-ee-syscall-abi.md, from docs/analysis/30, 32 and 33. The
// records are the reference's in shape and count -- 256 threads, 256
// semaphores, a ready queue per priority and one more at 128 for the thread
// the kernel boots on -- because a program can see all of that through the
// status slots. What is ours is the switch (spec/04 EE-7g): the dispatcher in
// syscall.S saves every register into one fixed block and restores from it
// before `eret`, so a handler that reschedules copies that block into the
// outgoing thread's frame, copies the incoming thread's frame back, points EPC
// at where it resumes, and returns what its saved $v0 says. The frame is a
// push onto the thread's own stack -- 0x280 bytes below its saved $sp, which
// is why slot 0x3C primes one at `top - 0x2A0` for a stack whose pointer
// starts at `top - 0x20`.

#include <stdint.h>

namespace {

constexpr uint32_t kThreads = 256;
constexpr uint32_t kSemaphores = 256;
constexpr uint32_t kPriorities = 129;           // 0..127 settable, 128 the boot thread's
constexpr uint32_t kBootPriority = 128;
constexpr uint16_t kNone = 0xFFFF;

// EE-7e: sixteen bytes a register, thirty-two registers, and the frame is
// 0x2A0 in all; the dispatcher's block is the register part of it.
constexpr uint32_t kFrameBytes = 0x2A0;
constexpr uint32_t kFramePush = 0x280;          // frame = saved $sp - this
constexpr uint32_t kBlockWords = 32 * 4;
constexpr uint32_t kSlotV0 = 2 * 16;
constexpr uint32_t kSlotA0 = 4 * 16;
constexpr uint32_t kSlotGp = 28 * 16;
constexpr uint32_t kSlotSp = 29 * 16;
constexpr uint32_t kSlotFp = 30 * 16;
constexpr uint32_t kSlotRa = 31 * 16;
// SYS-8b: the block a program hands to 0x3C comes back as argc, sixteen argv
// words, then the strings.
constexpr uint32_t kArgvSlots = 16;
constexpr uint32_t kStringsAt = 4 + kArgvSlots * 4;

// SYS-10a: the states, as a program sees them through 0x30.
enum State : uint32_t {
    Free = 0,
    Run = 1,
    Ready = 2,
    Wait = 4,
    Suspend = 8,
    WaitSuspend = 0xC,
    Dormant = 0x10,
};

// SYS-10g/SYS-9d: what a waiting thread waits on.
enum WaitType : uint32_t { NotWaiting = 0, Sleeping = 1, OnSemaphore = 2 };

struct ThreadRecord {
    uint32_t state;
    uint32_t resume_pc;
    uint32_t context;       // its frame: where its registers are while it is out
    uint32_t gp;
    int16_t initial_priority;
    int16_t current_priority;
    uint32_t wait_type;
    uint32_t wait_id;
    uint32_t wakeup_count;
    uint32_t attr;
    uint32_t option;
    uint32_t entry;
    uint32_t argc;
    const char *args;       // the strings, packed, NUL after each
    uint32_t stack;         // base
    uint32_t stack_size;
    uint32_t root;
    uint32_t heap_end;
    uint32_t start_arg;     // what 0x22 was given
    uint16_t next;          // the queue it is in: ready, a wait list, or free
    uint16_t prev;
};

struct Semaphore {
    int32_t count;          // -1 while free (SYS-9a)
    int32_t max_count;
    uint32_t attr;
    uint32_t option;
    uint32_t wait_count;
    uint16_t wait_head;
    uint16_t wait_tail;
    uint16_t next_free;
};

struct Queue {
    uint16_t head;
    uint16_t tail;
};

ThreadRecord thread_table[kThreads];
Semaphore semaphore_table[kSemaphores];
Queue ready_queue[kPriorities];
uint32_t current_thread = 0;
uint32_t lowest_ready = kPriorities;             // where the pick starts scanning
uint16_t free_thread = kNone;                    // free lists, LIFO
uint16_t free_semaphore = kNone;
bool tables_ready = false;
// SYS-12c: set when a thread better than the running one is made ready --
// from an interrupt handler's direct-form call, typically -- and read by the
// interrupt exit, which switches if it finds it set.
bool reschedule_requested = false;

[[nodiscard]] ThreadRecord &current() {
    return thread_table[current_thread];
}

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

// The tables come up on first use rather than in a static initialiser the
// image does not run: every thread but the boot thread free, every semaphore
// free, the boot thread running at priority 128 -- SYS-10b -- and reported
// ready, which is what the reference reports for it (SYS-10j).
void prepareTables() {
    if (tables_ready) {
        return;
    }
    tables_ready = true;
    for (uint32_t k = 0; k < kPriorities; k++) {
        ready_queue[k] = {kNone, kNone};
    }
    for (uint32_t k = kThreads - 1; k >= 1; k--) {
        thread_table[k].state = Free;
        thread_table[k].next = free_thread;
        free_thread = static_cast<uint16_t>(k);
    }
    thread_table[0].state = Ready;
    thread_table[0].initial_priority = kBootPriority;
    thread_table[0].current_priority = kBootPriority;
    for (uint32_t k = kSemaphores; k-- > 0;) {
        semaphore_table[k].count = -1;
        semaphore_table[k].next_free = free_semaphore;
        free_semaphore = static_cast<uint16_t>(k);
    }
}

// --- queues -----------------------------------------------------------------

void enqueue(Queue &queue, uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    thread.next = kNone;
    thread.prev = queue.tail;
    if (queue.tail == kNone) {
        queue.head = id;
    } else {
        thread_table[queue.tail].next = id;
    }
    queue.tail = id;
}

void unlink(Queue &queue, uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    if (thread.prev == kNone) {
        queue.head = thread.next;
    } else {
        thread_table[thread.prev].next = thread.next;
    }
    if (thread.next == kNone) {
        queue.tail = thread.prev;
    } else {
        thread_table[thread.next].prev = thread.prev;
    }
    thread.next = kNone;
    thread.prev = kNone;
}

[[nodiscard]] uint16_t popFront(Queue &queue) {
    const uint16_t id = queue.head;
    if (id != kNone) {
        unlink(queue, id);
    }
    return id;
}

// SYS-10b: a thread made ready goes to the tail of its priority's queue.
void makeReady(uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    thread.state = Ready;
    const uint32_t priority = static_cast<uint16_t>(thread.current_priority);
    enqueue(ready_queue[priority], id);
    if (priority < lowest_ready) {
        lowest_ready = priority;
    }
    if (static_cast<int32_t>(priority) < current().current_priority) {
        reschedule_requested = true;
    }
}

void takeOffReadyQueue(uint16_t id) {
    unlink(ready_queue[static_cast<uint16_t>(thread_table[id].current_priority)], id);
}

// A thread waiting on a semaphore is on that semaphore's list; taking it off
// keeps the count the reference keeps (SYS-9f reports it).
void leaveWaitList(uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    if (thread.wait_type == OnSemaphore && thread.wait_id < kSemaphores) {
        Semaphore &sema = semaphore_table[thread.wait_id];
        Queue list{sema.wait_head, sema.wait_tail};
        unlink(list, id);
        sema.wait_head = list.head;
        sema.wait_tail = list.tail;
        if (sema.wait_count > 0) {
            sema.wait_count--;
        }
    }
    thread.wait_type = NotWaiting;
}

// --- the switch (EE-7g, SYS-10c) ---------------------------------------------

// syscall.S: the block the dispatcher saved the caller into and restores from.
extern "C" uint32_t _syscall_context[kBlockWords];
extern "C" void print(const char *text) asm("_print");

[[nodiscard]] uint32_t readEpc() {
    uint32_t value;
    asm volatile("mfc0 %0, $14" : "=r"(value));
    return value;
}

// `sync.p` is a `.word` because LLVM has no R5900 target to assemble it with.
void setEpc(uint32_t address) {
    asm volatile("mtc0 %0, $14\n\t.word 0x0000040f" ::"r"(address));
}

void copyWords(uint32_t *to, const uint32_t *from, uint32_t count) {
    for (uint32_t k = 0; k < count; k++) {
        to[k] = from[k];
    }
}

// Park the caller: its registers -- from `block`, the dispatcher's or an
// interrupt frame -- into a frame pushed on its own stack, and its resume
// address: from a syscall the EPC the dispatcher advanced (EE-7c), from an
// interrupt the interrupted instruction.
void parkCurrent(const uint32_t *block, uint32_t resume_pc) {
    ThreadRecord &thread = current();
    thread.resume_pc = resume_pc;
    const uint32_t frame = block[kSlotSp / 4] - kFramePush;
    copyWords(reinterpret_cast<uint32_t *>(frame), block, kBlockWords);
    thread.context = frame;
}

void saveCaller() {
    parkCurrent(_syscall_context, readEpc());
}

// SYS-10b: the head of the lowest-numbered non-empty queue. Nothing ready is
// a stop the reference makes with a message too.
[[nodiscard]] uint16_t pickNext() {
    for (uint32_t priority = lowest_ready; priority < kPriorities; priority++) {
        if (ready_queue[priority].head != kNone) {
            lowest_ready = priority;
            return popFront(ready_queue[priority]);
        }
    }
    print("# no thread is ready to run: the kernel stops here.\n");
    for (;;) {
    }
}

// Bring the picked thread in: its frame becomes the dispatcher's block, EPC
// its resume address, and the value the dispatcher will put in $v0 is what
// its frame says -- so this is what a rescheduling handler returns.
[[nodiscard]] uint32_t resume(uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    current_thread = id;
    thread.state = Run;
    const auto *frame = reinterpret_cast<const uint32_t *>(thread.context);
    copyWords(_syscall_context, frame, kBlockWords);
    setEpc(thread.resume_pc);
    return frame[kSlotV0 / 4];
}

// What a parked thread's syscall answers when it is resumed: its frame's $v0.
void setResumeValue(uint16_t id, uint32_t value) {
    reinterpret_cast<uint32_t *>(thread_table[id].context)[kSlotV0 / 4] = value;
}

// The caller yields with `answer` as its own result: it goes back to the tail
// of its ready queue and the pick runs, which reselects it when it is still
// the best (SYS-10c).
[[nodiscard]] uint32_t yield(uint32_t answer) {
    saveCaller();
    setResumeValue(static_cast<uint16_t>(current_thread), answer);
    makeReady(static_cast<uint16_t>(current_thread));
    return resume(pickNext());
}

// The caller blocks with the given wait type; `answer` is what its call
// returns if it is woken normally -- a forced release overwrites it with -1
// (SYS-10c) -- and the pick runs.
[[nodiscard]] uint32_t block(WaitType type, uint32_t wait_id, uint32_t answer) {
    saveCaller();
    ThreadRecord &thread = current();
    setResumeValue(static_cast<uint16_t>(current_thread), answer);
    thread.state = Wait;
    thread.wait_type = type;
    thread.wait_id = wait_id;
    return resume(pickNext());
}

// SYS-10i: where a thread function that returns lands (syscall.S).
extern "C" void threadRoot() asm("_thread_root");

// SYS-10d/SYS-10e: a fresh frame for a thread about to be started -- or
// started again after exiting -- primed the way 0x3C primes the main one.
void primeFrame(ThreadRecord &thread) {
    const uint32_t top = thread.stack + thread.stack_size;
    const uint32_t frame = top - kFrameBytes;
    for (uint32_t k = 0; k < kBlockWords; k++) {
        reinterpret_cast<uint32_t *>(frame)[k] = 0;
    }
    writeWord(frame + kSlotGp, thread.gp);
    writeWord(frame + kSlotSp, top - 0x20);
    writeWord(frame + kSlotFp, top - 0x20);
    writeWord(frame + kSlotRa, thread.root);
    thread.context = frame;
}

// SYS-10e: back to dormant, startable again.
void resetToDormant(ThreadRecord &thread) {
    thread.state = Dormant;
    thread.resume_pc = thread.entry;
    thread.current_priority = thread.initial_priority;
    thread.wait_type = NotWaiting;
    thread.wakeup_count = 0;
    thread.root = reinterpret_cast<uintptr_t>(threadRoot);
    primeFrame(thread);
}

void freeThread(uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    thread.state = Free;
    thread.next = free_thread;
    free_thread = id;
}

// Take a thread out of whatever it is in, whatever its state (SYS-10e).
void detach(uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    if (thread.state == Ready) {
        takeOffReadyQueue(id);
    } else if (thread.state == Wait || thread.state == WaitSuspend) {
        leaveWaitList(id);
    }
}

[[nodiscard]] bool validOther(uint32_t id) {
    return id >= 1 && id < kThreads && id != current_thread;
}

}  // namespace

extern "C" {

// SYS-12c, for interrupt.cpp: the entry clears the request before handlers
// run; the exit asks. `frame` is the interrupt frame -- the block's shape,
// EPC in slot 0 -- and if a switch is due it is rewritten with the picked
// thread's registers and resume address, which the exit then restores.
void clearRescheduleRequest() {
    reschedule_requested = false;
}

bool interruptReschedule(uint32_t *frame) {
    if (!reschedule_requested || !tables_ready) {
        return false;
    }
    reschedule_requested = false;
    parkCurrent(frame, frame[0]);
    makeReady(static_cast<uint16_t>(current_thread));
    const uint16_t next = pickNext();
    ThreadRecord &thread = thread_table[next];
    current_thread = next;
    thread.state = Run;
    copyWords(frame, reinterpret_cast<const uint32_t *>(thread.context), kBlockWords);
    frame[0] = thread.resume_pc;
    return true;
}

// EE-2b / EE-4a: RDRAM's return, kept by the kernel entry (entry.cpp).
uint32_t memory_size;

// The launcher's half of SYS-8d: record what 0x3C will hand over.
void setProgramArguments(uint32_t argc, const char *packed) {
    prepareTables();
    ThreadRecord &thread = current();
    thread.argc = argc;
    thread.args = packed;
}

// --- SYS-8: the main thread's setup -------------------------------------------

// Slot 0x3C, SYS-8a and SYS-8b. `root` arrives in $t0, which syscall.S puts
// where this function's ABI expects a fifth argument.
uint32_t sysSetupThread(uint32_t gp, uint32_t stack, uint32_t stack_size,
                        uint32_t *args, uint32_t root) asm("_sys_setup_thread");
uint32_t sysSetupThread(uint32_t gp, uint32_t stack, uint32_t stack_size,
                        uint32_t *args, uint32_t root) {
    prepareTables();
    // SYS-8a: -1 asks for the top of memory. The reference compares all
    // sixty-four bits, so a zero-extended 0xFFFFFFFF wraps there; here it is
    // taken as -1 too (docs/implementation.md).
    if (stack == 0xFFFFFFFF) {
        stack = memory_size - 0x1000 - stack_size;
    }
    const uint32_t top = stack + stack_size;
    const uint32_t frame = top - kFrameBytes;
    writeWord(frame + kSlotGp, gp);
    writeWord(frame + kSlotSp, top - 0x20);
    writeWord(frame + kSlotFp, top - 0x20);
    writeWord(frame + kSlotRa, root);

    ThreadRecord &thread = current();
    thread.context = frame;
    thread.gp = gp;
    thread.stack = stack;
    thread.stack_size = stack_size;
    thread.root = root;

    // SYS-8b: fill the caller's block from the list the launcher left.
    if (args != nullptr) {
        args[0] = thread.argc;
        auto *strings = reinterpret_cast<char *>(args) + kStringsAt;
        const char *from = thread.args;
        for (uint32_t k = 0; k < thread.argc && k < kArgvSlots; k++) {
            args[1 + k] = reinterpret_cast<uintptr_t>(strings);
            if (from != nullptr) {
                do {
                    *strings++ = *from;
                } while (*from++ != '\0');
            } else {
                *strings++ = '\0';
            }
        }
    }
    thread.args = reinterpret_cast<const char *>(args);
    return frame;
}

// Slot 0x3D, SYS-8c.
uint32_t sysSetupHeap(uint32_t start, int32_t size) asm("_sys_setup_heap");
uint32_t sysSetupHeap(uint32_t start, int32_t size) {
    prepareTables();
    ThreadRecord &thread = current();
    thread.heap_end = size < 0 ? thread.stack
                               : start + static_cast<uint32_t>(size);
    return thread.heap_end;
}

// Slot 0x3E, SYS-8c.
uint32_t sysEndOfHeap() asm("_sys_end_of_heap");
uint32_t sysEndOfHeap() {
    prepareTables();
    return current().heap_end;
}

// --- SYS-10: threads -----------------------------------------------------------

// Slot 0x20, SYS-10d: create(block) -> id | -1. The block is the SDK's
// ee_thread_t; only these fields are read.
int32_t sysCreateThread(const uint32_t *block) asm("_sys_create_thread");
int32_t sysCreateThread(const uint32_t *block) {
    prepareTables();
    if (free_thread == kNone) {
        return -1;
    }
    const uint16_t id = free_thread;
    ThreadRecord &thread = thread_table[id];
    free_thread = thread.next;
    thread.entry = block[1];
    thread.stack = block[2];
    thread.stack_size = block[3];
    thread.gp = block[4];
    thread.initial_priority = static_cast<int16_t>(block[5]);
    thread.current_priority = thread.initial_priority;
    thread.attr = block[7];
    thread.option = block[8];
    thread.wait_id = 0;
    thread.argc = 0;
    thread.args = nullptr;
    thread.start_arg = 0;
    thread.heap_end = 0;
    thread.next = kNone;
    thread.prev = kNone;
    resetToDormant(thread);
    return id;
}

// Slot 0x21, SYS-10e: delete(id) -> id | -1.
int32_t sysDeleteThread(uint32_t id) asm("_sys_delete_thread");
int32_t sysDeleteThread(uint32_t id) {
    prepareTables();
    if (!validOther(id) || thread_table[id].state != Dormant) {
        return -1;
    }
    freeThread(static_cast<uint16_t>(id));
    return static_cast<int32_t>(id);
}

// Slot 0x22, SYS-10d: start(id, arg) -> id | -1, and a switch.
int32_t sysStartThread(uint32_t id, uint32_t arg) asm("_sys_start_thread");
int32_t sysStartThread(uint32_t id, uint32_t arg) {
    prepareTables();
    if (!validOther(id) || thread_table[id].state != Dormant) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    thread.start_arg = arg;
    writeWord(thread.context + kSlotA0, arg);
    thread.resume_pc = thread.entry;
    makeReady(static_cast<uint16_t>(id));
    return static_cast<int32_t>(yield(id));
}

// Slot 0x23, SYS-10e: exit(). Never returns to its caller.
uint32_t sysExitThread() asm("_sys_exit_thread");
uint32_t sysExitThread() {
    prepareTables();
    ThreadRecord &thread = current();
    resetToDormant(thread);
    return resume(pickNext());
}

// Slot 0x24, SYS-10e: exit and delete. Never returns to its caller.
uint32_t sysExitDeleteThread() asm("_sys_exit_delete_thread");
uint32_t sysExitDeleteThread() {
    prepareTables();
    freeThread(static_cast<uint16_t>(current_thread));
    return resume(pickNext());
}

// Slots 0x25/0x26, SYS-10e: terminate(id) -> id | -1. 0x25 switches after.
int32_t terminateThread(uint32_t id) {
    prepareTables();
    if (id < 1 || id >= kThreads) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    if (thread.state == Free || thread.state == Dormant) {
        return -1;
    }
    detach(static_cast<uint16_t>(id));
    resetToDormant(thread);
    return static_cast<int32_t>(id);
}
int32_t sysTerminateThread(uint32_t id) asm("_sys_terminate_thread");
int32_t sysTerminateThread(uint32_t id) {
    const int32_t result = terminateThread(id);
    if (result < 0) {
        return result;
    }
    if (id == current_thread) {
        return static_cast<int32_t>(resume(pickNext()));
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysTerminateThreadDirect(uint32_t id) asm("_sys_terminate_thread_direct");
int32_t sysTerminateThreadDirect(uint32_t id) {
    return terminateThread(id);
}

// Slots 0x29/0x2A, SYS-10f: change priority(id, priority) -> previous | -1.
int32_t changeThreadPriority(uint32_t id, uint32_t priority) {
    prepareTables();
    if (id == 0) {
        id = current_thread;
    }
    if (id >= kThreads || priority >= 128) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    if (thread.state == Free || thread.state == Dormant) {
        return -1;
    }
    const int32_t previous = thread.current_priority;
    if (thread.state == Ready) {
        takeOffReadyQueue(static_cast<uint16_t>(id));
        thread.current_priority = static_cast<int16_t>(priority);
        makeReady(static_cast<uint16_t>(id));
    } else {
        thread.current_priority = static_cast<int16_t>(priority);
    }
    return previous;
}
int32_t sysChangeThreadPriority(uint32_t id, uint32_t priority)
    asm("_sys_change_thread_priority");
int32_t sysChangeThreadPriority(uint32_t id, uint32_t priority) {
    const int32_t result = changeThreadPriority(id, priority);
    if (result < 0) {
        return result;
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysChangeThreadPriorityDirect(uint32_t id, uint32_t priority)
    asm("_sys_change_thread_priority_direct");
int32_t sysChangeThreadPriorityDirect(uint32_t id, uint32_t priority) {
    return changeThreadPriority(id, priority);
}

// Slots 0x2B/0x2C, SYS-10f: rotate(priority) -> priority | -1.
int32_t rotateReadyQueue(uint32_t priority) {
    prepareTables();
    if (priority >= 128) {
        return -1;
    }
    Queue &queue = ready_queue[priority];
    const uint16_t head = popFront(queue);
    if (head != kNone) {
        enqueue(queue, head);
    }
    return static_cast<int32_t>(priority);
}
int32_t sysRotateThreadReadyQueue(uint32_t priority)
    asm("_sys_rotate_thread_ready_queue");
int32_t sysRotateThreadReadyQueue(uint32_t priority) {
    const int32_t result = rotateReadyQueue(priority);
    if (result < 0) {
        return result;
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysRotateThreadReadyQueueDirect(uint32_t priority)
    asm("_sys_rotate_thread_ready_queue_direct");
int32_t sysRotateThreadReadyQueueDirect(uint32_t priority) {
    return rotateReadyQueue(priority);
}

// Slots 0x2D/0x2E, SYS-10h: release(id) -> id | -1. The released thread's
// blocking call answers -1 (SYS-10c).
int32_t releaseWaitThread(uint32_t id) {
    prepareTables();
    if (id < 1 || id >= kThreads) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    if (thread.state == Free) {
        return -1;
    }
    if (thread.state == Wait) {
        leaveWaitList(static_cast<uint16_t>(id));
        setResumeValue(static_cast<uint16_t>(id), static_cast<uint32_t>(-1));
        makeReady(static_cast<uint16_t>(id));
    } else if (thread.state == WaitSuspend) {
        leaveWaitList(static_cast<uint16_t>(id));
        setResumeValue(static_cast<uint16_t>(id), static_cast<uint32_t>(-1));
        thread.state = Suspend;
    }
    return static_cast<int32_t>(id);
}
int32_t sysReleaseWaitThread(uint32_t id) asm("_sys_release_wait_thread");
int32_t sysReleaseWaitThread(uint32_t id) {
    const int32_t result = releaseWaitThread(id);
    if (result < 0) {
        return result;
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysReleaseWaitThreadDirect(uint32_t id) asm("_sys_release_wait_thread_direct");
int32_t sysReleaseWaitThreadDirect(uint32_t id) {
    return releaseWaitThread(id);
}

// Slot 0x2F, SYS-10a.
uint32_t sysGetThreadId() asm("_sys_get_thread_id");
uint32_t sysGetThreadId() {
    prepareTables();
    return current_thread;
}

// Slots 0x30/0x31, SYS-10j: status(id, out) -> state | -1. The block is the
// SDK's twelve-word ee_thread_status_t.
int32_t sysReferThreadStatus(uint32_t id, uint32_t *out) asm("_sys_refer_thread_status");
int32_t sysReferThreadStatus(uint32_t id, uint32_t *out) {
    prepareTables();
    if (id == 0) {
        id = current_thread;
    }
    if (id >= kThreads) {
        return -1;
    }
    const ThreadRecord &thread = thread_table[id];
    if (out != nullptr) {
        out[0] = thread.state;
        out[1] = thread.entry;
        out[2] = thread.stack;
        out[3] = thread.stack_size;
        out[4] = thread.gp;
        out[5] = static_cast<uint32_t>(static_cast<int32_t>(thread.initial_priority));
        out[6] = static_cast<uint32_t>(static_cast<int32_t>(thread.current_priority));
        out[7] = thread.attr;
        out[8] = thread.option;
        out[9] = thread.wait_type;
        out[10] = thread.wait_id;
        out[11] = thread.wakeup_count;
    }
    return static_cast<int32_t>(thread.state);
}

// Slot 0x32, SYS-10g: sleep() -> id, now or when woken.
int32_t sysSleepThread() asm("_sys_sleep_thread");
int32_t sysSleepThread() {
    prepareTables();
    ThreadRecord &thread = current();
    if (thread.wakeup_count > 0) {
        thread.wakeup_count--;
        return static_cast<int32_t>(current_thread);
    }
    return static_cast<int32_t>(block(Sleeping, 0, current_thread));
}

// Slots 0x33/0x34, SYS-10g: wakeup(id) -> id | -1.
int32_t wakeupThread(uint32_t id) {
    prepareTables();
    if (id >= kThreads) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    switch (thread.state) {
    case Wait:
        if (thread.wait_type == Sleeping) {
            thread.wait_type = NotWaiting;
            makeReady(static_cast<uint16_t>(id));
        } else {
            thread.wakeup_count++;
        }
        return static_cast<int32_t>(id);
    case Ready:
    case Suspend:
        thread.wakeup_count++;
        return static_cast<int32_t>(id);
    case WaitSuspend:
        if (thread.wait_type == Sleeping) {
            thread.wait_type = NotWaiting;
            thread.state = Suspend;
        } else {
            thread.wakeup_count++;
        }
        return static_cast<int32_t>(id);
    default:
        return -1;
    }
}
int32_t sysWakeupThread(uint32_t id) asm("_sys_wakeup_thread");
int32_t sysWakeupThread(uint32_t id) {
    const int32_t result = wakeupThread(id);
    if (result < 0) {
        return result;
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysWakeupThreadDirect(uint32_t id) asm("_sys_wakeup_thread_direct");
int32_t sysWakeupThreadDirect(uint32_t id) {
    return wakeupThread(id);
}

// Slots 0x35/0x36, SYS-10g: cancel wakeups(id) -> previous count | -1.
int32_t sysCancelWakeupThread(uint32_t id) asm("_sys_cancel_wakeup_thread");
int32_t sysCancelWakeupThread(uint32_t id) {
    prepareTables();
    if (id >= kThreads) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    const int32_t previous = static_cast<int32_t>(thread.wakeup_count);
    thread.wakeup_count = 0;
    return previous;
}

// Slots 0x37/0x38, SYS-10h: suspend(id) -> id | -1.
int32_t sysSuspendThread(uint32_t id) asm("_sys_suspend_thread");
int32_t sysSuspendThread(uint32_t id) {
    prepareTables();
    if (id < 1 || id >= kThreads) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    switch (thread.state) {
    case Run:
        thread.state = Suspend;     // the reference has no path that switches here
        return static_cast<int32_t>(id);
    case Ready:
        takeOffReadyQueue(static_cast<uint16_t>(id));
        thread.state = Suspend;
        return static_cast<int32_t>(id);
    case Wait:
        thread.state = WaitSuspend;
        return static_cast<int32_t>(id);
    default:
        return -1;
    }
}

// Slots 0x39/0x3A, SYS-10h: resume(id) -> id | -1. 0x39 switches after.
int32_t resumeThread(uint32_t id) {
    prepareTables();
    if (!validOther(id)) {
        return -1;
    }
    ThreadRecord &thread = thread_table[id];
    if (thread.state == Suspend) {
        makeReady(static_cast<uint16_t>(id));
    } else if (thread.state == WaitSuspend) {
        thread.state = Wait;
    } else {
        return -1;
    }
    return static_cast<int32_t>(id);
}
int32_t sysResumeThread(uint32_t id) asm("_sys_resume_thread");
int32_t sysResumeThread(uint32_t id) {
    const int32_t result = resumeThread(id);
    if (result < 0) {
        return result;
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysResumeThreadDirect(uint32_t id) asm("_sys_resume_thread_direct");
int32_t sysResumeThreadDirect(uint32_t id) {
    return resumeThread(id);
}

// --- SYS-9: semaphores -----------------------------------------------------------

// Slot 0x40, SYS-9b: create(block) -> id | -1. The SDK's ee_sema_t; only
// these fields are read, and nothing is validated but the initial count.
int32_t sysCreateSema(const uint32_t *block) asm("_sys_create_sema");
int32_t sysCreateSema(const uint32_t *block) {
    prepareTables();
    if (free_semaphore == kNone) {
        return -1;
    }
    const int32_t init_count = static_cast<int32_t>(block[2]);
    if (init_count < 0) {
        return -1;
    }
    const uint16_t id = free_semaphore;
    Semaphore &sema = semaphore_table[id];
    free_semaphore = sema.next_free;
    sema.count = init_count;
    sema.max_count = static_cast<int32_t>(block[1]);
    sema.attr = block[4];
    sema.option = block[5];
    sema.wait_count = 0;
    sema.wait_head = kNone;
    sema.wait_tail = kNone;
    return id;
}

[[nodiscard]] Semaphore *allocated(uint32_t id) {
    prepareTables();
    if (id >= kSemaphores || semaphore_table[id].count < 0) {
        return nullptr;
    }
    return &semaphore_table[id];
}

// Hand a waiting thread back to running order: off the list, and ready
// unless it was suspended meanwhile.
void wakeWaiter(Semaphore &sema, uint16_t id) {
    ThreadRecord &thread = thread_table[id];
    Queue list{sema.wait_head, sema.wait_tail};
    unlink(list, id);
    sema.wait_head = list.head;
    sema.wait_tail = list.tail;
    if (sema.wait_count > 0) {
        sema.wait_count--;
    }
    thread.wait_type = NotWaiting;
    if (thread.state == Wait) {
        makeReady(id);
    } else if (thread.state == WaitSuspend) {
        thread.state = Suspend;
    }
}

// Slots 0x41/0x49, SYS-9g: delete(id) -> id | -1. Every waiter is released
// and answers -1 in its 0x44 (SYS-10c).
int32_t deleteSema(uint32_t id) {
    Semaphore *sema = allocated(id);
    if (sema == nullptr) {
        return -1;
    }
    while (sema->wait_head != kNone) {
        const uint16_t waiter = sema->wait_head;
        setResumeValue(waiter, static_cast<uint32_t>(-1));
        wakeWaiter(*sema, waiter);
    }
    sema->count = -1;
    sema->next_free = free_semaphore;
    free_semaphore = static_cast<uint16_t>(id);
    return static_cast<int32_t>(id);
}
int32_t sysDeleteSema(uint32_t id) asm("_sys_delete_sema");
int32_t sysDeleteSema(uint32_t id) {
    const int32_t result = deleteSema(id);
    if (result < 0) {
        return result;
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysDeleteSemaDirect(uint32_t id) asm("_sys_delete_sema_direct");
int32_t sysDeleteSemaDirect(uint32_t id) {
    return deleteSema(id);
}

// Slots 0x42/0x43, SYS-9c: signal(id) -> id | -1. No max-count check: the
// reference has none, and a full semaphore signalled again counts higher.
int32_t signalSema(uint32_t id) {
    Semaphore *sema = allocated(id);
    if (sema == nullptr) {
        return -1;
    }
    if (sema->wait_head == kNone) {
        sema->count++;
        return static_cast<int32_t>(id);
    }
    const uint16_t waiter = sema->wait_head;
    setResumeValue(waiter, id);          // its 0x44 answers the id
    wakeWaiter(*sema, waiter);
    return static_cast<int32_t>(id);
}
int32_t sysSignalSema(uint32_t id) asm("_sys_signal_sema");
int32_t sysSignalSema(uint32_t id) {
    const int32_t result = signalSema(id);
    if (result < 0) {
        return result;
    }
    return static_cast<int32_t>(yield(static_cast<uint32_t>(result)));
}
int32_t sysSignalSemaDirect(uint32_t id) asm("_sys_signal_sema_direct");
int32_t sysSignalSemaDirect(uint32_t id) {
    return signalSema(id);
}

// Slot 0x44, SYS-9d: wait(id) -> id | -1, or blocks until signalled.
int32_t sysWaitSema(uint32_t id) asm("_sys_wait_sema");
int32_t sysWaitSema(uint32_t id) {
    Semaphore *sema = allocated(id);
    if (sema == nullptr) {
        return -1;
    }
    if (sema->count > 0) {
        sema->count--;
        return static_cast<int32_t>(id);
    }
    Queue list{sema->wait_head, sema->wait_tail};
    enqueue(list, static_cast<uint16_t>(current_thread));
    sema->wait_head = list.head;
    sema->wait_tail = list.tail;
    sema->wait_count++;
    return static_cast<int32_t>(block(OnSemaphore, id, id));
}

// Slots 0x45/0x46, SYS-9e: poll(id) -> id | -1.
int32_t sysPollSema(uint32_t id) asm("_sys_poll_sema");
int32_t sysPollSema(uint32_t id) {
    Semaphore *sema = allocated(id);
    if (sema == nullptr || sema->count <= 0) {
        return -1;
    }
    sema->count--;
    return static_cast<int32_t>(id);
}

// Slots 0x47/0x48, SYS-9f: status(id, out) -> id | -1; out+0x08 is not written.
int32_t sysReferSemaStatus(uint32_t id, uint32_t *out) asm("_sys_refer_sema_status");
int32_t sysReferSemaStatus(uint32_t id, uint32_t *out) {
    Semaphore *sema = allocated(id);
    if (sema == nullptr) {
        return -1;
    }
    if (out == nullptr) {
        return static_cast<int32_t>(id);
    }
    out[0] = static_cast<uint32_t>(sema->count);
    out[1] = static_cast<uint32_t>(sema->max_count);
    out[3] = sema->wait_count;
    out[4] = sema->attr;
    out[5] = sema->option;
    return static_cast<int32_t>(id);
}

}  // extern "C"
