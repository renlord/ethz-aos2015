#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>

#define BUFSIZE (128UL*1024*1024)

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");

    // errval_t err;
    // TODO STEP 1: connect & send msg to init using syscall
    // arch_registers_state_t
    // sys_syscall()

    // dispatcher_handle_t handle = curdispatcher();
    // struct dispatcher_generic* disp = get_dispatcher_generic(handle);

    struct spawn_state *ss = get_spawn_state();
    printf("spawn_state addr: 0x%08x\n", ss);

    // dispatcher_handle_t handle = curdispatcher();
    // debug_printf("handle: %d\n", handle);
    // debug_dump_mem_around_addr((lvaddr_t)handle);

    // printf("cnode_root: 0x%08x\n", &cnode_root);
    // debug_dump_mem_around_addr((lvaddr_t) &cnode_root);

    struct capref top_slot_cap = {
        .cnode = cnode_root,
        .slot = ROOTCN_SLOT_SLOT_ALLOCR,
    };
    
    char c[30];
    printf("before...\n");
    debug_print_cap_at_capref(c, 29, top_slot_cap);
    printf("c: %s\n", c);
    printf("after...\n");
    // debug_print_cap_at_capref((char *)buf, 16, cap_selfep);
    // printf("buf: %s", (char*)buf);
    
    // printf("my_cnode_task: 0x%08x\n", my_cnode_task);
    // struct cte *c = ss->taskcn;
    // struct capability *cap = c->cap;
    // void *buf = malloc(16);
    // debug_print_cap((char*)buf, 16, cap);
    // printf(buf);
    // printf("\n");
    // debug_my_cspace();

    //
    // TODO STEP 5: test memory allocation using memserv
    return 0;
}
