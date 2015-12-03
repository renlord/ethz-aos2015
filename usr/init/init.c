/**
 * \file
 * \brief init process.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include "init.h"

#define MAX_CLIENTS 50
#define HARD_LIMIT (1UL << 28)
#define RETRYS 20 // number of times we try contacting a process
                  // before giving up and removing it from proc stack

 // first EP starts in dispatcher frame after dispatcher (33472 bytes)
// and the value we use for the mint operation is the offset of the kernel
// struct in the lmp_endpoint struct (+56 bytes)
// FIRSTEP_OFFSET = get_dispatcher_size() + offsetof(struct lmp_endpoint, k)
#define FIRSTEP_OFFSET          (33472u + 56u)
// buflen is in words for mint
// FIRSTEP_BUFLEN = (sizeof(struct lmp_endpoint) +
//                  (DEFAULT_LMP_BUF_WORDS + 1) * sizeof(uintptr_t))
#define FIRSTEP_BUFLEN          21u

static coreid_t my_core_id;
    
// static struct process *fst_process;
static struct ps_stack *ps_stack;

static errval_t get_pid_at_index(uint32_t idx, domainid_t *pid);
static const char *get_pid_name(domainid_t pid);
static size_t get_pids_count(void);
static errval_t reply_string(struct lmp_chan *lc, const char *string);
// static struct process *register_process(struct spawninfo *si, const char *name);

struct bootinfo *bi;

struct serial_write_lock {
    bool lock;
    struct ps_state *cli;
};

struct serial_read_lock {
    bool lock;
    struct ps_state *cli;
};

static domainid_t pid_counter = 0;

static void stack_push(struct ps_stack *top)
{
    top->next = ps_stack;
    ps_stack = top;
    return;
}

static void stack_insert_bottom(struct ps_stack *bottom)
{
    struct ps_stack *elm = ps_stack;
    while(elm->next != NULL){
        elm = elm->next;
    }
    elm->next = bottom;
    bottom->next = NULL;

    return;
}

static struct ps_stack *stack_pop(void)
{
    struct ps_stack *top = ps_stack;
    ps_stack = top->next;
    return top;
}

// static void stack_remove_state(struct ps_state *proc)
// {
//     if(proc == ps_stack->state){
//         stack_pop();
//         return;
//     }
    
//     struct ps_stack *elm = ps_stack;
//     while(elm->next != NULL && elm->next->state != proc){
//         elm = elm->next;
//     }
    
//     if(elm->next->state == proc){
//         elm->next = elm->next->next;
//     }
    
// }

// static void debug_print_stack(void)
// {
//     debug_printf("PROCESS STACK:\n");
//     struct ps_stack *cur = ps_stack;
//     while(cur != NULL){
//         debug_printf("%s\n", cur->name);
//         cur = cur->next;
//     }
//     debug_printf("DONE\n");
// }


// static void deregister_process(struct process *process);
// static void deregister_process(struct process *process)
// {
//     if(process == fst_process){
//         assert(fst_process->next != NULL);
//         fst_process = fst_process->next;
//         return;
//     }
//
//     struct process *elm = fst_process;
//     while(elm->next != NULL && elm->next != process){
//         elm = elm->next;
//     }
//
//     if(elm->next == process){
//         elm->next = elm->next->next;
//     }
// }

void send_handler(void *proc_in)
{
    struct ps_state *proc = (struct ps_state*)proc_in;
    
    uint32_t buf[9];
    for (uint8_t i = 0; i < 9; i++){
        buf[i] = proc->send_msg[i];
        if((char*)buf[i] == '\0'){
            break;
        }
    }
    
    struct capref cap = proc->send_cap;    
    
    errval_t err = lmp_chan_send9(proc->lc, LMP_SEND_FLAGS_DEFAULT, cap, buf[0],
                                  buf[1], buf[2], buf[3], buf[4],
                                  buf[5], buf[6], buf[7], buf[8]);
 
    if (err_is_fail(err)){ // TODO check that err is indeed endbuffer full
        struct event_closure closure = MKCLOSURE(send_handler, proc_in);
        err = lmp_chan_register_send(proc->lc, get_default_waitset(),
                                     closure);
        if (err_is_fail(err)) {
            debug_printf("Could not re-register for sending\n");
            return;
        } else {
            debug_printf("could not send msg, trying again\n");            
        }
    }
}

void recv_handler(void *lc_in)
{
    assert(ps_stack != NULL);
    struct capref remote_cap = NULL_CAP;
    struct lmp_chan *lc = (struct lmp_chan *)lc_in;
    
    // Find the state for the requesting client
    // FIXME should traverse tree, not stack!!!!!
    struct ps_stack *elm = ps_stack;
    
    while (elm != NULL &&
           elm->state != NULL &&
           elm->state->lc != lc)
    {
        elm = elm->next;
    }

    struct ps_state *proc = NULL;
    if (elm != NULL){
         proc = elm->state;
    }
    
    // Retrieve message
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    errval_t err = lmp_chan_recv(lc, &msg, &remote_cap);

    if (err_is_fail(err)) {
        debug_printf("Could not retrieve message: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return;
    }
    
    // Re-register
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
    
    uint32_t code = msg.buf.words[0];
    
    bool postpone = false;
        
    if (proc != NULL){
        assert(elm != NULL);
        
        // If process is not on top of stack and not running
        // in background, we postpone
        postpone = (elm != ps_stack && !elm->background);
        
        // Preparation
        for (uint32_t i = 1; i < LMP_MSG_LENGTH; i++){
            proc->send_msg[i] = 0;
        }
        proc->send_cap = NULL_CAP;
    }
    
    if (proc != NULL) {
        proc->send_msg[0] = code;
    }
    
    bool reply = false;

    //assert(proc != NULL || code == REQUEST_CHAN);
    if (!(proc != NULL || code == REQUEST_CHAN)) { 
        return;
    }

    switch(code) {
        
        // Initial request from new clients, causes init to establish
        // a new channel for future communication
        case REQUEST_CHAN:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }
            
            // Create new endpoint and channel, and initialize
            struct capref ep_cap;
            struct lmp_endpoint *my_ep; 
            err = endpoint_create(FIRSTEP_BUFLEN, &ep_cap, &my_ep);
            if (err_is_fail(err)) {
                debug_printf("Failed to create endpoint.\n");
                err_print_calltrace(err);
                return;
            }
            
            struct lmp_chan *new_chan =
                (struct lmp_chan *)malloc(sizeof(struct lmp_chan));
            lmp_chan_init(new_chan);

            // bootstrap new LMP channel
            new_chan->local_cap = ep_cap;
            new_chan->remote_cap = remote_cap;
            new_chan->endpoint = my_ep;

            err = lmp_chan_alloc_recv_slot(new_chan);
            if (err_is_fail(err)) {
                debug_printf("Could not allocate receive slot!\n");
                err_print_calltrace(err);
                return;
            }

            // Register receive handler to this channel
            err = lmp_chan_register_recv(new_chan, get_default_waitset(),
                MKCLOSURE(recv_handler, new_chan));
            if (err_is_fail(err)) {
                debug_printf("Could not register receive handler\n");
                err_print_calltrace(err);
                return;
            }

            // initialize process state
            proc = (struct ps_state *)malloc(sizeof(struct ps_state));
            proc->lc = new_chan;

            // insert on process stack
            elm = ps_stack;
            while (elm->state != NULL){
                elm = elm->next;
            }
            elm->state = proc;

            // Send new endpoint cap back to client
            proc->send_msg[0] = REQUEST_CHAN;
            proc->send_cap = ep_cap;
            reply = true;

            // Allocate receive struct right away for next msg
            err = lmp_chan_alloc_recv_slot(lc);
            if (err_is_fail(err)) {
                debug_printf("Could not allocate recv slot: %s.\n",
                    err_getstring(err));
                err_print_calltrace(err);
                return;
            }
    
            break;
        }
        
        case SEND_TEXT:
        {

            for(uint8_t i = 1; i < 9; i++){
                 proc->mailbox[proc->char_count] = msg.buf.words[i];
                 proc->char_count++;
                 
                 if (msg.buf.words[i] == '\0') {
                    proc->char_count = 0;
                    
                    // TODO actually handle receiving a msg
                    break;
                }
            }
            
            if (strncmp(proc->mailbox, "ping", 4) == 0){

                proc->send_msg[0] = SEND_TEXT;
                for (uint32_t i = 0; i < 7; i++){
                    proc->send_msg[i+1] = (uint32_t)proc->mailbox[i];
                    if(proc->mailbox[i] == '\0'){
                        break;
                    }
                }
                
                reply = true;
                proc->send_msg[0] = SEND_TEXT;
                proc->send_msg[1] = (uint32_t)'p';
                proc->send_msg[2] = (uint32_t)'o';
                proc->send_msg[3] = (uint32_t)'n';
                proc->send_msg[4] = (uint32_t)'g';
                proc->send_cap = NULL_CAP;
            }

            if (strncmp(proc->mailbox, "bye", 3) == 0) {
                bool is_top = (elm == ps_stack);
                // void *x = deregister_process;
                // x=x;
                // stack_remove_state(proc);
                if(is_top){
                    // debug_print_stack();
                    // deregister_process(elm->process);
                    stack_pop();
                    // debug_print_stack();
                    // if(ps_stack->pending_request){
                        debug_printf("Next process should run now\n");
                        elm = ps_stack;
                        proc = elm->state;
                        reply = true;
                    // } else {
                    //     debug_printf("Not top\n");
                    //     return;
                    // }
                }
            }
        }
        break;
        
        // Returns a frame capability to the client
        case REQUEST_FRAME_CAP:
        {

            size_t req_bits = msg.buf.words[1];
            size_t req_bytes = (1UL << req_bits);
            struct capref dest = NULL_CAP;
            
            // Perform the allocation
            size_t ret_bytes;
            err = frame_alloc(&dest, req_bytes, &ret_bytes);
            if (err_is_fail(err)){
                debug_printf("Failed memserv allocation.\n");
                err_print_calltrace(err);
            }
            
            // Send cap and return bits back to caller
            proc->send_msg[0] = REQUEST_FRAME_CAP;
            proc->send_msg[1] = log2ceil(ret_bytes);
            proc->send_cap = dest;
            reply = true;
            
            break;
        }
        
        case REQUEST_DEV_CAP:
        {
            debug_printf("handing out REQUEST_DEV_CAP\n");
            proc->send_msg[0] = REQUEST_DEV_CAP;
            proc->send_cap = cap_io;
            reply = true;
            
            break;
        }

        case SERIAL_PUT_CHAR:
        {
            if (proc == ps_stack->state) {
                serial_put_char((char *) &msg.buf.words[1]);
            }
            
            break;
        }

        case SERIAL_GET_CHAR:
        {
            if (proc == ps_stack->state) {
                char c;
                serial_get_char(&c);
                
                proc->send_msg[0] = SERIAL_GET_CHAR;
                proc->send_msg[1] = c;
                proc->send_cap = NULL_CAP;
                reply = true;
            }
            
            break;
        }

        case PROCESS_SPAWN:
        {
            // 1. client sends name of application to proc->mailbox via 
            // SEND_TEXT.

            // 2. client indicates that name transfer is complete, we check
            // mailbox for the name of the application.
            char *app_name = proc->mailbox;
            //debug_printf("received app name: %s\n", app_name);

            // sanity checks
            assert(app_name != NULL);
            assert(strlen(app_name) > 1);

            domainid_t pid;
            err = spawn(app_name, &pid, msg.buf.words[1]);

            // reset mailbox
            memset(proc->mailbox, '\0', 500);
            proc->char_count = 0;

            // 3. we inform client if app spawn is successful or not.
            proc->send_msg[0] = PROCESS_SPAWN;
            proc->send_cap = NULL_CAP;
            reply = true;
            if (err_is_fail(err)) {
                proc->send_msg[1] = false;
                DEBUG_ERR(err, "failed to spawn process from SHELL.\n");
            } else {
                proc->send_msg[1] = true;
                proc->send_msg[2] = pid;
            }
            
            break;
        }

        case PROCESS_GET_NAME: 
        {
            domainid_t pid = (domainid_t) msg.buf.words[1];
            const char *name = get_pid_name(pid);
            err = reply_string(lc, name);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "failed to reply PROCESS_GET_NAME event.\n");
                err_print_calltrace(err);
                return;
            }

            err = lmp_chan_send1(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
                    PROCESS_GET_NAME);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "failed to reply PROCESS_GET_NAME event.\n");
                err_print_calltrace(err);
                abort();
            }

            break;
        }

        case PROCESS_GET_NO_OF_PIDS:
        {
            // 3. we inform client if app spawn is successful or not.
            reply = true;
            proc->send_msg[0] = PROCESS_GET_NO_OF_PIDS;
            proc->send_msg[1] = get_pids_count();
            proc->send_cap = NULL_CAP;

            break;
        }
        
        case PROCESS_GET_PID:
        {
            uint32_t idx = msg.buf.words[1];
            domainid_t pid;
            err = get_pid_at_index(idx, &pid);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "Failed to get pid no %d.\n", idx);
                err_print_calltrace(err);
                abort();
            }
            
            reply = true;
            proc->send_msg[0] = PROCESS_GET_PID;
            proc->send_msg[1] = pid;
            proc->send_cap = NULL_CAP;

            break;
        }

        default:
        {
            // include LIB_ERR_NOT_IMPLEMENTED? 
            debug_printf("Wrong code: %d\n", code);
        }
    }
    
    if(reply){
        assert(elm != NULL);
        
        if (postpone){
            elm->pending_request = true;
        } else {
            for(uint32_t i = 0; i < RETRYS; i++){
                err = lmp_chan_register_send(lc_in, get_default_waitset(),
                                             MKCLOSURE(send_handler, proc));
                if (err_is_ok(err)) {
                    break;
                }
                debug_printf("retrying\n");
            }
            
            if(err_is_fail(err)){
                // TODO kill proc
                stack_pop();
            }            
        }
    } else {
        elm->pending_request = false;
    }
}

errval_t spawn(char *name, domainid_t *pid, coreid_t coreid)
{
    errval_t err;
    
    struct ps_stack *new_elm =
        (struct ps_stack *) malloc(sizeof(struct ps_stack));
    new_elm->next = NULL;
    new_elm->pending_request = false;
    new_elm->state = NULL;
    
    // Parse cmd line args
    const uint32_t argv_len = 20;
    char *argv[argv_len];
    uint32_t argc = spawn_tokenize_cmdargs(name, argv, argv_len);
    char amp = '&';
    if(strncmp(argv[argc-1], &amp, 1) != 0){
        argv[argc] = NULL;
        new_elm->background = false;
        stack_push(new_elm);
    } else {
        argv[argc-1] = NULL;
        new_elm->background = true;
        stack_insert_bottom(new_elm);
    }
    
    for(uint32_t i = 0; i < 30; i++){
        new_elm->name[i] = name[i];
        if(name[i] == '\0'){
            break;
        }
    }

    char *envp[1];
    envp[0] = NULL; // FIXME pass parent environment
    
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

    // concat name with path
    const char *path = "armv7/sbin/"; // size 11
    char concat_name[strlen(name) + 11];

    memcpy(concat_name, path, strlen(path));
    memcpy(&concat_name[strlen(path)], name, strlen(name)+1);
    
    struct mem_region *mr = multiboot_find_module(bi, concat_name);
    
    if (mr == NULL){
        // FIXME convert this to user space printing
        debug_printf("Could not spawn '%s': Program does not exist.\n", name);
        free(stack_pop());
        return SYS_ERR_OK;
    }
    
    struct spawninfo si;
    si.domain_id = ++pid_counter;
    *pid = si.domain_id;
    // struct process *p = register_process(&si, name);
    // void *x = register_process;
    // x=x;
    new_elm->pid = *pid;

    err = spawn_load_with_args(&si, mr, concat_name, disp_get_core_id(),
            argv, envp);

    if (err_is_fail(err)) {
        debug_printf("Failed spawn image: %s\n", err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

    // Copy Init's EP to Spawned Process' CSpace
    struct capref cap_dest;
    cap_dest.cnode = si.taskcn;
    cap_dest.slot = TASKCN_SLOT_INITEP;
    err = cap_copy(cap_dest, cap_initep);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy init ep to new process' cspace\n");
        return err;
    }

    // Copy Parent Process's EP to Spawned Process' CSpace
    assert(!capref_is_null(cap_selfep));
    cap_dest.cnode = si.taskcn;
    cap_dest.slot = TASKCN_SLOT_REMEP;
    err = cap_copy(cap_dest, cap_selfep);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy rem ep to new process' cspace\n");
        return err;
    }

    //debug_printf("calling spawn_run\n");
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

static size_t get_pids_count(void) 
{
    struct ps_stack *temp = ps_stack;
    size_t n = 0;
    while (temp != NULL) {
        temp = temp->next;
        n++;
    }
    return n;
}

static errval_t get_pid_at_index(uint32_t idx, domainid_t *pid)
{
    assert(ps_stack);
    struct ps_stack *current = ps_stack;

    for (uint32_t j = 0; j < idx; j++) {
        current = current->next;
        assert(current != NULL);
    }
    
    *pid = current->pid;
    return SYS_ERR_OK;
}

static errval_t reply_string(struct lmp_chan *lc, const char *string)
{
    size_t slen = strlen(string) + 1; // adjust for null-character
    size_t rlen = 0;
    char buf[8];
    errval_t err;
    
    while (rlen < slen) {
        size_t chunk_size = ((slen-rlen) < 8) ? (slen-rlen) : 8;
        memcpy(buf, string, chunk_size);
        err = lmp_chan_send(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                            9, SEND_TEXT, buf[0], buf[1], buf[2],
                            buf[3], buf[4], buf[5], buf[6], buf[7]);

        if (err_is_fail(err)) {
            return err;
        } 

        string = &(string[8]);
        rlen += 8;

    }

    return SYS_ERR_OK;
}

static const char *get_pid_name(domainid_t pid) 
{
    struct ps_stack *temp = ps_stack;
    while (temp != NULL) {
        if (pid == temp->pid) {
            return temp->name;
        }
        temp = temp->next;
    }
    return NULL;
}

// static struct process *register_process(struct spawninfo *si, const char *name)
// {
//     struct process *temp = fst_process;
//     while (temp->next != NULL) {
//         temp = temp->next;
//     }
//     temp->next = (struct process *) malloc(sizeof(struct process));
//     temp->next->pid = si->domain_id;
//     // temp->next->name = (char *)malloc(strlen(name));
//     for(uint32_t i = 0; i < 30; i++){
//         temp->next->name[i] = name[i];
//         if(name[i] == '\0'){
//             break;
//         }
//     }
//     temp->next->next = NULL;
//     return temp;
// }

int main(int argc, char *argv[])
{
    errval_t err;

    /* Set the core id in the disp_priv struct */
    err = invoke_kernel_get_core_id(cap_kernel, &my_core_id);
    assert(err_is_ok(err));
    disp_set_core_id(my_core_id);

    debug_printf("init: invoked as:");
    for (int i = 0; i < argc; i++) {
       debug_printf(" %s", argv[i]);
    }
    debug_printf("\n");

    debug_printf("FIRSTEP_OFFSET should be %zu + %zu\n",
            get_dispatcher_size(), offsetof(struct lmp_endpoint, k));

    // First argument contains the bootinfo location
    bi = (struct bootinfo*)strtol(argv[1], NULL, 10);

    // setup memory serving
    err = initialize_ram_alloc();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init local ram allocator");
        abort();
    }
    debug_printf("initialized local ram alloc\n");
    
    // setup memory serving
    err = initialize_mem_serv();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init memory server module");
        abort();
    }
    
    // int *int_buf = (int *)malloc(1000);
    // for (uint32_t i = 0; i < 1000; i++) {
    //     int_buf[i] = 1;
    // }
    debug_printf("Mem stuff done\n");
    
    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    debug_printf("Initialized dev memory management\n");

    if (my_core_id == 0) {
        // boot second core
        struct capref urpc_frame;
        size_t retsize;

        err = frame_alloc(&urpc_frame, MON_URPC_SIZE, &retsize);
        if (err_is_fail(err) || retsize != MON_URPC_SIZE) {
            DEBUG_ERR(err, "fail to allocate & create urpc frame\n");
            abort();
        }

        char *buf;
        err = paging_map_frame_attr(get_current_paging_state(), 
                                    (void **) &buf,
                                    MON_URPC_SIZE, 
                                    urpc_frame,
                                    VREGION_FLAGS_READ_WRITE_NOCACHE, 
                                    NULL, NULL);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "fail to map urpc frames\n");
            abort();
        }
#ifdef INIT_COMM_TEST
        // TESTING COMMUNICATION
        debug_printf("mapped ump frame in init. addr: 0x%08x\n", buf);
        memset((void *) buf, 0, MON_URPC_SIZE);
        const char *test_str = "hello";
        strncpy((char *) buf, test_str, 5);
        debug_printf("TEST STRING: %s\n", buf);
#endif
        struct frame_identity urpc_frame_id; 
        err = invoke_frame_identify(urpc_frame, &urpc_frame_id);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "fail to get urpc frame identity\n");
            abort();
        }
        debug_printf("URPC Physical Addr: 0x%08x\n", urpc_frame_id.base);
        debug_printf("URPC Virtual Addr: 0x%08x\n", buf);

        urpc_init((uintptr_t) buf, 1);

        err = spawn_core_load_kernel(bi, 1, 1, "", urpc_frame_id, cap_kernel);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "fail to boot 2nd core and load kernel\n");
            abort();
        }
        debug_printf("Started application processor\n");
    } else {
        assert(!capref_is_null(cap_urpcframe));
        char *buf;
        err = paging_map_frame_attr(get_current_paging_state(), 
                                    (void **) &buf,
                                    MON_URPC_SIZE, 
                                    cap_urpcframe,
                                    VREGION_FLAGS_READ_WRITE_NOCACHE, 
                                    NULL, NULL);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "fail to map urpc frames\n");
            abort();
        }

        urpc_init((uintptr_t) buf, 0);

#ifdef INIT_COMM_TEST
        struct frame_identity urpc_frame_id; 
        err = invoke_frame_identify(cap_urpcframe, &urpc_frame_id);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "fail to get urpc frame identity\n");
            abort();
        }
        // TESTING COMMUNICATION
        debug_printf("URPC Physical Addr: 0x%08x\n", urpc_frame_id.base);
        debug_printf("we got the test string: %s\n", buf);
        debug_printf("mapped ump frame in init\n");
#endif
    }

    // Create our endpoint to self
    err = cap_retype(cap_selfep, cap_dispatcher, ObjType_EndPoint, 0);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to create our endpoint to self\n");
        abort();
    }

    err = cap_mint(cap_initep, cap_selfep, FIRSTEP_OFFSET, FIRSTEP_BUFLEN);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to mint cap_selfep to cap_initep\n");
        abort();
    }
        
    struct waitset *ws = get_default_waitset();
    waitset_init(ws);
    
    struct lmp_chan lc;
    lmp_chan_init(&lc);
  
    /* make local endpoint available -- this was minted in the kernel in a way
     * such that the buffer is directly after the dispatcher struct and the
     * buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel 
     * sentinel word).
     */
    
    // client_stack = (struct ps_state *) malloc(sizeof(struct ps_state));
    // fst_process = (struct process *) malloc(sizeof(struct process));

    // Register INIT as first process!!!
    
    ps_stack = (struct ps_stack *)malloc(sizeof(struct ps_stack));
    ps_stack->pid = disp_get_domain_id();
    ps_stack->name[0] = 'i';
    ps_stack->name[1] = 'n';
    ps_stack->name[2] = 'i';
    ps_stack->name[3] = 't';
    ps_stack->name[4] = '\0';

    ps_stack->next = NULL;
    
    struct lmp_endpoint *my_ep;
    lmp_endpoint_setup(0, FIRSTEP_BUFLEN, &my_ep);
    
    lc.endpoint = my_ep;
    lc.local_cap = cap_selfep;
    
    // allocate slot for incoming capability from memeatermultiboot_find_module(bi, name);
    err = lmp_chan_alloc_recv_slot(&lc);
    if (err_is_fail(err)){
        printf("Could not allocate receive slot!\n");
        exit(-1);
    }

    // register receive handler 
    err = lmp_chan_register_recv(&lc, ws, MKCLOSURE(recv_handler, &lc));
    if (err_is_fail(err)){
        printf("Could not register receive handler!\n");
        exit(-1);
    }
    

    debug_printf("cap_initep, cap_selfep OK.\n");

    size_t offset = OMAP44XX_MAP_L4_PER_UART3 - 0x40000000;
    // lvaddr_t uart_addr = (1UL << 28);
    lvaddr_t uart_addr;
    paging_alloc(get_current_paging_state(), (void**)&uart_addr,
        OMAP44XX_MAP_L4_PER_UART3_SIZE);
    struct capref copy;
    err = devframe_type(&copy, cap_io, 30);
    if (err_is_fail(err)){
        debug_printf("Could not copy capref: %s\n", err_getstring(err));
        err_print_calltrace(err);
        abort();
    }
    debug_printf("UART ADDRESS: 0x%08x\n", uart_addr);
    err = paging_map_user_device(get_current_paging_state(), uart_addr,
                        copy, offset, OMAP44XX_MAP_L4_PER_UART3_SIZE,
                        VREGION_FLAGS_READ_WRITE_NOCACHE);


    if (err_is_fail(err)) {
        debug_printf("Could not map io cap: %s\n", err_getstring(err));
        err_print_calltrace(err);
        abort();
    } else {
        debug_printf("cap_io mapped OK.\n");
    }

    set_uart3_registers(uart_addr);
    
    struct thread *urpc_poll_thread = thread_create((thread_func_t) urpc_poll, 
                                                    NULL);
    err = thread_detach(urpc_poll_thread);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to detach URPC polling thread\n");
        abort();
    }

    if (my_core_id == 0) {
        debug_printf("Spawning shell...\n");
        domainid_t shell_pid;
        err = spawn("shell", &shell_pid, disp_get_core_id());
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "failed to spawn memeater\n");
        }
        debug_printf("init domain_id: %d\n", disp_get_domain_id());
    } else {
        debug_printf("init on core[%d] waiting for further instructions\n", 
            my_core_id);
    }
    debug_printf("Entering dispatch loop\n");
    while(true) {
        event_dispatch(get_default_waitset());
    }

    return EXIT_SUCCESS;
}
