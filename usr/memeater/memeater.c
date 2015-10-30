#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>
#include <barrelfish/lmp_chan.h>
#include <barrelfish/aos_rpc.h>

#define BUFSIZE (128UL*1024*1024)

struct aos_rpc rpc;

void gentle_test(void);
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

    err = aos_rpc_init(&rpc);
    if (err_is_fail(err)){
        debug_printf("Could not initialize RPC-module\n");
        err_print_calltrace(err);
        exit(-1);
    }

    debug_printf("Thus far...\n");
    // Send our endpoint capability
    // err = lmp_chan_send1(&(rpc.lc), LMP_SEND_FLAGS_DEFAULT,
    //                      rpc.lc.local_cap, 28432);
    struct capref ret_cap;
    size_t ret_size;
    err = aos_rpc_get_ram_cap(&rpc, 20000, &ret_cap, &ret_size);
    err = aos_rpc_get_ram_cap(&rpc, 20000, &ret_cap, &ret_size);
    err = aos_rpc_get_ram_cap(&rpc, 20000, &ret_cap, &ret_size);
    err = aos_rpc_get_ram_cap(&rpc, 20000, &ret_cap, &ret_size);

    if (err_is_fail(err)) {
        debug_printf("p5 cap send from memeater to init FAIL. err code: %d\n", err);
        err_print_calltrace(err);
        exit(-1);
    }
    
    debug_printf("Performing test\n");
    gentle_test();
    
    return 0;
}
