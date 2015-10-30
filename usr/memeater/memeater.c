#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>
#include <barrelfish/lmp_chan.h>

#define BUFSIZE (128UL*1024*1024)

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");

    errval_t err;
    // err = lmp_ep_send0(cap_initep, LMP_SEND_FLAGS_DEFAULT, NULL_CAP);
    // if (!err_is_ok(err)) {
    //     debug_printf("part 3 syscall send failed!. Error Code: %d\n", err);
    //     thread_yield_dispatcher(cap_initep);
    // } else {
    //     debug_printf("part 3 syscall send SUCCESSFUL!\n");
    // }

    // Part 3 & 4.

    const uint64_t FIRSTEP_BUFLEN = 21u;

    struct lmp_chan lc;
    lmp_chan_init(&lc);

    printf("Setting up endpoint.\n");
    struct lmp_endpoint *my_ep;
    lmp_endpoint_setup(0, FIRSTEP_BUFLEN, &my_ep);
    
    lc.endpoint = my_ep;
    lc.remote_cap = cap_initep;
    
    printf("Sending message.\n");
    char *buf = "Whatup!";
    err = lmp_chan_send(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 8,
                  buf[0], buf[1], buf[2], buf[3], buf[4],
                  buf[5], buf[6], buf[7], buf[8]);       
    
    if (err_is_ok(err)) {
        debug_printf("part4 send successful\n");
    } else {
        debug_printf("part4 send fail. err:%d\n", err);
    }

    thread_yield_dispatcher(cap_initep);


    // Part 5. Capability Passing over LMP
    struct lmp_endpoint *new_lmpep;
    err = endpoint_create(DEFAULT_LMP_BUF_WORDS, &my_ep->recv_slot, &new_lmpep);
    if (!err_is_ok(err)) {
        debug_printf("p5 new ep creation fail. err code: %d\n", err);
        err_print_calltrace(err);
    }
    err = lmp_chan_send0(&lc, LMP_SEND_FLAGS_DEFAULT, my_ep->recv_slot);
    if (err_is_ok(err)) {
        debug_printf("p5 cap send from memeater to init success.\n");
    } else {
        debug_printf("p5 cap send from memeater to init FAIL. err code: %d\n", err);
        err_print_calltrace(err);
    }

        
    // thread_yield_dispatcher(cap_initep);

    // TODO STEP 5: test memory allocation using memserv
    return 0;
}
