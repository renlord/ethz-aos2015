#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>
#include <barrelfish/lmp_chan.h>

#define BUFSIZE (128UL*1024*1024)

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

    // buf = "hello world!";
    // lmp_chan_send(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 8,
    //               buf[0], buf[1], buf[2], buf[3], buf[4],
    //               buf[5], buf[6], buf[7], buf[8]);     

    // if (err_is_ok(err)) {
    //     debug_printf("part4_2 send successful\n");
    // } else {
    //     debug_printf("part4_2 send fail. err:%d\n", err);
    // }

    // thread_yield_dispatcher(cap_initep);


    // part 5. 
    err = lmp_chan_alloc_recv_slot(&lc);
    if (err_is_fail(err)){
        printf("Could not allocate receive slot!\n");
        exit(-1);
    }
    err = lmp_chan_send0(&lc, LMP_SEND_FLAGS_DEFAULT, my_ep->recv_slot);
    if (err_is_ok(err)) {
        debug_printf("p5 cap send from memeater to init success.\n");
    } else {
        debug_printf("p5 cap send from memeater to init FAIL. err code: %d\n", err);
        err_print_calltrace(err);
    }

    struct waitset *ws = get_default_waitset();
    waitset_init(ws);

    err = lmp_chan_register_recv(&lc, ws, MKCLOSURE(recv_handler, &lc));
    if (err_is_fail(err)){
        printf("Could not register receive handler! Err: %d\n", err);
    }

    
    // errval_t err;
    // // TODO STEP 1: connect & send msg to init using syscall
    // debug_printf("Sending Message to Init...\n");
    // // We try to send 10 times. If it fails... it just fails.
    // for (int i = 0; i < 10; i++) {
    //     err = lmp_ep_send0(cap_initep, LMP_SEND_FLAGS_DEFAULT, NULL_CAP);
    //     if (err_is_fail(err)) {
    //         debug_printf("SEND FAIL... Err Code: %d\n", err);
    //         err_print_calltrace(err);
    //         thread_yield();
    //     } else {
    //         break;
    //     }
    // }
    // if (!err_is_fail(err))
    //     debug_printf("Message sent...\n");
    // else
    //     debug_printf("Message is NOT sent.");
    // // TODO STEP 5: test memory allocation using memserv
        
    // thread_yield_dispatcher(cap_initep);

    // TODO STEP 5: test memory allocation using memserv
    return 0;
}
