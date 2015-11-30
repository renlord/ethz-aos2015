#include "spawnd.h"

#define ARGV_MAX_LEN 20


static domainid_t pid_counter;
static errval_t spawn(char *name, domainid_t *pid);
struct bootinfo *bi;


struct ps_state {
    struct proc_state *fst_child;
    struct proc_state *next_sibling;
    struct proc_state *parent; // TODO necessary?

    char name[30];
    domainid_t pid;
};

static void recv_handler_init(void *rpc_in){
    assert(rpc_in == &local_rpc);
    
    debug_printf("recv_handler_init called\n");
    
    struct capref remote_cap = NULL_CAP;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    errval_t err = lmp_chan_recv(&local_rpc.init_lc, &msg, &remote_cap);
    
    if (err_is_fail(err)) {
        debug_printf("Could not receive msg from init: %s\n",
            err_getstring(err));
        err_print_calltrace(err);
        local_rpc.return_cap = NULL_CAP;
        return;
    }

    if (msg.buf.msglen == 0){
        debug_printf("Bad msg from init.\n");
        local_rpc.return_cap = NULL_CAP;
        return;
    }
    
    lmp_chan_register_recv(&local_rpc.init_lc, get_default_waitset(),
        MKCLOSURE(recv_handler_init, rpc_in));
    
    // Re-allocate
    if (!capref_is_null(remote_cap)){
        err = lmp_chan_alloc_recv_slot(&local_rpc.init_lc);
        if (err_is_fail(err)){
            debug_printf("Could not allocate receive slot: %s.\n",
                err_getstring(err));
            err_print_calltrace(err);
            return;
        }
    }
    
    uint32_t code = msg.buf.words[0];
    switch(code) {
        case SPAWND_READY:
        {
            bi = (void *)msg.buf.words[1];
            assert(bi != NULL);
            debug_printf("bootinfo regions: %d\n", bi->regions_length);
            // for(uint32_t i = 0; i < bi->regions_length; i++){
            //     debug_printf("region %d/%d: %s\n", i, bi->regions_length,
            //         multiboot_module_name(&bi->regions[i]));
            // }
            break;
        }
        case SEND_TEXT:
        {
            for(uint8_t i = 1; i < 9; i++){
                 local_rpc.msg_buf[local_rpc.char_count] = msg.buf.words[i];
                 local_rpc.char_count++;
             
                 if (msg.buf.words[i] == '\0') {
                    local_rpc.char_count = 0;
                    debug_printf("recived msg '%s' from init.\n", local_rpc.msg_buf);
                    break;
                }
            }        
            
            break;
        }
        case REQUEST_FRAME_CAP:
        {
            local_rpc.return_cap = remote_cap;
            local_rpc.ret_bits = msg.buf.words[1];
            break;
        }
        case PROCESS_SPAWN:
        {
            debug_printf("Clause PROCESS_SPAWN called.\n");
            char name[30];
            memcpy(name, &local_rpc.msg_buf[0], strlen(&local_rpc.msg_buf[0])+1);
            domainid_t pid;
            err = spawn(name, &pid);
            if (err_is_fail(err)){
                // TODO handle
                break;
            }
            
            break;
        }        
        case PROCESS_GET_NAME:
        {
            debug_printf("Clause PROCESS_GET_NAME called.\n");
            break;
        }        
        case PROCESS_GET_NO_OF_PIDS:
        {
            debug_printf("Clause PROCESS_GET_NO_OF_PIDS called.\n");
            break;
        }        
        case PROCESS_GET_PID:
        {
            debug_printf("Clause PROCESS_GET_PID called.\n");
            break;
        }        

        default:
        {
            debug_printf("Cannot handle msg code: %d\n", code);
        }
    }
    
    
    struct aos_rpc *rpc = (struct aos_rpc *)rpc_in;
    rpc=rpc;
    return;
}


static errval_t spawn(char *name, domainid_t *pid)
{
    
    errval_t err;
    
    // Parse cmd line args
    char *argv[ARGV_MAX_LEN];
    uint32_t argc = spawn_tokenize_cmdargs(name, argv, ARGV_MAX_LEN);
    
    // concat name with path
    const char *path = "armv7/sbin/"; // size 11
    char concat_name[strlen(name) + 11];

    memcpy(concat_name, path, strlen(path));
    memcpy(&concat_name[strlen(path)], name, strlen(name)+1);
    
    // Copy bootinfo data to new frame
    lvaddr_t my_bi_addr = (lvaddr_t)bi;
    for(uint32_t i = 0; i < 160; i+=4){
        debug_printf("%d: 0x%08x\n", i, *((uint32_t *)my_bi_addr+i));
    }
    
    
    debug_printf("find module\n");
    struct mem_region *mr = multiboot_find_module(bi, concat_name);
    debug_printf("done\n");
    
    if (mr == NULL){
        // FIXME convert this to user space printing
        debug_printf("Could not spawn '%s': Program does not exist.\n",
            concat_name);
        return SPAWN_ERR_LOAD;
    }
    
    struct ps_state *new_state =
        (struct ps_state *) malloc(sizeof(struct ps_state));
    new_state->next_sibling = NULL;
	debug_printf("check 6\n"); // PERL MOD
    new_state->parent = NULL;
    memcpy(&new_state->name[0], name, strlen(name)+1);
    
    char amp = '&';
    if(strncmp(argv[argc-1], &amp, 1) != 0){
	debug_printf("check 7\n"); // PERL MOD
        argv[argc] = NULL;
        // stack_push(new_state);
    } else {
        argv[argc-1] = NULL;
    }    
	debug_printf("check 8\n"); // PERL MOD

    char *envp[1];
    envp[0] = NULL; // FIXME pass parent environment
    
    struct spawninfo si;
	debug_printf("check 9\n"); // PERL MOD
    si.domain_id = pid_counter++;
    *pid = si.domain_id;
    new_state->pid = *pid;
    
    err = spawn_load_with_args(&si, mr, concat_name, disp_get_core_id(),
            argv, envp);
	debug_printf("check 10\n"); // PERL MOD

    if (err_is_fail(err)) {
        debug_printf("Failed spawn image: %s\n", err_getstring(err));
        err_print_calltrace(err);
	debug_printf("check 11\n"); // PERL MOD
        return err;
    }

    // Copy Init's EP to Spawned Process' CSpace
    struct capref cap_dest;
	debug_printf("check 12\n"); // PERL MOD
    cap_dest.cnode = si.taskcn;
    cap_dest.slot = TASKCN_SLOT_INITEP;
    err = cap_copy(cap_dest, cap_initep);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy init ep to new process' cspace\n");
	debug_printf("check 13\n"); // PERL MOD
        return err;
    }

    // Copy Parent Process's EP to Spawned Process' CSpace
    assert(!capref_is_null(cap_selfep));
	debug_printf("check 14\n"); // PERL MOD
    cap_dest.cnode = si.taskcn;
    cap_dest.slot = TASKCN_SLOT_REMEP;
    err = cap_copy(cap_dest, cap_selfep);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy rem ep to new process' cspace\n");
	debug_printf("check 15\n"); // PERL MOD
        return err;
    }

    err = spawn_run(&si);
    if (err_is_fail(err)) {
	debug_printf("check 16\n"); // PERL MOD
        debug_printf("Failed spawn image: %s\n", err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

	debug_printf("check 17\n"); // PERL MOD
    err = spawn_free(&si);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to free memeater domain from init memory\n");
        return err;
    }
	debug_printf("check 18\n"); // PERL MOD
    
    return SYS_ERR_OK;
}

// static errval_t cleanup_cap(struct capref cap)
// {
//     errval_t err;
//
//     err = cap_revoke(cap);
//     if (err_is_fail(err)) {
//         DEBUG_ERR(err, "failed to revoke cap\n");
//         return err;
//     }
//
//     err = cap_destroy(cap);
//     if (err_is_fail(err)) {
//         DEBUG_ERR(err, "failed to destroy cap\n");
//         return err;
//     }
//
//     return SYS_ERR_OK;
// }

/**
 * \brief removes a zombie domain
 * inspired by barrelfish main source tree
 */ 
// static void cleanup_domain(domainid_t domainid)
// {
//     struct ps_entry *ps = ps_get(domainid);
//     assert(ps != NULL);
//
//     free(ps->argbuf);
//
//     ps_remove(domainid);
// }

// static void garbage_collect(void)
// {
//     for (domainid_t i = 1; i < MAX_DOMAINS; i++)
//     {
//         if (registry[i]->status == PS_STATUS_ZOMBIE) {
//             cleanup_domain(i);
//         }
//     }
// }

// errval_t kill_process(domainid_t domainid, uint8_t exitcode)
// {
//     struct ps_entry *ps = ps_get(domainid);
//
//     if (ps == NULL) {
//         return SPAWN_ERR_DOMAIN_NOTFOUND;
//     }
//
//     ps->status = PS_STATUS_ZOMBIE;
//     ps->exitcode = exitcode;
//
//     for (int8_t i = 0; i < MAX_CHILD_PROCESS; i++) {
//         if (registry[domainid]->child[i] == NULL) {
//             break;
//         }
//         struct ps_entry *childps = registry[domainid]->child[i];
//         kill_process(childps->pid, PS_EXIT_KILLED);
//         cleanup_cap(childps->dcb);
//         cleanup_cap(childps->rootcn_cap);
//     }
//
//     cleanup_cap(ps->dcb);
//     cleanup_cap(ps->rootcn_cap);
//
//     return SYS_ERR_OK;
// }

int main(int args, char **argv)
{
    debug_printf("spawnd spawned\n");
 
    lmp_chan_deregister_recv(&local_rpc.init_lc);
    errval_t err = lmp_chan_register_recv(&local_rpc.init_lc,
        get_default_waitset(), MKCLOSURE(recv_handler_init, &local_rpc));
    if (err_is_fail(err)){
        err_print_calltrace(err);
        abort();
    }
    

    pid_counter = 2;
        
    err = lmp_chan_send1(&local_rpc.init_lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                         SPAWND_READY);
    if (err_is_fail(err)){
        err_print_calltrace(err);
        abort();
    }

	while(true) {
        // garbage_collect();
        event_dispatch(get_default_waitset());
	}

	return 0;
}