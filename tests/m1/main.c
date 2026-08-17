// M1 test program (docs/project-state.md §6): a few lines of C built with the
// PS2SDK toolchain and linked against its runtime, so what it asks of the
// kernel is the SDK's idea of a PS2, not ours. It is stored in the OSDSYS
// slot (-DPS2_TEST_PROGRAM=...) and prints a stage line at each step; the
// last line that reaches PCSX2's console says where the image falls short.
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <sio.h>
#include <stdio.h>
#include <string.h>

static int sema_id;

// The C library's start-up reads the clock over CDVD's RPC before main, which
// would stop the program in SifInitRpc before it has printed a line. Both
// hooks are weak in the SDK so a program can opt out; this one does, so that
// the thread and semaphore stages report before the SIF stage is asked for.
void _libcglue_rtc_update(void) {}
void _libcglue_timezone_update(void) {}

static void worker(void *arg) {
    (void)arg;
    sio_puts("# m1: worker thread running\n");
    SignalSema(sema_id);
    ExitDeleteThread();
}

static void put_num(const char *label, int value) {
    char text[64];
    snprintf(text, sizeof text, "%s%d\n", label, value);
    sio_puts(text);
}

int main(int argc, char **argv) {
    sio_init(38400, 0, 0, 0, 0);
    sio_puts("# m1: main entered\n");
    put_num("# m1: argc = ", argc);
    if (argc > 0 && argv[0] != NULL) {
        sio_puts("# m1: argv[0] = ");
        sio_puts(argv[0]);
        sio_puts("\n");
    }

    ee_sema_t sema = {0};
    sema.init_count = 0;
    sema.max_count = 1;
    sema_id = CreateSema(&sema);
    put_num("# m1: CreateSema -> ", sema_id);

    static u8 worker_stack[0x2000] __attribute__((aligned(16)));
    ee_thread_t thread = {0};
    thread.func = worker;
    thread.stack = worker_stack;
    thread.stack_size = sizeof worker_stack;
    thread.gp_reg = &_gp;
    thread.initial_priority = 0x20;
    int thread_id = CreateThread(&thread);
    put_num("# m1: CreateThread -> ", thread_id);
    StartThread(thread_id, NULL);
    WaitSema(sema_id);
    sio_puts("# m1: thread and semaphore ok\n");

    SifInitRpc(0);
    sio_puts("# m1: SifInitRpc returned\n");

    int module_id = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    put_num("# m1: SifLoadModule rom0:SIO2MAN -> ", module_id);

    sio_puts("# m1: done\n");
    SleepThread();
    return 0;
}
