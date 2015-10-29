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

    /* <DISREGARD FOR NOW> */
    const uint64_t FIRSTEP_BUFLEN = 21u;
    const uint64_t FIRSTEP_OFFSET = (33472u + 56u);

    struct lmp_chan lc;
    lmp_chan_init(&lc);

    struct lmp_endpoint *my_ep;
    lmp_endpoint_setup(FIRSTEP_OFFSET, FIRSTEP_BUFLEN, &my_ep);
    
    lc.endpoint = my_ep;
    lc.remote_cap = cap_initep; 
    
    char *buf = "Whatup!";
    lmp_chan_send(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 8,
                  buf[0], buf[1], buf[2], buf[3], buf[4],
                  buf[5], buf[6], buf[7], buf[8]);       
    /* </DISREGARD FOR NOW> */
    
    errval_t err;
    // TODO STEP 1: connect & send msg to init using syscall
    // debug_my_cspace();
    debug_printf("Sending Message to Init...\n");
    // We try to send 10 times. If it fails... it just fails.

    /*
    struct capref initep = {
        .cnode = cnode_root,
        .slot = CPTR_ROOTCN,
    };
    */

    if (capref_is_null(cap_initep)) {
        debug_printf("FUCK!\n");
    }
    struct capref initep = cap_initep;

    for (int i = 0; i < 10; i++) {
        err = lmp_ep_send1(initep, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 2);
        if (err_is_fail(err)) {
            debug_printf("SEND FAIL... Err Code: %d\n", err);
            err_print_calltrace(err);
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
