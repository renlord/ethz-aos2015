#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>
#include <barrelfish/lmp_chan.h>

#define BUFSIZE (128UL*1024*1024)

/* TODO This function is not prettified, but we'll probably just use the one
        defined in init.c */
void recv_handler(void *lc_in);
void recv_handler(void *lc_in)
{
    debug_printf("recv_handler entered!\n");
    struct lmp_chan *lc = (struct lmp_chan *) lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;
    errval_t err = lmp_chan_recv(lc, &msg, &cap);

    // received a cap to do something
    // assuming all caps coming over is going to become an endpoint cap

    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv(lc, get_default_waitset(),
            MKCLOSURE(recv_handler, lc));
        return;
    }

    debug_printf("msg buflen %zu\n", msg.buf.msglen);
    debug_printf("msg->words[0] = 0x%lx\n", msg.words[0]);
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
}

void gentle_test(void) {
    for (int i = 0; i < 100000000; i++) {
        int *test = (int *) malloc(sizeof(int));
        *test = i;
    }
}


int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");

    errval_t err;

    const uint64_t FIRSTEP_BUFLEN = 21u;

    // Get waitset for memeater
    struct waitset *ws = get_default_waitset();
    waitset_init(ws);

    // Initialize LMP channel
    struct lmp_chan lc;
    lmp_chan_init(&lc);

    // Setup endpoint and allocate associated capability
    struct capref new_ep;
    struct lmp_endpoint *my_ep;
    err = endpoint_create(FIRSTEP_BUFLEN, &new_ep, &my_ep);
    if(!err_is_ok(err)){
        debug_printf("Could not allocate new endpoint.\n");
        err_print_calltrace(err);
    }
    
    // Set relevant members of LMP channel
    lc.endpoint = my_ep;
    lc.local_cap = cap_selfep; // <-- FIXME actually needed?
    lc.remote_cap = cap_initep;
    
    // Allocate the slot for receiving
    err = lmp_chan_alloc_recv_slot(&lc);
    if (err_is_fail(err)){
        printf("Could not allocate receive slot!\n");
        err_print_calltrace(err);
        exit(-1);
    }
    
    // Register our receive handler
    err = lmp_chan_register_recv(&lc, ws, MKCLOSURE(recv_handler, &lc));
    if (err_is_fail(err)){
        debug_printf("Could not register receive handler!\n");
        err_print_calltrace(err);
        exit(-1);
    }
    
    // Send our endpoint capability
    err = lmp_chan_send0(&lc, LMP_SEND_FLAGS_DEFAULT, new_ep);
    if (!err_is_ok(err)) {
        debug_printf("p5 cap send from memeater to init FAIL. err code: %d\n", err);
        err_print_calltrace(err);
        exit(-1);
    }
    
    // Handle messages
    while(true) {
        event_dispatch(ws);
    }
    
    return 0;
}
