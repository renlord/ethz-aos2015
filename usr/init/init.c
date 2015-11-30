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
static lvaddr_t spawnd_bi_addr;

// static struct process *fst_process;
static struct ps_stack *ps_stack;
static struct proc_state *fst_state;
static struct proc_state spawnd_state;
    
static volatile uint32_t *uart3_thr = NULL;
static volatile uint32_t *uart3_rhr = NULL;
static volatile uint32_t *uart3_fcr = NULL;
static volatile uint32_t *uart3_lsr = NULL;

static errval_t reply_string(struct lmp_chan *lc, const char *string);
// static struct process *register_process(struct spawninfo *si, const char *name);

struct bootinfo *bi;
struct capref cap_spawndep;

struct foreground_proc { 
    struct ps_entry *cur_ps;
    struct proc_state *cur_cs;
} foreground;

enum state_type {
    ACTIVE,
    WAITING,
    BACKGROUND,
};

struct proc_state {
    struct proc_state *next;
    struct lmp_chan lc;
    char mailbox[500];
    size_t char_count;
    uint32_t send_msg[9];
    struct capref send_cap;
    enum state_type type;
    bool pending_request;
};

struct ps_stack { //propose rename to foreground stack;
    struct ps_stack *next;
    struct proc_state *state;
    // struct process *process;
    bool background;
    bool pending_request;
    char name[30];
    domainid_t pid;
};

// struct process {
//     domainid_t pid;
//     char name[30];
//     struct process *next;
// };

struct serial_write_lock {
    bool lock;
    struct proc_state *cli;
};

struct serial_read_lock {
    bool lock;
    struct proc_state *cli;
};

// static struct proc_state spawnd_cs; //ONLY 1 `spawnd` service may be spawned.

static void stack_push(struct ps_stack *top);
static void stack_push(struct ps_stack *top)
{
    top->next = ps_stack;
    ps_stack = top;
    return;
}

static void stack_insert_bottom(struct ps_stack *bottom);
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

static struct ps_stack *stack_pop(void);
static struct ps_stack *stack_pop(void)
{
    struct ps_stack *top = ps_stack;
    ps_stack = top->next;
    return top;
}

static void debug_print_stack(void);
static void debug_print_stack(void)
{
    debug_printf("PROCESS STACK:\n");
    struct ps_stack *cur = ps_stack;
    while(cur != NULL){
        debug_printf("%s\n", cur->name);
        cur = cur->next;
    }
    debug_printf("DONE\n");
}

// static void stack_remove_state(struct proc_state *proc);
// static void stack_remove_state(struct proc_state *proc)
// {
//     if(proc == ps_stack->state){
//         stack_pop();
//         return;
//     }
//
//     struct ps_stack *elm = ps_stack;
//     while(elm->next != NULL && elm->next->state != proc){
//         elm = elm->next;
//     }
//
//     if(elm->next->state == proc){
//         elm->next = elm->next->next;
//     }
//
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

errval_t serial_put_char(const char *c) 
{
    //debug_printf("serial put char ----> %c\n\n", *c);
    assert(uart3_lsr && uart3_thr);
    while(*uart3_lsr == 0x20);
    *uart3_thr = *c;
    
    return SYS_ERR_OK;
}

errval_t serial_get_char(char *c) 
{
    *uart3_fcr &= ~1; // write 0 to bit 0
    *uart3_rhr = 0;
    while((*uart3_lsr & 1) == 0);
    *c = (char)*uart3_rhr;
    return SYS_ERR_OK;
}

void send_handler(void *proc_in)
{
    struct proc_state *proc = (struct proc_state*)proc_in;
    
    uint32_t buf[9];
    for (uint8_t i = 0; i < 9; i++){
        buf[i] = proc->send_msg[i];
        if((char*)buf[i] == '\0'){
            break;
        }
    }
    
    struct capref cap = proc->send_cap;    
    
    errval_t err = lmp_chan_send9(&proc->lc, LMP_SEND_FLAGS_DEFAULT, cap, buf[0],
                                  buf[1], buf[2], buf[3], buf[4],
                                  buf[5], buf[6], buf[7], buf[8]);
 
    if (err_is_fail(err)){ // TODO check that err is indeed endbuffer full
        struct event_closure closure = MKCLOSURE(send_handler, proc_in);
        err = lmp_chan_register_send(&proc->lc, get_default_waitset(),
                                     closure);
        if (err_is_fail(err)) {
            debug_printf("Could not re-register for sending\n");
            return;
        } else {
            debug_printf("could not send msg, trying again\n");            
        }
    }
}

static void spawnd_recv_handler(void *lc_in)
{
    assert(lc_in == &spawnd_state.lc);
    
    struct capref remote_cap = NULL_CAP;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    errval_t err = lmp_chan_recv(&spawnd_state.lc, &msg, &remote_cap);
    
    if (err_is_fail(err)) {
        debug_printf("Could not receive msg from spawnd: %s\n",
            err_getstring(err));
        err_print_calltrace(err);
        spawnd_state.lc.remote_cap = NULL_CAP;
        return;
    }

    if (msg.buf.msglen == 0){
        debug_printf("Bad msg from spawnd.\n");
        spawnd_state.lc.remote_cap = NULL_CAP;
        return;
    }
    
    lmp_chan_register_recv(&spawnd_state.lc, get_default_waitset(),
        MKCLOSURE(spawnd_recv_handler, lc_in));
    
    // Re-allocate
    if (!capref_is_null(remote_cap)){
        err = lmp_chan_alloc_recv_slot(&spawnd_state.lc);
        if (err_is_fail(err)){
            debug_printf("Could not allocate receive slot: %s.\n",
                err_getstring(err));
            err_print_calltrace(err);
            return;
        }
    }
    
    uint32_t code = msg.buf.words[0];
    
    switch(code){
        case REGISTER_CHANNEL:
        {
            debug_printf("received ep cap from spawnd.\n");
            spawnd_state.lc.remote_cap = remote_cap;
            
            break;
        }
        case SPAWND_READY:
        {
            debug_printf("received ready signal from spawnd.\n");

            err = lmp_chan_send2(&spawnd_state.lc, LMP_SEND_FLAGS_DEFAULT,
                                 NULL_CAP, SPAWND_READY, spawnd_bi_addr);
            
            if(err_is_fail(err)){
                debug_printf("Could not send msg to spawnd: %s\n",
                    err_getstring(err));
                abort();
            }
            
            reply_string(&spawnd_state.lc, "shell");
            lmp_chan_send1(&spawnd_state.lc, LMP_SEND_FLAGS_DEFAULT,
                           NULL_CAP, PROCESS_SPAWN);
            if(err_is_fail(err)){
                debug_printf("Could not send msg to spawnd: %s\n",
                    err_getstring(err));
                abort();
            }
            break;
        }
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
            spawnd_state.send_msg[0] = REQUEST_FRAME_CAP;
            spawnd_state.send_msg[1] = log2ceil(ret_bytes);
            spawnd_state.send_cap = dest;
            
            err = lmp_chan_register_send(&spawnd_state.lc, get_default_waitset(),
                                         MKCLOSURE(send_handler, &spawnd_state));
            if (err_is_fail(err)) {
                err_print_calltrace(err);
                abort();
            }
            
            break;
        }
        
        default:{}
    }
    
    
    return;
}


void recv_handler(void *lc_in)
{
    assert(fst_state != NULL);
    struct capref remote_cap = NULL_CAP;
    struct lmp_chan *lc = (struct lmp_chan *)lc_in;
    
    // Find the state for the requesting client
    struct proc_state *proc = fst_state;
    while (proc != NULL && &proc->lc != lc){
         proc = proc->next;
    }
    
    // If we haven't seen the process before, it should be requesting a channel
    if (proc == NULL){
        proc = fst_state;
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
    // FIXME perhaps split up for spawnd/not spawnd
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
    
    uint32_t code = msg.buf.words[0];
    
    assert(proc != NULL || code == REGISTER_CHANNEL);
    
    bool postpone = false;
        
    // if (proc != NULL){
    //     assert(elm != NULL);
    //
    //     // If process is not on top of stack and not running
    //     // in background, we postpone
    //     postpone = (elm != ps_stack && !elm->background);
    //
    //     // Preparation
    //     for (uint32_t i = 1; i < LMP_MSG_LENGTH; i++){
    //         proc->send_msg[i] = 0;
    //     }
    //     proc->send_cap = NULL_CAP;
    // }
    
    if (proc != NULL) {
        proc->send_msg[0] = code;
    }
    
    bool reply = false;

    switch(code) {
        
        // Initial request from new clients, causes init to establish
        // a new channel for future communication
        case REGISTER_CHANNEL:
        {
            debug_printf("REGISTER_CHANNEL not handled!\n");
            break;
            
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
            debug_printf("REGISTER_CHANNEL clause 2\n");
            
            // bootstrap new LMP channel
            proc->lc.local_cap = ep_cap;
            proc->lc.remote_cap = remote_cap;
            proc->lc.endpoint = my_ep;

            debug_printf("REGISTER_CHANNEL clause 3\n");
            err = lmp_chan_alloc_recv_slot(&proc->lc);
            if (err_is_fail(err)) {
                debug_printf("Could not allocate receive slot!\n");
                err_print_calltrace(err);
                return;
            }

            debug_printf("REGISTER_CHANNEL clause 4\n");
            // Register receive handler to this channel
            err = lmp_chan_register_recv(&proc->lc, get_default_waitset(),
                MKCLOSURE(recv_handler, &proc->lc));
            if (err_is_fail(err)) {
                debug_printf("Could not register receive handler\n");
                err_print_calltrace(err);
                return;
            }
            
            // initialize process state
            debug_printf("REGISTER_CHANNEL clause 5\n");
            proc = fst_state;

            // Send new endpoint cap back to client
            debug_printf("REGISTER_CHANNEL clause 7\n");
            proc->send_msg[0] = REGISTER_CHANNEL;
            proc->send_cap = ep_cap;
            reply = true;

            // Allocate receive struct right away for next msg
            debug_printf("REGISTER_CHANNEL clause 8\n");
            err = lmp_chan_alloc_recv_slot(&proc->lc);
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

            // if (strncmp(proc->mailbox, "bye", 3) == 0) {
            //     bool is_top = (elm == ps_stack);
            //     // void *x = deregister_process;
            //     // x=x;
            //     // stack_remove_state(proc);
            //     if(is_top){
            //         // debug_print_stack();
            //         // deregister_process(elm->process);
            //         stack_pop();
            //         // debug_print_stack();
            //         // if(ps_stack->pending_request){
            //             debug_printf("Next process should run now\n");
            //             elm = ps_stack;
            //             proc = elm->state;
            //             reply = true;
            //         // } else {
            //         //     debug_printf("Not top\n");
            //         //     return;
            //         // }
            //     }
            // }
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
            // // 1. client sends name of application to proc->mailbox via
            // // SEND_TEXT.
            //
            // // 2. client indicates that name transfer is complete, we check
            // // mailbox for the name of the application.
            // char *app_name = proc->mailbox;
            // bool background = msg.buf.words[1];
            // //debug_printf("received app name: %s\n", app_name);
            //
            // // sanity checks
            // assert(app_name != NULL);
            // assert(strlen(app_name) > 1);
            //
            // // tell spawnd to spawn application
            // err = reply_string(spawnd_cs.lc, app_name);
            // if (err_is_fail(err)) {
            //     DEBUG_ERR(err, "failed to send app name to spawnd\n");
            //     abort();
            // }
            //
            // err = lmp_chan_send2(spawnd_cs.lc, LMP_SEND_FLAGS_DEFAULT,
            //                      NULL_CAP, SPAWND_PROCESS_SPAWN, background);
            // if (err_is_fail(err)) {
            //     DEBUG_ERR(err, "failed to send app statuses to spawnd\n");
            //     abort();
            // }
            // // reset mailbox
            // memset(proc->mailbox, '\0', 500);
            // proc->char_count = 0;
            break;
        }

        // case SPAWND_PROCESS_SPAWN:
        // {
        //     // we always assume foreground shell is the one that spawned child
        //     // processes.
        //     foreground.cur_cs->send_msg[0] = PROCESS_SPAWN;
        //     foreground.cur_cs->send_cap = NULL_CAP;
        //     reply = true;
        //     if (err_is_fail(err)) {
        //         foreground.cur_cs->send_msg[1] = false;
        //         foreground.cur_cs->send_msg[2] = '\0';
        //         DEBUG_ERR(err, "failed to spawn process from SHELL.\n");
        //     } else {
        //         foreground.cur_cs->send_msg[1] = true;
        //         foreground.cur_cs->send_msg[2] = msg.buf.words[1];
        //         foreground.cur_cs->send_msg[3] = '\0';
        //     }
        //     break;
        // }

        case PROCESS_GET_NAME: 
        {
            // domainid_t pid = (domainid_t) msg.buf.words[1];
            // err = lmp_chan_send2(spawnd_cs.lc, LMP_SEND_FLAGS_DEFAULT,
            //                      NULL_CAP, SPAWND_GET_NAME, pid);
            // if (err_is_fail(err)) {
            //     DEBUG_ERR(err, "failed to send get name req. to spawnd\n");
            //     abort();
            // }
            //
            break;
        }

        // case SPAWND_GET_NAME:
        // {
        //     const char *name = spawnd_cs.mailbox;
        //     memset(spawnd_cs.mailbox, '\0', 500);
        //
        //     err = reply_string(foreground.cur_cs->lc, name);
        //     if (err_is_fail(err)) {
        //         DEBUG_ERR(err, "failed to reply PROCESS_GET_NAME event.\n");
        //         err_print_calltrace(err);
        //         return;
        //     }
        //
        //     err = lmp_chan_send1(foreground.cur_cs->lc, LMP_SEND_FLAGS_DEFAULT,
        //                          NULL_CAP, PROCESS_GET_NAME);
        //     if (err_is_fail(err)) {
        //         DEBUG_ERR(err, "failed to reply PROCESS_GET_NAME event.\n");
        //         err_print_calltrace(err);
        //         abort();
        //     }
        //
        //     break;
        // }

        case PROCESS_GET_NO_OF_PIDS:
        {
            // err = lmp_chan_send1(spawnd_cs.lc, LMP_SEND_FLAGS_DEFAULT,
            //                      NULL_CAP, SPAWND_GET_NO_OF_PIDS);
            // if (err_is_fail(err)) {
            //     DEBUG_ERR(err, "failed to send get pid nos. req.\n");
            // }
            break;
        }

        // case SPAWND_GET_NO_OF_PIDS:
        // {
        //     reply = true;
        //     foreground.cur_cs->send_msg[0] = PROCESS_GET_NO_OF_PIDS;
        //     foreground.cur_cs->send_msg[1] = msg.buf.words[1];
        //     foreground.cur_cs->send_msg[2] = '\0';
        //     foreground.cur_cs->send_cap = NULL_CAP;
        //     break;
        // }
        
        case PROCESS_GET_PID:
        {
            // uint32_t idx = msg.buf.words[1];
            //
            // err = lmp_chan_send2(foreground.cur_cs->lc, LMP_SEND_FLAGS_DEFAULT,
            //                      NULL_CAP, SPAWND_GET_PID, idx);
            // if (err_is_fail(err)) {
            //     DEBUG_ERR(err, "failed to send SPAWND_GET_PID event.\n");
            //     err_print_calltrace(err);
            //     abort();
            // }
            //
            break;
        }

        // case SPAWND_GET_PID:
        // {
        //     reply = true;
        //     foreground.cur_cs->send_msg[0] = PROCESS_GET_PID;
        //     foreground.cur_cs->send_msg[1] = msg.buf.words[1];
        //     foreground.cur_cs->send_msg[2] = '\0';
        //     foreground.cur_cs->send_cap = NULL_CAP;
        //
        //     break;
        // }
        
        case REQUEST_SPAWND_EP:
        {
            debug_printf("sent spawnd_ep");
            reply = true;
            proc->send_msg[0] = REQUEST_SPAWND_EP;
            proc->send_cap = cap_spawndep;
        }
        default:
        {
            // include LIB_ERR_NOT_IMPLEMENTED? 
            debug_printf("Wrong code: %d\n", code);
        }
    }
    
    if(reply){
        if (postpone){
            proc->pending_request = true;
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
        proc->pending_request = false;
    }
}

void set_uart3_registers(lvaddr_t base)
{
    uart3_thr = (uint32_t *)((uint32_t)base + 0x0000);
    uart3_rhr = (uint32_t *)((uint32_t)base + 0x0000);
    uart3_fcr = (uint32_t *)((uint32_t)base + 0x0008);
    uart3_lsr = (uint32_t *)((uint32_t)base + 0x0014);
}

void my_print(const char *buf)
{
    assert(uart3_lsr && uart3_thr);
    for (uint32_t i = 0; i < strlen(buf); i++){
        while(*uart3_lsr == 0x20);
        // FIXME why not this? --> while((*uart3_lsr & 0x20) != 0); 
        *uart3_thr = buf[i];
    }
}

void my_read(void)
{
    char buf[256];
    memset(buf, '\0', 256);

    size_t i = 0;

    while (true) {
        *uart3_fcr &= ~1; // write 0 to bit 0
        // *uart3_fcr |= 2; // write 1 to bit 1
        *((uint8_t *)uart3_rhr) = 0;
        while ((*uart3_lsr & 1) == 0);
        my_print((char *)uart3_rhr);
        memcpy(&buf[i++], (char *) uart3_rhr, 1);
        if (*uart3_rhr == 13) { // ENTER keystroke.
            debug_printf("cur buf: %s\n", buf);
            printf("\n");
            i = 0;
        }
    }
}

errval_t spawn_spawnd(void)
{
    assert(bi != NULL);
    errval_t err;
    
    spawnd_state.next = NULL; // FIXME ??
    spawnd_state.type = BACKGROUND;
        
    // Parse cmd line args
    debug_printf("setting argv[0]\n");
    char *argv[2];
    argv[0] = malloc(7);
    sprintf(argv[0], "spawnd");

    debug_printf("setting argv[1]\n");
    argv[1] = NULL;

    char *envp[1];
    envp[0] = NULL; // FIXME pass parent environment
    
    struct mem_region *mr = multiboot_find_module(bi, "armv7/sbin/spawnd");
    
    if (mr == NULL){
        // FIXME convert this to user space printing
        debug_printf("Could not find spawnd image.\n");
        abort();
    }
    
    struct spawninfo si;
    si.domain_id = 1;

    err = spawn_load_with_args(&si, mr, "armv7/sbin/spawnd", disp_get_core_id(),
            argv, envp);

    if (err_is_fail(err)) {
        debug_printf("Failed spawn spawnd: %s\n", err_getstring(err));
        err_print_calltrace(err);
        abort();
    } else {
        debug_printf("Spawn succesful.\n");
    }
    
    
    // Copy Parent Process's EP to Spawned Process' CSpace
    assert(!capref_is_null(cap_selfep)); // TODO change to parents
    struct capref cap_parent_dest = {
        .cnode = si.taskcn,
        .slot = TASKCN_SLOT_PARENTEP
    };
    err = cap_copy(cap_parent_dest, cap_selfep);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy rem ep to new process' cspace\n");
        return err;
    }
    
    struct capref spawnd_selfep = {
        .cnode = si.taskcn,
        .slot = TASKCN_SLOT_SELFEP
    };
    
    // Setup connection init->spawnd
    aos_setup_channel(&spawnd_state.lc, spawnd_selfep,
        MKCLOSURE(spawnd_recv_handler, &spawnd_state.lc));
    
    // Copy Init's EP to Spawned Process' CSpace
    struct capref spawnd_initep = {
        .cnode = si.taskcn,
        .slot = TASKCN_SLOT_INITEP
    };
    
    err = cap_copy(spawnd_initep, spawnd_state.lc.local_cap);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy init ep to new process' cspace\n");
        return err;
    }
    
    // Calculate bootinfo size
    size_t bi_size = sizeof(struct bootinfo) + sizeof(struct mem_region)*bi->regions_length;
    debug_printf("bi_size: %d, regions: %d\n", bi_size, bi->regions_length);

    // Allocate frame for new bootinfo in own cspace
    // (FIXME we could skip this if we new how to get a hold of
    //        the frame cap for the bootinfo at adress bi)
    debug_printf("Allocating frame for bootinfo\n");
    struct capref bi_cap;
    size_t retbytes;
    err = frame_alloc(&bi_cap, bi_size, &retbytes);
    if(err_is_fail(err) || bi_size > retbytes){
        debug_printf("Could not allocate bootinfo for spawnd.\n");
        abort();
    }
    
    // Allocate virtual memory in own vspace
    debug_printf("Allocating vspace for bootinfo\n");
    lvaddr_t my_bi_addr;
    err = paging_map_frame_attr(get_current_paging_state(), (void**)&my_bi_addr,
        bi_size, bi_cap, VREGION_FLAGS_READ_WRITE_NOCACHE, NULL, NULL);

    debug_printf("Copying bootinfo\n");    
    
    // Map the frame cap in spawnd's memory space
    debug_printf("Mapping frame in spawnd's vmem\n");    
    err = paging_map_frame_attr(si.vspace, (void**)&spawnd_bi_addr,
        bi_size, bi_cap, VREGION_FLAGS_READ_WRITE_NOCACHE, NULL, NULL);
    if(err_is_fail(err)){
        debug_printf("Could not map bootinfo frame\n");
        abort();
    }
    debug_printf("bootinfo mapped at addr 0x%08x\n", spawnd_bi_addr);

    // Copy bootinfo data to new frame
    memcpy((void *)my_bi_addr, bi, bi_size);
    struct bootinfo *new_bi = (struct bootinfo *)my_bi_addr;
    debug_printf("new_bi->regions_length: %d\n", new_bi->regions_length);
    
    for(uint32_t i = 0; i < 160; i+=4){
        debug_printf("%d: 0x%08x\n", i, *((uint32_t *)my_bi_addr+i));
    }

    // For region in region-length copy from inits module cnode to spawnd's module cnode
    // Run image
    err = spawn_run(&si);
    if (err_is_fail(err)) {
        debug_printf("Failed spawn image: %s\n", err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

    // De-allocate memory
    err = spawn_free(&si);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to free memeater domain from init memory\n");
        return err;
    }
    
    return SYS_ERR_OK;    
}

// static errval_t spawn(char *name, domainid_t *pid)
// {
//     errval_t err;
//
//     struct ps_stack *new_elm =
//         (struct ps_stack *) malloc(sizeof(struct ps_stack));
//     new_elm->next = NULL;
//     new_elm->pending_request = false;
//     new_elm->state = NULL;
//
//     // Parse cmd line args
//     const uint32_t argv_len = 20;
//     char *argv[argv_len];
//     uint32_t argc = spawn_tokenize_cmdargs(name, argv, argv_len);
//     char amp = '&';
//     if(strncmp(argv[argc-1], &amp, 1) != 0){
//         argv[argc] = NULL;
//         new_elm->background = false;
//         stack_push(new_elm);
//     } else {
//         argv[argc-1] = NULL;
//         new_elm->background = true;
//         stack_insert_bottom(new_elm);
//     }
//
//     for(uint32_t i = 0; i < 30; i++){
//         new_elm->name[i] = name[i];
//         if(name[i] == '\0'){
//             break;
//         }
//     }
//
//     char *envp[1];
//     envp[0] = NULL; // FIXME pass parent environment
//
//     // concat name with path
//     const char *path = "armv7/sbin/"; // size 11
//     char concat_name[strlen(name) + 11];
//
//     memcpy(concat_name, path, strlen(path));
//     memcpy(&concat_name[strlen(path)], name, strlen(name)+1);
//
//     struct mem_region *mr = multiboot_find_module(bi, concat_name);
//
//     if (mr == NULL){
//         // FIXME convert this to user space printing
//         debug_printf("Could not spawn '%s': Program does not exist.\n", name);
//         free(stack_pop());
//         return SYS_ERR_OK;
//     }
//
//     struct spawninfo si;
//     si.domain_id = ++pid_counter;
//     *pid = si.domain_id;
//     new_elm->pid = *pid;
//
//     err = spawn_load_with_args(&si, mr, concat_name, disp_get_core_id(),
//             argv, envp);
//
//     if (err_is_fail(err)) {
//         debug_printf("Failed spawn image: %s\n", err_getstring(err));
//         err_print_calltrace(err);
//         return err;
//     }
//
//     *pid = si.domain_id;
//     //register_process(&si, name);
//
//     // Copy Init's EP to Spawned Process' CSpace
//     struct capref cap_dest;
//     cap_dest.cnode = si.taskcn;
//     cap_dest.slot = TASKCN_SLOT_INITEP;
//     err = cap_copy(cap_dest, cap_initep);
//     if (err_is_fail(err)) {
//         DEBUG_ERR(err, "failed to copy init ep to new process' cspace\n");
//         return err;
//     }
//
//     // Copy Parent Process's EP to Spawned Process' CSpace
//     assert(!capref_is_null(cap_selfep));
//     cap_dest.cnode = si.taskcn;
//     cap_dest.slot = TASKCN_SLOT_REMEP;
//     err = cap_copy(cap_dest, cap_selfep);
//     if (err_is_fail(err)) {
//         DEBUG_ERR(err, "failed to copy rem ep to new process' cspace\n");
//         return err;
//     }
//
//     //debug_printf("calling spawn_run\n");
//     err = spawn_run(&si);
//     if (err_is_fail(err)) {
//         debug_printf("Failed spawn image: %s\n", err_getstring(err));
//         err_print_calltrace(err);
//         return err;
//     }
//
//     err = spawn_free(&si);
//     if (err_is_fail(err)) {
//         DEBUG_ERR(err, "failed to free memeater domain from init memory\n");
//         return err;
//     }
//
//     return SYS_ERR_OK;
// }

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

int main(int argc, char *argv[])
{
    errval_t err;

    /* Set the core id in the disp_priv struct */
    err = invoke_kernel_get_core_id(cap_kernel, &my_core_id);
    assert(err_is_ok(err));
    disp_set_core_id(my_core_id);

    debug_printf("init: invoked as:");
    for (int i = 0; i < argc; i++) {
       printf(" %s", argv[i]);
    }
    printf("\n");

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
    
    int *int_buf = (int *)malloc(1000);
    for (uint32_t i = 0; i < 1000; i++) {
        int_buf[i] = 1;
    }
    debug_printf("Mem stuff done\n");
    
    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    debug_printf("Initialized dev memory management\n");

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
    
    // client_stack = (struct proc_state *) malloc(sizeof(struct proc_state));
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
    
    // struct lmp_endpoint *my_ep;
    // lmp_endpoint_setup(0, FIRSTEP_BUFLEN, &my_ep);
    //
    // lc.endpoint = my_ep;
    // lc.local_cap = cap_selfep;
    //
    // // allocate slot for incoming capability from memeatermultiboot_find_module(bi, name);
    // err = lmp_chan_alloc_recv_slot(&lc);
    // if (err_is_fail(err)){
    //     printf("Could not allocate receive slot!\n");
    //     exit(-1);
    // }
    //
    // // register receive handler
    // err = lmp_chan_register_recv(&lc, ws, MKCLOSURE(recv_handler, &lc));
    // if (err_is_fail(err)){
    //     printf("Could not register receive handler!\n");
    //     exit(-1);
    // }
    //
    // debug_printf("cap_initep, cap_selfep OK.\n");

    size_t offset = OMAP44XX_MAP_L4_PER_UART3 - 0x40000000;

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

    // Technically, we would just like to spawn a shell process and that's it.
    // TODO

    // PROCESS SPAWNING CODE 
    // TO BE MOVED TO DEDICATED program afterwards.

    debug_printf("Spawning spawnd\n");    
    err = spawn_spawnd();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to spawn spawnd\n");
        abort();
    }

    // err = reply_string(&spawnd_state.lc, "shell");
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "Could not send message to spawn.\n");
    //     abort();
    // }
    
    lmp_chan_send1(&spawnd_state.lc, LMP_SEND_FLAGS_DEFAULT,
                   NULL_CAP, PROCESS_SPAWN);
    // err = spawn("spawnd", &spawnd_pid);
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "failed to spawn spawnd\n");
    // } else {
    //     debug_printf("Done\n");
    // }
    //
    // debug_printf("spawning shell\n");
    //
    //
    // err = spawn("memeater", &memeater_pid);
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "failed to spawn memeater\n");
    // } else {
    //     debug_printf("Done\n");
    // }
    
    void *c = debug_print_stack;
    c = reply_string;
    c = get_pids_count;
    c = get_pid_at_index;
    c = get_pid_name;
    c = stack_push;
    c = stack_insert_bottom;
    c = stack_pop;
    c = &lc;
    c=c;

    debug_printf("Entering dispatch loop\n");
    while(true) {
        event_dispatch(get_default_waitset());
    }

    return EXIT_SUCCESS;
}
