// os_thread.cpp — Dolphin OS threads, semaphores and thread queues on cooperative fibers
// (xbox/docs/05-SYSTEM.md section 3).
//
// The game runs its 18 task slots as OSThreads that the main thread resumes one after the other each
// frame (scheduler.cpp): a task created at priority 15 preempts the main thread (16) as soon as it is
// resumed or woken, and gives control back by resuming its parent and sleeping on its queue
// (TaskSleep). Nothing in the game relies on real parallelism or time slicing, so a faithful model of
// the OS scheduler on one kernel thread is enough: every call that changes a thread's state ends with
// reschedule(), which switches to the highest-priority runnable thread (lowest number; FIFO among
// equals, the running thread keeps the CPU on a tie), exactly the Dolphin rule.
//
// State lives inside the game's OSThread (its 0x2C8-byte OSContext area is unused by the game), so no
// side table is needed. The stack is the one the game passes (top of stack, grows down).
//
// Callbacks that ran in interrupt context on the GameCube (DVD, ARQ, AX) are delivered by the port from
// safe points on the main fiber, so OSDisableInterrupts/OSRestoreInterrupts stay trivial.
#include "re4port/re4_xbox.h"
#include <string.h>

extern "C" {

typedef int s32;
typedef unsigned int u32;

struct OSThread;
struct OSThreadQueue { OSThread* head; OSThread* tail; };
struct OSThreadLink { OSThread* next; OSThread* prev; };

enum { ST_READY = 1, ST_RUNNING = 2, ST_WAITING = 4, ST_MORIBUND = 8 };

// Port view of the game's OSThread (layout must match include/dolphin/os/OSThread.h for the fields
// past the context, because the game's headers size the struct; the context area holds the fiber).
struct FiberCtx {
    void* sp;                      // saved stack pointer (the switch pushes ebp/ebx/esi/edi first)
    void* (*func)(void*);
    void* arg;
    u32 magic;
};

struct OSThread {
    union {
        FiberCtx fiber;
        unsigned char context[0x2C8];
    };
    unsigned short state;           // 0x2C8
    unsigned short attr;            // 0x2CA
    s32 suspend;                    // 0x2CC
    s32 priority;                   // 0x2D0
    s32 base;                       // 0x2D4
    void* val;                      // 0x2D8
    OSThreadQueue* queue;           // 0x2DC  queue the thread waits on
    OSThreadLink link;              // 0x2E0  link inside that queue
    OSThreadQueue queueJoin;        // 0x2E8
    void* mutex;                    // 0x2F0
    void* queueMutex[2];            // 0x2F4
    OSThreadLink linkActive;        // 0x2FC
    unsigned char* stackBase;       // 0x304  top of stack
    u32* stackEnd;                  // 0x308  lowest word (0xDEADBABE guard)
    s32 error;                      // 0x30C
    void* specific[2];              // 0x310
};
static_assert(sizeof(OSThread) == 0x318, "OSThread layout must match the SDK");

struct OSSemaphore { s32 count; OSThreadQueue queue; };

#define FIBER_MAGIC 0x52453446u   // 'RE4F'
enum { MAX_THREADS = 64 };

static OSThread s_mainThread;
static OSThread* s_current;
static OSThread* s_threads[MAX_THREADS];   // every created thread, for the scheduler scan
static int s_nthreads;

// ---- context switch ---------------------------------------------------------------------------
// re4_fiber_switch(&from->sp, to->sp): push the callee-saved registers, save esp, load the new esp,
// pop and return into the other fiber. cdecl; the same code works for i386-win32 (nxdk) and i386 ELF.
__attribute__((naked, noinline)) static void re4_fiber_switch(void** save_sp, void* new_sp)
{
    __asm__ volatile(
        "movl 4(%esp), %eax\n\t"     // save_sp
        "movl 8(%esp), %edx\n\t"     // new_sp
        "pushl %ebp\n\t"
        "pushl %ebx\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "movl %esp, (%eax)\n\t"
        "movl %edx, %esp\n\t"
        "popl %edi\n\t"
        "popl %esi\n\t"
        "popl %ebx\n\t"
        "popl %ebp\n\t"
        "ret\n\t");
}

void OSExitThread(void* val);

static void fiber_entry(void)
{
    OSThread* t = s_current;
    void* r = t->fiber.func(t->fiber.arg);
    OSExitThread(r);
    re4_fatal(__FILE__, __LINE__, "exited thread resumed");
}

// ---- queues -------------------------------------------------------------------------------------
static void q_push_tail(OSThreadQueue* q, OSThread* t)
{
    t->link.next = 0;
    t->link.prev = q->tail;
    if (q->tail) q->tail->link.next = t; else q->head = t;
    q->tail = t;
    t->queue = q;
}

static void q_remove(OSThreadQueue* q, OSThread* t)
{
    if (t->link.prev) t->link.prev->link.next = t->link.next; else q->head = t->link.next;
    if (t->link.next) t->link.next->link.prev = t->link.prev; else q->tail = t->link.prev;
    t->link.next = t->link.prev = 0;
    t->queue = 0;
}

// ---- scheduler ------------------------------------------------------------------------------------
static int runnable(OSThread* t) { return t->state == ST_READY || t->state == ST_RUNNING ? t->suspend <= 0 : 0; }

static OSThread* pick_next(void)
{
    OSThread* best = runnable(s_current) ? s_current : 0;
    for (int i = 0; i < s_nthreads; i++) {
        OSThread* t = s_threads[i];
        if (t == s_current || !runnable(t)) continue;
        if (!best || t->priority < best->priority) best = t;   // strict: equals keep the CPU / FIFO by creation
    }
    return best;
}

static void reschedule(void)
{
    OSThread* next = pick_next();
    if (!next) {
        re4_fatal(__FILE__, __LINE__, "OS scheduler: no runnable thread (deadlock)");
        return;
    }
    if (next == s_current) {
        s_current->state = ST_RUNNING;
        return;
    }
    OSThread* prev = s_current;
    if (prev->state == ST_RUNNING) prev->state = ST_READY;
    next->state = ST_RUNNING;
    s_current = next;
    re4_fiber_switch(&prev->fiber.sp, next->fiber.sp);
    // back on `prev`
}

static void ensure_init(void)
{
    if (s_current) return;
    memset(&s_mainThread, 0, sizeof s_mainThread);
    s_mainThread.fiber.magic = FIBER_MAGIC;
    s_mainThread.state = ST_RUNNING;
    s_mainThread.priority = s_mainThread.base = 16;   // OSInit's default thread priority
    s_current = &s_mainThread;
    s_threads[s_nthreads++] = &s_mainThread;
}

static void forget(OSThread* t)
{
    for (int i = 0; i < s_nthreads; i++) {
        if (s_threads[i] == t) {
            s_threads[i] = s_threads[--s_nthreads];
            return;
        }
    }
}

// ---- API ------------------------------------------------------------------------------------------
void OSInitThreadQueue(OSThreadQueue* q) { q->head = q->tail = 0; }

OSThread* OSGetCurrentThread(void) { ensure_init(); return s_current; }

int OSCreateThread(OSThread* t, void* (*func)(void*), void* param, void* stack, u32 stackSize, s32 prio, unsigned short attr)
{
    ensure_init();
    if (prio < 0 || prio > 31) return 0;
    if (s_nthreads >= MAX_THREADS) {
        re4_log("OSCreateThread: too many threads\n");
        return 0;
    }
    forget(t);   // a slot's OSThread is reused after the previous thread exited
    memset(t, 0, sizeof *t);
    t->fiber.magic = FIBER_MAGIC;
    t->fiber.func = func;
    t->fiber.arg = param;
    t->state = ST_READY;
    t->attr = attr;
    t->suspend = 1;                    // created suspended; OSResumeThread starts it
    t->priority = t->base = prio;
    t->stackBase = (unsigned char*) stack;
    t->stackEnd = (u32*) ((unsigned char*) stack - stackSize);
    *t->stackEnd = 0xDEADBABE;         // the game's StackOverflowCheck reads this guard
    // initial frame popped by re4_fiber_switch: edi esi ebx ebp, then the return address
    u32* sp = (u32*) (((unsigned) stack - 16) & ~15u);
    *--sp = 0;                          // fake return address of fiber_entry
    *--sp = (u32) (void*) fiber_entry;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    t->fiber.sp = sp;
    s_threads[s_nthreads++] = t;
    return 1;
}

s32 OSResumeThread(OSThread* t)
{
    ensure_init();
    s32 prev = t->suspend--;
    if (t->suspend <= 0) {
        t->suspend = 0;
        reschedule();
    }
    return prev;
}

s32 OSSuspendThread(OSThread* t)
{
    ensure_init();
    s32 prev = t->suspend++;
    if (prev == 0) reschedule();
    return prev;
}

void OSSleepThread(OSThreadQueue* q)
{
    ensure_init();
    OSThread* t = s_current;
    t->state = ST_WAITING;
    q_push_tail(q, t);
    reschedule();
}

void OSWakeupThread(OSThreadQueue* q)
{
    ensure_init();
    while (q->head) {
        OSThread* t = q->head;
        q_remove(q, t);
        t->state = ST_READY;
    }
    reschedule();
}

void OSExitThread(void* val)
{
    ensure_init();
    OSThread* t = s_current;
    t->val = val;
    t->state = ST_MORIBUND;
    forget(t);
    OSThread* next = pick_next();
    if (!next) re4_fatal(__FILE__, __LINE__, "OSExitThread: no thread left to run");
    next->state = ST_RUNNING;
    s_current = next;
    static void* dead_sp;
    re4_fiber_switch(&dead_sp, next->fiber.sp);
}

void OSCancelThread(OSThread* t)
{
    ensure_init();
    if (t == s_current) {
        OSExitThread(0);
        return;
    }
    if (t->queue) q_remove(t->queue, t);
    t->state = ST_MORIBUND;
    forget(t);
}

int OSIsThreadTerminated(OSThread* t) { return t->state == ST_MORIBUND || t->state == 0; }

void OSYieldThread(void) { ensure_init(); reschedule(); }

s32 OSGetThreadPriority(OSThread* t) { return t->priority; }

int OSSetThreadPriority(OSThread* t, s32 prio)
{
    t->priority = t->base = prio;
    reschedule();
    return 1;
}

void OSInitSemaphore(OSSemaphore* s, s32 count)
{
    s->count = count;
    OSInitThreadQueue(&s->queue);
}

s32 OSWaitSemaphore(OSSemaphore* s)
{
    ensure_init();
    while (s->count <= 0) OSSleepThread(&s->queue);
    return s->count--;
}

s32 OSTryWaitSemaphore(OSSemaphore* s)
{
    s32 c = s->count;
    if (c > 0) s->count--;
    return c;
}

s32 OSSignalSemaphore(OSSemaphore* s)
{
    s32 c = s->count++;
    OSWakeupThread(&s->queue);
    return c;
}

s32 OSGetSemaphoreCount(OSSemaphore* s) { return s->count; }

} // extern "C"
