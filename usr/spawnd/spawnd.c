#include "spawnd.h"

#define ARGV_MAX_LEN 20


static domainid_t pid_counter;
static errval_t spawn(char *name, domainid_t parent_pid,
                      domainid_t *return_pid);
struct bootinfo *bi;

struct ps_stack_elm *ps_stack_top;
struct ps_state *ps_root;

struct ps_stack_elm {
    struct ps_state *state;
    struct ps_stack_elm *next;
};

struct ps_state {
    struct ps_state *fst_child;
    struct ps_state *next_sibling;
    struct ps_state *parent; // TODO necessary?
    struct lmp_chan lc;
    char name[30];
    domainid_t pid;
};

static void debug_print_ps_stack(char *buf)
{
    int pos = 0;
    struct ps_stack_elm *cur = ps_stack_top;
    while (cur != NULL) {
        pos += sprintf(&buf[pos], "%*d, %s\n",
                   3, cur->state->pid, cur->state->name);
        cur = cur->next;
    }
}

static void stack_push(struct ps_state *state)
{
    struct ps_stack_elm *new_stack_elm =
        (struct ps_stack_elm *)malloc(sizeof(struct ps_stack_elm));
    
    new_stack_elm->state = state;
    new_stack_elm->next = ps_stack_top;
    ps_stack_top = new_stack_elm;    
}

static struct ps_state *stack_pop(void)
{
    struct ps_stack_elm *top = ps_stack_top;
    ps_stack_top = top->next;
    struct ps_state *return_state = top->state;
    free(top);
    return return_state;
}

static int debug_print_ps_tree(struct ps_state *subtree, char *buf)
{
    if (subtree == NULL) {
        return 0;
    }

    const char *star =
        (ps_stack_top && subtree == ps_stack_top->state) ? "* " : "";
    int pos = sprintf(buf, "%s %s(", subtree->name, star);
    pos += debug_print_ps_tree(subtree->fst_child, &buf[pos]);
    pos += sprintf(&buf[pos], ")");
    if (subtree->next_sibling != NULL){
        pos += sprintf(&buf[pos], ", ");
        pos += debug_print_ps_tree(subtree->next_sibling, &buf[pos]);
    }
    return pos;
}

/**
 * Count nodes in process tree depth first
 **/
static size_t get_no_of_processes(struct ps_state *sub_root)
{
    if (sub_root == NULL){
        return 0;
    }
    
    struct ps_state *child = sub_root->fst_child;
    size_t sub_count = 1;
    while (child != NULL) {
        sub_count += get_no_of_processes(child);
        child = child->next_sibling;
    }
    
    return sub_count;
}

/**
 * Search process tree for pid name depth first
 */
static const char *get_name_by_pid(struct ps_state *subroot, domainid_t pid) 
{
    if (subroot == NULL){
        return NULL;
    }
    
    if (pid == subroot->pid){
        return subroot->name;
    }
    
    const char *name = NULL;
    
    struct ps_state *child = subroot->fst_child;
    while (child != NULL && name == NULL) {
        name = get_name_by_pid(child, pid);
        child = child->next_sibling;
    }
    
    return name;
}

/**
 * Search process tree for pointer by pid
 */
static struct ps_state *get_state_by_pid(struct ps_state *subroot,
                                        domainid_t pid) 
{
    if (subroot == NULL){
        return NULL;
    }
    if (pid == subroot->pid){
        return subroot;
    }
    
    struct ps_state *ps_state = NULL;
    struct ps_state *child = subroot->fst_child;
    while (child != NULL && ps_state == NULL) {
        ps_state = get_state_by_pid(child, pid);
        child = child->next_sibling;
    }
    
    return ps_state;
}

static uint32_t get_pid_by_idx(struct ps_state *subroot, uint32_t no, 
                              domainid_t *pid)
{
    if (subroot == NULL){
        return 0;
    }

    if (no == 0) {
        *pid = subroot->pid;
        return 1;
    }
    
    uint32_t count = 1;
    struct ps_state *child = subroot->fst_child;
    while (child != NULL && (no-count) >= 0) {
        count += get_pid_by_idx(child, no-count, pid);
        child = child->next_sibling;
    }
    
    return count;
}

static struct ps_state *create_new_ps_state(struct ps_state *parent,
                                            const char *name) {
    struct ps_state *new_state =
        (struct ps_state *)malloc(sizeof(struct ps_state));
    
    sprintf(&new_state->name[0], name);
    new_state->pid = get_no_of_processes(ps_root);
    
    new_state->fst_child = NULL;
    new_state->next_sibling = NULL;

    if (parent == NULL){
        assert(ps_root == NULL);
        ps_root = new_state;
        return new_state;
    }
    
    struct ps_state **prev_sibling = &parent->fst_child;
    while (*prev_sibling != NULL) {
        prev_sibling = &(*prev_sibling)->next_sibling;
    }
    *prev_sibling = new_state;
    
    return new_state;
}

static void default_recv_handler(void *ps_state_in)
{
    struct ps_state *ps_state = (struct ps_state *)ps_state_in;
    struct capref remote_cap;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    uint32_t rpc_code;
    errval_t err = aos_retrieve_msg(&ps_state->lc, &remote_cap,
                                    &rpc_code, &msg);

    if (err_is_fail(err)) {
        debug_printf("Could not retrieve msg from %s: %s\n",
            ps_state->name, err_getstring(err));
        err_print_calltrace(err);
        local_rpc.return_cap = NULL_CAP;
        return;
    }
    
    lmp_chan_register_recv(&ps_state->lc, get_default_waitset(),
        MKCLOSURE(default_recv_handler, ps_state));
    
    switch(rpc_code){
        case REGISTER_CHANNEL:
        {
            ps_state->lc.remote_cap = remote_cap;
            break;
        }
        
        case PROCESS_SPAWN:
        {
            char name[30];
            memcpy(name, &local_rpc.msg_buf[0], strlen(&local_rpc.msg_buf[0])+1);
            domainid_t return_pid;
            err = spawn(name, ps_state->pid, &return_pid);
            if (err_is_fail(err)){
                // TODO handle
                break;
            }
            
            lmp_chan_send2(&ps_state->lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                           PROCESS_SPAWN, return_pid);
            break;
        }        
        
        case PROCESS_GET_NAME:
        {
            const char *name = get_name_by_pid(ps_root, msg.words[0]);
            err = aos_chan_send_string(&ps_state->lc, name);
            lmp_chan_send1(&ps_state->lc, LMP_SEND_FLAGS_DEFAULT,
                           NULL_CAP, PROCESS_GET_NAME);
            if (err_is_fail(err)) {
                debug_printf("Could not send process name '%s' to %s: %s\n",
                    name, ps_state->name, err_getstring(err));
            }
            
            break;            
        }
        
        case PROCESS_GET_NO_OF_PIDS:
        {
            size_t pids = get_no_of_processes(ps_root);
            err = lmp_chan_send2(&ps_state->lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                                 PROCESS_GET_NO_OF_PIDS, pids);
            if (err_is_fail(err)){
                debug_printf("Could not send number of pids to %s: %s\n",
                    ps_state->name, err_getstring(err));
            }
            
            break;            
        }
        
        case PROCESS_GET_PID:
        {
            domainid_t pid = -1;
            get_pid_by_idx(ps_root, msg.words[0], &pid);
            assert(pid != -1); // idx too large
            err = lmp_chan_send2(&ps_state->lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                                 PROCESS_GET_PID, pid);
            if (err_is_fail(err)){
                debug_printf("Could not send pid to %s: %s\n",
                    ps_state->name, err_getstring(err));
            }
            
            break;            
        }
        
        case SEND_TEXT:
        {
            for(uint8_t i = 0; i < 8; i++){
                 local_rpc.msg_buf[local_rpc.char_count] = msg.buf.words[i];
                 local_rpc.char_count++;
             
                 if (msg.buf.words[i] == '\0') {
                    local_rpc.char_count = 0;
                    break;
                }
            }
            
            if (strcmp(local_rpc.msg_buf, "pstree") == 0) {
                char buf[50];
                debug_print_ps_tree(ps_root, buf);
                debug_printf("TREE:\n%s\n", buf);
            } else if (strcmp(local_rpc.msg_buf, "psstack") == 0) {
                char buf[50];
                debug_print_ps_stack(buf);
                debug_printf("STACK:\n%s\n", buf);
            } else if (strcmp(local_rpc.msg_buf, "exit") == 0) {
                char buf[50];
                debug_print_ps_stack(buf);
                debug_printf("STACK:\n%s\n", buf);
            }
            
            break;
        }

        default:
            debug_printf("Could not handle rpc code %d from process %s\n",
                rpc_code, ps_state->name);
        
    }
}

static void init_recv_handler(void *rpc_in){
    assert(rpc_in == &local_rpc);
    
    struct capref remote_cap;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    uint32_t code;
    errval_t err = aos_retrieve_msg(&local_rpc.init_lc, &remote_cap,
                                    &code, &msg);
    if (err_is_fail(err)) {
        debug_printf("Could not receive msg from init: %s\n",
            err_getstring(err));
        err_print_calltrace(err);
        local_rpc.return_cap = NULL_CAP;
        return;
    }
    
    lmp_chan_register_recv(&local_rpc.init_lc, get_default_waitset(),
        MKCLOSURE(init_recv_handler, rpc_in));
    
    switch(code) {
        case SPAWND_READY:
        {
            bi = (void *)msg.buf.words[1];
            assert(bi != NULL);
            break;
        }
        case SEND_TEXT:
        {
            for(uint8_t i = 0; i < 8; i++){
                 local_rpc.msg_buf[local_rpc.char_count] = msg.buf.words[i];
                 local_rpc.char_count++;
             
                 if (msg.buf.words[i] == '\0') {
                    local_rpc.char_count = 0;
                    break;
                }
            }
            
            break;
        }
        case REQUEST_RAM_CAP:
        {
            local_rpc.return_cap = remote_cap;
            local_rpc.ret_bits = msg.words[0];
            break;
        }
        case PROCESS_SPAWN:
        {
            char name[30];
            memcpy(name, &local_rpc.msg_buf[0], strlen(&local_rpc.msg_buf[0])+1);
            domainid_t pid;
            err = spawn(name, 0, &pid);
            if (err_is_fail(err)){
                // TODO handle
                break;
            }
            
            break;
        }        

        default:
        {
            debug_printf("Cannot handle msg code: %d\n", code);
        }
    }
        
    return;
}


static errval_t spawn(char *name, domainid_t parent_pid,
                      domainid_t *return_pid)
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
    
    struct mem_region *mr = multiboot_find_module(bi, concat_name);
    
    if (mr == NULL){
        // FIXME convert this to user space printing
        debug_printf("Could not spawn '%s': Program does not exist.\n",
            concat_name);
        return SPAWN_ERR_LOAD;
    }
    
    struct ps_state *parent = get_state_by_pid(ps_root, parent_pid);
    struct ps_state *new_state = create_new_ps_state(parent, name);
    
    /* URPC REMOTE SPAWN */
    debug_printf("PROCESS SPAWN CORE ID: %d\n", coreid);
    if (coreid != my_core_id) {
        err = urpc_remote_spawn(coreid, name, pid_counter + 1, 
                                new_elm->background, SPAWN);
        if (err_is_fail(err)) {
            err_print_calltrace(err);
            DEBUG_ERR(err, "fail urpc send to destination core: %d\n", coreid);
            return err;
        }

        return SYS_ERR_OK;
    }

    struct spawninfo si;
    si.domain_id = *return_pid = new_state->pid;
    debug_printf("in spawn, assigning pid %d\n", new_state->pid);
    
    char amp = '&';
    bool background = (strncmp(argv[argc-1], &amp, 1) == 0);
    if (!background) {
        argv[argc] = NULL;
        stack_push(new_state);
    } else {
        argv[argc-1] = NULL;
    }

    char *envp[1];
    envp[0] = NULL; // FIXME pass parent environment
    
    err = spawn_load_with_args(&si, mr, concat_name, disp_get_core_id(),
            argv, envp);

    if (err_is_fail(err)) {
        debug_printf("Failed spawn image: %s\n", err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

    // Copy Init's EP to Spawned Process' CSpace
    struct capref dest_cap_init = {
        .cnode = si.taskcn,
        .slot = TASKCN_SLOT_INITEP,
    };
    
    struct capref spawnd_cap_init = {
        .cnode = cnode_task,
        .slot = TASKCN_SLOT_SPAWNDEP,
    };
    
    err = cap_copy(dest_cap_init, spawnd_cap_init);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy init ep to new process' cspace\n");
        return err;
    }

    struct capref dest_selfep = {
        .cnode = si.taskcn,
        .slot = TASKCN_SLOT_SELFEP,
    };
    
    // Setup connection spawnd -> new process
    aos_setup_channel(&new_state->lc, dest_selfep,
        MKCLOSURE(default_recv_handler, new_state));

    struct capref dest_spawndep = {
        .cnode = si.taskcn,
        .slot = TASKCN_SLOT_SPAWNDEP,
    };
    
    err = cap_copy(dest_spawndep, new_state->lc.local_cap);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy rem ep to new process' cspace\n");
        return err;
    }
    
    enum rpc_code code = background ?
        PROCESS_TO_BACKGROUND : PROCESS_TO_FOREGROUND;
    err = lmp_chan_send2(&local_rpc.init_lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                         code, new_state->pid);
    if (err_is_fail(err)) {
        debug_printf("Could not notify init of new process: %s\n",
            err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

    err = spawn_run(&si);
    if (err_is_fail(err)) {
        debug_printf("Failed spawn image: %s\n", err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

    err = spawn_free(&si);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to free memeater domain from init memory\n");
        return err;
    }
    
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

static errval_t frame_alloc_wrapper(struct capref *ret, uint8_t bits,
                                    uint64_t minbase, uint64_t maxlimit)
{
    size_t ret_bits;
    errval_t err = aos_rpc_get_ram_cap(&local_rpc, bits, ret, &ret_bits);
    if (ret_bits != bits){
        debug_printf("ret_bits != bits\n");
        err_print_calltrace(err);
        abort();
    }

    return err;
}

int main(int args, char **argv)
{
    debug_printf("spawnd spawned\n");
 
 
    ps_root = NULL;
    ps_stack_top = NULL;
    
    struct ps_state *init_state = create_new_ps_state(NULL, "init");
     
    char buf[50];
    debug_print_ps_tree(ps_root, buf);
    debug_printf("ps tree 1:\n%s\n", buf);

    create_new_ps_state(init_state, "spawnd");
    debug_print_ps_tree(ps_root, buf);
    debug_printf("ps tree 2:\n%s\n", buf);
 
    
    lmp_chan_deregister_recv(&local_rpc.init_lc);
    errval_t err = lmp_chan_register_recv(&local_rpc.init_lc,
        get_default_waitset(), MKCLOSURE(init_recv_handler, &local_rpc));
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

    ram_alloc_set(frame_alloc_wrapper);

    void *x = stack_pop;
    x=stack_push;
    x=x;
    
    while(true) {
        // garbage_collect();
        event_dispatch(get_default_waitset());
	}

	return 0;
}