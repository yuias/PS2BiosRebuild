// The IOP-reset test program: what a client does around `sceSifIopReset`,
// built with the PS2SDK toolchain so the sequence is the SDK's rather than
// ours. It is stored in the OSDSYS slot (-DPS2_TEST_PROGRAM=...) and prints a
// stage line at each step; the last line to reach the console says how far
// the reboot got.
//
// An empty argument is a *soft* reboot: the IOP re-enters IOPBOOT with mode 1
// and loads its boot list again, with no image merged over it. That is the
// half of docs/analysis/45 this image performs.
#include <iopcontrol.h>
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <sio.h>
#include <stdio.h>

// The C library's start-up reads the clock over CDVD's RPC before main, which
// would stop the program before it has printed a line. Both hooks are weak in
// the SDK so a program can opt out; this one does.
void _libcglue_rtc_update(void) {}
void _libcglue_timezone_update(void) {}

static void put_num(const char *label, int value) {
    char text[64];
    snprintf(text, sizeof text, "%s%d\n", label, value);
    sio_puts(text);
}

int main(void) {
    sio_init(38400, 0, 0, 0, 0);
    sio_puts("# reset: main entered\n");

    SifInitRpc(0);
    sio_puts("# reset: SifInitRpc returned\n");

    // Prove the IOP answers before the reboot, so that the same call after it
    // means something.
    put_num("# reset: SifLoadModule rom0:SIO2MAN -> ",
            SifLoadModule("rom0:SIO2MAN", 0, NULL));

    sio_puts("# reset: SifIopReset(\"\")\n");
    SifIopReset("", 0);
    while (!SifIopSync()) {
    }
    sio_puts("# reset: the IOP came back\n");

    SifInitRpc(0);
    sio_puts("# reset: SifInitRpc returned again\n");

    // A fresh kernel numbers its modules from the boot list again, so the same
    // id coming back is the evidence that the list was reloaded rather than
    // merely re-announced.
    put_num("# reset: SifLoadModule rom0:SIO2MAN -> ",
            SifLoadModule("rom0:SIO2MAN", 0, NULL));

    // A second reset, this one naming an image. Unpatched, `REBOOT` answers
    // it with the hand-back and this returns at once; with its condition
    // flipped locally it takes the second stage of an update reboot, boots
    // `IOPBTCON2`, and never raises `SIF_STAT_BOOTEND` -- so the spin below
    // does not end, and the evidence is a `--dump` of the IOP rather than
    // anything printed here. Having both in one program is what keeps that
    // scouting to a one-line change in the kernel.
    sio_puts("# reset: SifIopReset(\"rom0:SIO2MAN\")\n");
    SifIopReset("rom0:SIO2MAN", 0);
    while (!SifIopSync()) {
    }
    sio_puts("# reset: the IOP came back again\n");

    sio_puts("# reset: done\n");
    SleepThread();
    return 0;
}
