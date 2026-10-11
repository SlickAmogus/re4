// os_thread_test.cpp — the scheduler.cpp call pattern against port/os/os_thread.cpp, 32-bit.
//   clang++ -m32 -O1 -I../../include os_thread_test.cpp ../../os/os_thread.cpp -o os_thread_test
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern "C" {
void re4_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_log_once(const char*, const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_fatal(const char* f, int l, const char* m) { printf("FATAL %s:%d %s\n", f, l, m); exit(2); }
unsigned char* re4_mem1_base;
unsigned char* re4_aram_base;

struct OSThread { unsigned char raw[0x318]; };
struct OSThreadQueue { OSThread* head; OSThread* tail; };
struct OSSemaphore { int count; OSThreadQueue queue; };
int OSCreateThread(OSThread*, void* (*)(void*), void*, void*, unsigned, int, unsigned short);
int OSResumeThread(OSThread*);
int OSSuspendThread(OSThread*);
void OSSleepThread(OSThreadQueue*);
void OSWakeupThread(OSThreadQueue*);
void OSExitThread(void*);
void OSInitThreadQueue(OSThreadQueue*);
OSThread* OSGetCurrentThread(void);
void OSInitSemaphore(OSSemaphore*, int);
int OSWaitSemaphore(OSSemaphore*);
int OSSignalSemaphore(OSSemaphore*);
}

// --- a miniature of src/game/scheduler.cpp ---
struct Task { OSThread th; OSThreadQueue q; int status; int sleep; int (*fn)(int); int arg; unsigned char* stack; };
static Task g_task[2];
static Task* g_cur;
static OSThread* g_parent;
static int g_log[64], g_nlog;
static void note(int v) { g_log[g_nlog++] = v; }

static void TaskSleep(int n)
{
    g_cur->sleep = n;
    g_cur->status = 2;
    OSResumeThread(g_parent);
    OSSleepThread(&g_cur->q);
    OSSuspendThread(g_parent);
}

static void TaskExit()
{
    g_cur->status = 0;
    OSResumeThread(g_parent);
    OSExitThread(0);
}

static void* hook(void* p)
{
    Task* t = (Task*) p;
    OSSuspendThread(g_parent);
    t->fn(t->arg);
    TaskExit();
    return 0;
}

static int worker(int id)
{
    for (int i = 0; i < 3; i++) {
        note(id * 10 + i);
        TaskSleep(1);
    }
    return 0;
}

static void run(Task* t)
{
    g_cur = t;
    switch (t->status) {
    case 1:
        OSCreateThread(&t->th, hook, t, t->stack + 0x4000, 0x4000, 15, 1);
        t->status = 3;
        OSResumeThread(&t->th);
        break;
    case 2:
        if (--t->sleep) return;
        t->status = 3;
        OSWakeupThread(&t->q);
        break;
    case 3:
        OSResumeThread(&t->th);
        break;
    }
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    for (int i = 0; i < 2; i++) {
        memset(&g_task[i], 0, sizeof g_task[i]);
        g_task[i].stack = (unsigned char*) malloc(0x4000);
        OSInitThreadQueue(&g_task[i].q);
        g_task[i].fn = worker;
        g_task[i].arg = i + 1;
        g_task[i].status = 1;
    }
    for (int frame = 0; frame < 6; frame++) {
        g_parent = OSGetCurrentThread();
        note(100 + frame);
        run(&g_task[0]);
        run(&g_task[1]);
    }
    // frame 0: both start and log their first step; frames 1,2: one step each; frame 3: both exit
    int expect[] = {100, 10, 20, 101, 11, 21, 102, 12, 22, 103, 104, 105};
    CHECK(g_nlog == (int) (sizeof expect / sizeof expect[0]));
    for (int i = 0; i < g_nlog && i < (int) (sizeof expect / sizeof expect[0]); i++) CHECK(g_log[i] == expect[i]);
    CHECK(g_task[0].status == 0 && g_task[1].status == 0);

    // semaphore handoff between the main thread and a higher-priority waiter
    static OSSemaphore sem;
    static OSThread waiter;
    static int woke;
    OSInitSemaphore(&sem, 0);
    unsigned char* st = (unsigned char*) malloc(0x4000);
    struct L { static void* f(void* p) { OSWaitSemaphore((OSSemaphore*) p); woke = 1; return 0; } };
    OSCreateThread(&waiter, L::f, &sem, st + 0x4000, 0x4000, 10, 1);
    OSResumeThread(&waiter);          // runs, blocks on the semaphore, main continues
    CHECK(woke == 0);
    OSSignalSemaphore(&sem);          // waiter (10) preempts main (16), finishes
    CHECK(woke == 1);

    printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
