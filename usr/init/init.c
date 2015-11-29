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
// #define FIRSTEP_BUFLEN 20u
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

struct bootinfo *bi;
static coreid_t my_core_id;

static struct client_state *fst_client;
static struct process *fst_process;
    
static volatile uint32_t *uart3_thr = NULL;
static volatile uint32_t *uart3_rhr = NULL;
static volatile uint32_t *uart3_fcr = NULL;
static volatile uint32_t *uart3_lsr = NULL;

static errval_t spawn(const char *name, domainid_t *pid);
static errval_t get_all_pids(struct lmp_chan *lc);
static const char *get_pid_name(domainid_t pid);
static size_t get_pids_count(void);
static errval_t reply_string(struct lmp_chan *lc, const char *string);
static void register_process(struct spawninfo *si, const char *name);


struct client_state {
    struct client_state *next;
    struct lmp_chan *lc;
    size_t alloced;
    char mailbox[500];
    size_t char_count;
    uint32_t send_msg[9];
    struct capref send_cap;
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

//static struct serial_write_lock write_lock;
//static struct serial_read_lock read_lock;

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
    // debug_printf("receiving on lc 0x%08x\n", lc_in);
    struct capref remote_cap = NULL_CAP;
    struct lmp_chan *lc = (struct lmp_chan *)lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    
    errval_t err = lmp_chan_recv(lc, &msg, &remote_cap);

    if (err_is_fail(err)) {
        debug_printf("Could not retrieve message: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return;
    }

    // Our protocol requires that there is a procedure code
    // at a bare minimum
    if (msg.buf.msglen == 0) {
        debug_printf("Bad msg for init.\n");
        return;
    }

    // Re-register
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
    
    uint32_t code = msg.buf.words[0];

    struct client_state *cs = fst_client;
    while (cs != NULL && cs->lc->endpoint != lc->endpoint){
        cs = cs->next;
    }

    assert(cs != NULL || code == REQUEST_CHAN);

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
            debug_printf("init handed out channel 0x%08x\n", new_chan->endpoint);

            // Allocate new client state
            struct client_state **cur = &fst_client;
            struct client_state **prev;
            while(*cur != NULL) {
                prev = cur;
                cur = &((*cur)->next);
            }
            
            struct client_state *new_state =
                (struct client_state *) malloc(sizeof(struct client_state));
            new_state->next = NULL;
            new_state->lc = new_chan;
            new_state->alloced = 0;
            new_state->char_count = 0;
            *cur = new_state;

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

            // Send new endpoint cap back to client
            (*cur)->send_msg[0] = REQUEST_CHAN;
            (*cur)->send_msg[1] = '\0';
            (*cur)->send_cap = ep_cap;
            // we use the new lmp chan from now on, instead of the initial
            // lmp chan used to register a new channel. 
            err = lmp_chan_register_send(new_state->lc, get_default_waitset(),
                                         MKCLOSURE(send_handler, *cur));
            if (err_is_fail(err)) {
                debug_printf("Could not re-register for sending\n");
                return;
            }


            // Allocate receive struct right away for next msg
            err = lmp_chan_alloc_recv_slot(lc);
            if (err_is_fail(err)) {
                debug_printf("Could not allocate recv slot: %s.\n",
                    err_getstring(err));
                err_print_calltrace(err);
                return;
            }
    

            debug_printf("first lmp msg OK!\n");
            
            break;
        }
        
        case SEND_TEXT:
        {

            for(uint8_t i = 1; i < 9; i++){
                 cs->mailbox[cs->char_count] = msg.buf.words[i];
                 cs->char_count++;
                 
                 if (msg.buf.words[i] == '\0') {
                    debug_printf("Text msg received: %s\n",
                        cs->mailbox);
                    
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
                
                cs->send_msg[0] = SEND_TEXT;
                cs->send_msg[1] = (uint32_t)'p';
                cs->send_msg[2] = (uint32_t)'o';
                cs->send_msg[3] = (uint32_t)'n';
                cs->send_msg[4] = (uint32_t)'g';
                cs->send_msg[5] = (uint32_t)'\0';
                cs->send_cap = NULL_CAP;
                err = lmp_chan_register_send(lc_in, get_default_waitset(),
                                             MKCLOSURE(send_handler, cs));
                if (err_is_fail(err)) {
                    debug_printf("Could not register for sending\n");
                    return;
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
            cs->send_cap = dest;
            err = lmp_chan_register_send(lc_in, get_default_waitset(),
                                         MKCLOSURE(send_handler, cs));
            if (err_is_fail(err)) {
                debug_printf("Could not register for sending\n");
                return;
            }
            
            break;
        }
        
        case REQUEST_DEV_CAP:
        {
            cs->send_msg[0] = REQUEST_DEV_CAP;
            cs->send_cap = cap_io;
            err = lmp_chan_register_send(lc_in, get_default_waitset(),
                                         MKCLOSURE(send_handler, cs));
            if (err_is_fail(err)) {
                debug_printf("Could not register for sending\n");
                return;
            }

            break;
        }

        case SERIAL_PUT_CHAR:
        {
            if (msg.buf.msglen != 2) {
                debug_printf("invalid message size for serial put char!");
                return;
            }

            serial_put_char((char *) &msg.buf.words[1]);

            break;
        }

        case SERIAL_GET_CHAR:
        {
            char c;
            serial_get_char(&c);

            cs->send_msg[0] = SERIAL_GET_CHAR;
            cs->send_msg[1] = c;
            cs->send_cap = NULL_CAP;
            err = lmp_chan_register_send(lc_in, get_default_waitset(),
                                         MKCLOSURE(send_handler, cs));
            
            if (err_is_fail(err)) {
                debug_printf("Could not register for sending\n");
                return;
            }

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
            if (err_is_fail(err)) {
                err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
                    PROCESS_SPAWN, false);
                DEBUG_ERR(err, "failed to spawn process from SHELL.\n");
                err_print_calltrace(err);
                return;
            } else {
                err = lmp_chan_send3(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
                    PROCESS_SPAWN, true, pid);
                err_print_calltrace(err);
                return;
            }

            if (err_is_fail(err)) {
                DEBUG_ERR(err, "failed to reply SPAWN_PROCESS event.\n");
                err_print_calltrace(err);
                return;
            }
            break;
        }

        case PROCESS_GET_NAME: 
        {
            if (msg.buf.msglen != 2) {
                debug_printf("bad message for PROCESS_GET_NAME, 2 args"
                    " expected!\n");
                return;
            }
            
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
                return;
            }

            break;
        }

        case PROCESS_GET_ALL_PIDS:
        {
            if (msg.buf.msglen != 1) {
                debug_printf("bad message for PROCESS_GET_NAME, 1 args"
                    " expected!\n");
                return;
            }
            err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
                    PROCESS_GET_ALL_PIDS, get_pids_count());
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "failed to get pids count.\n");
                err_print_calltrace(err);
                return;
            }
            err = get_all_pids(lc);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "failed to get all pids event.\n");
                err_print_calltrace(err);
                return;
            }
            break;
        }

        default:
        {
            // include LIB_ERR_NOT_IMPLEMENTED? 
            debug_printf("Wrong code: %d\n", code);
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

static errval_t spawn(const char *name, domainid_t *pid)
{
    // concat name with path
    const char *path = "armv7/sbin/"; // size 11
    char concat_name[strlen(name) + 11];

    // strcat(concat_name, path);
    // strcat(concat_name, name);

    memcpy(concat_name, path, strlen(path));
    memcpy(&concat_name[strlen(path)], name, strlen(name)+1);
    
    struct mem_region *mr = multiboot_find_module(bi, concat_name);
    assert(mr != NULL);

    char *argv[1];
    argv[0] = "";
    
    char *envp[1];
    envp[0] = "";
    
    struct spawninfo si;
    errval_t err = spawn_load_with_args(&si, mr, concat_name, disp_get_core_id(),
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

static errval_t get_all_pids(struct lmp_chan *lc)
{
    struct process *temp = fst_process;
    while (temp != NULL) {
        errval_t err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
                PROCESS_GET_ALL_PIDS, temp->pid);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "fail to send one of many pids.\n");
            err_print_calltrace(err);
            return err;
        }
    }

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

// static void temp_core_func(void) 
// {
//     debug_printf("hello from core 2\n");
//     debug_printf("================================\n");
//     while(true);
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

    // boot second core
    struct capref urpc_frame;
    size_t retsize;
    err = frame_alloc(&urpc_frame, MON_URPC_SIZE, &retsize);
    if (err_is_fail(err) || retsize != MON_URPC_SIZE) {
        DEBUG_ERR(err, "fail to allocate urpc frame\n");
        abort();
    }

    struct frame_identity urpc_frame_id; 
    err = invoke_frame_identify(urpc_frame, &urpc_frame_id);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "fail to get urpc frame identity\n");
        abort();
    }

    err = spawn_core_load_kernel(bi, 1, 1, "", urpc_frame_id, cap_kernel);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "fail to boot 2nd core and load kernel\n");
        abort();
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

    debug_printf("cap_initep, cap_selfep OK.\n");
            
    size_t offset = OMAP44XX_MAP_L4_PER_UART3 - 0x40000000;
    lvaddr_t uart_addr = 1UL << 30;
    err = paging_map_user_device(get_current_paging_state(), uart_addr,
                            cap_io, offset, OMAP44XX_MAP_L4_PER_UART3_SIZE,
                            VREGION_FLAGS_READ_WRITE_NOCACHE);

    debug_printf("cap_io mapped OK.\n");

    if (err_is_fail(err)) {
        debug_printf("Could not map io cap: %s\n", err_getstring(err));
        err_print_calltrace(err);
        abort();
    }

    set_uart3_registers(uart_addr);
        
    struct waitset *ws = get_default_waitset();
    waitset_init(ws);
    
    struct lmp_chan lc;
    lmp_chan_init(&lc);
  
    /* make local endpoint available -- this was minted in the kernel in a way
     * such that the buffer is directly after the dispatcher struct and the
     * buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel 
     * sentinel word).
     */
    
    // fst_client = (struct client_state *) malloc(sizeof(struct client_state));
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
    
    fst_client = NULL;

    // Technically, we would just like to spawn a shell process and that's it.
    // TODO

    // PROCESS SPAWNING CODE 
    // TO BE MOVED TO DEDICATED program afterwards.
    debug_printf("Spawning memeater...\n");
    
    domainid_t memeater_pid;
    err = spawn("memeater", &memeater_pid);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to spawn memeater\n");
    }

    domainid_t blink_pid;
    err = spawn("blink", &blink_pid);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to spawn blink\n");
    }

    debug_printf("init domain_id: %d\n", disp_get_domain_id());

    debug_printf("Entering dispatch loop\n");
    while(true) {
        event_dispatch(get_default_waitset());
    }

    return EXIT_SUCCESS;
}
