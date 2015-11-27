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
#include <stdlib.h>
#include <string.h>
#include <barrelfish/morecore.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan.h>
#include <barrelfish/aos_rpc.h>
#include <barrelfish/sys_debug.h>
#include <barrelfish/sys_debug.h>
#include <omap44xx_map.h>


#define MAX_CLIENTS 50
#define HARD_LIMIT (1UL << 28)

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

static struct client_state *client_stack;
static struct process *fst_process;
    
static volatile uint32_t *uart3_thr = NULL;
static volatile uint32_t *uart3_rhr = NULL;
static volatile uint32_t *uart3_fcr = NULL;
static volatile uint32_t *uart3_lsr = NULL;

static errval_t spawn(char *name, domainid_t *pid);
static errval_t get_pid_at_index(uint32_t idx, domainid_t *pid);
static const char *get_pid_name(domainid_t pid);
static size_t get_pids_count(void);
static errval_t reply_string(struct lmp_chan *lc, const char *string);
static void register_process(struct spawninfo *si, const char *name);

struct bootinfo *bi;

struct client_state {
    struct client_state *next;
    struct lmp_chan *lc;
    size_t alloced;
    char mailbox[500];
    size_t char_count;
    uint32_t send_msg[9];
    struct capref send_cap;
    struct event_closure next_event;
};


struct process {
    domainid_t pid;
    const char *name;
    struct process *next;
};

struct serial_write_lock {
    bool lock;
    struct client_state *cli;
};

struct serial_read_lock {
    bool lock;
    struct client_state *cli;
};

static void stack_push(struct client_state *cs);
static void stack_push(struct client_state *cs)
{
    cs->next = client_stack;
    client_stack = cs;
    return;
}

static struct client_state *stack_pop(void);
static struct client_state *stack_pop(void)
{
    struct client_state *cs = client_stack;
    client_stack = cs->next;
    return cs;
}

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

static void parse_cmd_args(char *str, char **argv, uint32_t *args);
static void parse_cmd_args(char *str, char **argv, uint32_t *args)
{
    char *tok; 
    int i;
    const char *delim = " ";

    tok = strtok(str, delim);
    
    if (tok != NULL) {
        argv[0] = tok; 
    }

    for (i = 1; (tok = strtok(NULL, delim)) != NULL; i++) {
        argv[i] = tok;
    }

    *args = i;
}


void send_handler(void *client_state_in)
{
    struct client_state *client_state = (struct client_state*)client_state_in;
    
    uint32_t buf[9];
    for (uint8_t i = 0; i < 9; i++){
        buf[i] = client_state->send_msg[i];
        if((char*)buf[i] == '\0'){
            break;
        }
    }
    
    struct capref cap = client_state->send_cap;    
    
    errval_t err = lmp_chan_send9(client_state->lc, LMP_SEND_FLAGS_DEFAULT, cap, buf[0],
                                  buf[1], buf[2], buf[3], buf[4],
                                  buf[5], buf[6], buf[7], buf[8]);
 
    if (err_is_fail(err)){ // TODO check that err is indeed endbuffer full
        struct event_closure closure = MKCLOSURE(send_handler, client_state_in);
        err = lmp_chan_register_send(client_state->lc, get_default_waitset(),
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
    struct capref remote_cap = NULL_CAP;
    struct lmp_chan *lc = (struct lmp_chan *)lc_in;
    
    // Find the state for the requesting client
    struct client_state *cs = client_stack;
    while (cs != NULL && cs->lc != lc){
        cs = cs->next;
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
    
    // Our protocol requires that there is a procedure code
    // at a bare minimum
    if (msg.buf.msglen == 0) {
        debug_printf("Bad msg for init.\n");
        return;
    }
    
    uint32_t code = msg.buf.words[0];

    assert(cs != NULL || code == REQUEST_CHAN);
    
    bool reply = false;
    
    switch(code) {
        
        // Initial request from new clients, causes init to establish
        // a new channel for future communication
        case REQUEST_CHAN:
        {
            
            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }
            
            
            cs = client_stack;
            
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

            // Insert in ps_entry
            assert(cs->lc == NULL);
            cs->lc = new_chan;

            // Send new endpoint cap back to client
            cs->send_msg[0] = REQUEST_CHAN;
            cs->send_msg[1] = '\0';
            cs->send_cap = ep_cap;
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
                 cs->mailbox[cs->char_count] = msg.buf.words[i];
                 cs->char_count++;
                 
                 if (msg.buf.words[i] == '\0') {
                    cs->char_count = 0;
                    
                    // TODO actually handle receiving a msg
                    break;
                }
            }
            
            if (strncmp(cs->mailbox, "ping", 4) == 0){

                cs->send_msg[0] = SEND_TEXT;
                for (uint32_t i = 0; i < 7; i++){
                    cs->send_msg[i+1] = (uint32_t)cs->mailbox[i];
                    if(cs->mailbox[i] == '\0'){
                        break;
                    }
                }
                
                reply = true;
                cs->send_msg[0] = SEND_TEXT;
                cs->send_msg[1] = (uint32_t)'p';
                cs->send_msg[2] = (uint32_t)'o';
                cs->send_msg[3] = (uint32_t)'n';
                cs->send_msg[4] = (uint32_t)'g';
                cs->send_msg[5] = (uint32_t)'\0';
                cs->send_cap = NULL_CAP;
            }
        }
        break;
        
        // Returns a frame capability to the client
        case REQUEST_FRAME_CAP:
        {

            size_t req_bits = msg.buf.words[1];
            size_t req_bytes = (1UL << req_bits);
            struct capref dest = NULL_CAP;
            
            if (cs->alloced + req_bytes >= HARD_LIMIT){
                debug_printf("Client request exceeds hard limit.\n");
                return;
            }
            
            // Perform the allocation
            size_t ret_bytes;
            err = frame_alloc(&dest, req_bytes, &ret_bytes);
            if (err_is_fail(err)){
                debug_printf("Failed memserv allocation.\n");
                err_print_calltrace(err);
            }
            
            cs->alloced += ret_bytes;
            
            
            // Send cap and return bits back to caller
            cs->send_msg[0] = REQUEST_FRAME_CAP;
            cs->send_msg[1] = log2ceil(ret_bytes);
            cs->send_msg[2] = '\0';
            cs->send_cap = dest;
            reply = true;
            
            break;
        }
        
        case REQUEST_DEV_CAP:
        {
            cs->send_msg[0] = REQUEST_DEV_CAP;
            cs->send_msg[1] = '\0';
            cs->send_cap = cap_io;
            reply = true;
            
            break;
        }

        case SERIAL_PUT_CHAR:
        {
            serial_put_char((char *) &msg.buf.words[1]);
            break;
        }

        case SERIAL_GET_CHAR:
        {
            char c;
            serial_get_char(&c);

            cs->send_msg[0] = SERIAL_GET_CHAR;
            cs->send_msg[1] = c;
            cs->send_msg[2] = '\0';
            cs->send_cap = NULL_CAP;
            reply = true;
            
            break;
        }

        case PROCESS_SPAWN:
        {
            // 1. client sends name of application to cs->mailbox via 
            // SEND_TEXT.

            // 2. client indicates that name transfer is complete, we check
            // mailbox for the name of the application.
            char *app_name = cs->mailbox;
            //debug_printf("received app name: %s\n", app_name);

            // sanity checks
            assert(app_name != NULL);
            assert(strlen(app_name) > 1);

            domainid_t pid;
            err = spawn(app_name, &pid);

            // reset mailbox
            memset(cs->mailbox, '\0', 500);
            cs->char_count = 0;

            // 3. we inform client if app spawn is successful or not.
            cs->send_msg[0] = PROCESS_SPAWN;
            cs->send_cap = NULL_CAP;
            reply = true;
            if (err_is_fail(err)) {
                cs->send_msg[1] = false;
                cs->send_msg[2] = '\0';
                DEBUG_ERR(err, "failed to spawn process from SHELL.\n");
            } else {
                cs->send_msg[1] = true;
                cs->send_msg[2] = pid;
                cs->send_msg[3] = '\0';
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
            cs->send_msg[0] = PROCESS_GET_NO_OF_PIDS;
            cs->send_msg[1] = get_pids_count();
            cs->send_msg[2] = '\0';
            cs->send_cap = NULL_CAP;

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
            cs->send_msg[0] = PROCESS_GET_PID;
            cs->send_msg[1] = pid;
            cs->send_msg[2] = '\0';
            cs->send_cap = NULL_CAP;

            break;
        }

        default:
        {
            // include LIB_ERR_NOT_IMPLEMENTED? 
            debug_printf("Wrong code: %d\n", code);
        }
    }
    
    
    if(reply){
        if(cs == client_stack){
            do {
                err = lmp_chan_register_send(lc_in, get_default_waitset(),
                                             MKCLOSURE(send_handler, cs));
                if (err_is_fail(err)) {
                    debug_printf("Could not register for sending\n");
                }
            } while(err_is_fail(err));
            
        } else {
            cs->next_event = MKCLOSURE(send_handler, cs);
        }
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

static errval_t spawn(char *name, domainid_t *pid)
{
    errval_t err;
    
    struct client_state *new_state =
        (struct client_state *) malloc(sizeof(struct client_state));
    new_state->next = NULL;
    new_state->lc = NULL;
    new_state->alloced = 0;
    new_state->char_count = 0;
    new_state->next_event = NOP_CLOSURE;
    stack_push(new_state);

    char *argv[20];
    uint32_t argc;
    parse_cmd_args(name, argv, &argc);
    if (argc >= 20){
        debug_printf("Too many spawn arguments.\n");
        abort();
    }
    
    argv[argc] = NULL;
    
    char *envp[20];
    envp[0] = NULL;
    
    
    // concat name with path
    const char *path = "armv7/sbin/"; // size 11
    char concat_name[strlen(name) + 11];

    memcpy(concat_name, path, strlen(path));
    memcpy(&concat_name[strlen(path)], name, strlen(name)+1);
    
    struct mem_region *mr = multiboot_find_module(bi, concat_name);
    
    // struct capref frame = {
    //     .cnode = cnode_module,
    //     .slot  = mr->mrmod_slot,
    // };
    //
    // struct capref capcopy;
    // slot_alloc(&capcopy);
    // errval_t err = cap_copy(capcopy, frame);
    // if (err_is_fail(err)){
    //     err_print_calltrace(err);
    //     abort();
    // }
    
    
    if (mr == NULL){
        // FIXME convert this to user space printing
        debug_printf("Could not spawn '%s': Program does not exist.\n", name);
        free(stack_pop());
        return SYS_ERR_OK;
    }
    
    struct spawninfo si;

    err = spawn_load_with_args(&si, mr, concat_name, disp_get_core_id(),
            argv, envp);

    if (err_is_fail(err)) {
        debug_printf("Failed spawn image: %s\n", err_getstring(err));
        err_print_calltrace(err);
        return err;
    } 

    *pid = si.domain_id;
    register_process(&si, name);

    // Copy Init's EP to Memeater's CSpace
    struct capref cap_dest;
    cap_dest.cnode = si.taskcn;
    cap_dest.slot = TASKCN_SLOT_INITEP;
    err = cap_copy(cap_dest, cap_initep);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to copy init ep to memeater cspace\n");
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
    
    // struct capref frame = {
    //     .cnode = cnode_module,
    //     .slot  = mr->mrmod_slot,
    // };
    //
    // err = cap_destroy(frame);
    // if (err_is_fail(err)){
    //     err_print_calltrace(err);
    //     abort();
    // }
    
    return SYS_ERR_OK;
}

static size_t get_pids_count(void) 
{
    struct process *temp = fst_process;
    size_t n = 0;
    while (temp != NULL) {
        temp = temp->next;
        n++;
    }
    return n;
}

static errval_t get_pid_at_index(uint32_t idx, domainid_t *pid)
{
    assert(fst_process);
    struct process *current = fst_process;

    for (uint32_t j = 1; j < idx; j++) {
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
        debug_printf("sending %s\n", buf);
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
    struct process *temp = fst_process;
    while (temp != NULL) {
        if (pid == temp->pid) {
            return temp->name;
        }
    }
    return NULL;
}

static void register_process(struct spawninfo *si, const char *name)
{
    struct process *temp = fst_process;
    while (temp->next != NULL) {
        temp = temp->next;
    }
    temp->next = (struct process *) malloc(sizeof(struct process));
    temp->next->pid = si->domain_id;
    temp->next->name = name;
    temp->next->next = NULL;
}

// initializes all essential services..
// void bootstrap(void) 
// {

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
    
    // client_stack = (struct client_state *) malloc(sizeof(struct client_state));
    fst_process = (struct process *) malloc(sizeof(struct process));

    // Register INIT as first process!!!
    fst_process->pid = disp_get_domain_id();
    fst_process->name = "init";
    fst_process->next = NULL;

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
    
    client_stack = NULL;

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
    domainid_t memeater_pid;

    debug_printf("Spawning memeater...\n");

    err = spawn("memeater", &memeater_pid);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to spawn memeater\n");
    } else {
        debug_printf("Done\n");
    }
    
    
    void *c = stack_pop;
    c = stack_push;
    c=c;

    debug_printf("Entering dispatch loop\n");
    while(true) {
        event_dispatch(get_default_waitset());
    }

    return EXIT_SUCCESS;
}
