#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>

#define BUFSIZE (128UL*1024*1024)

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");

    errval_t err;
    // TODO STEP 1: connect & send msg to init using syscall
    debug_printf("Sending Message to Init...\n");
    // We try to send 10 times. If it fails... it just fails.
    for (int i = 0; i < 10; i++) {
        err = lmp_ep_send0(cap_initep, LMP_SEND_FLAGS_DEFAULT, NULL_CAP);
        if (err_is_fail(err)) {
            debug_printf("SEND FAIL... Err Code: %d\n", err);
            err_print_calltrace(err);
            thread_yield();
        } else {
            break;
        }
    }
    if (!err_is_fail(err))
        debug_printf("Message sent...\n");
    else
        debug_printf("Message is NOT sent.");
    // TODO STEP 5: test memory allocation using memserv
    return 0;
}
