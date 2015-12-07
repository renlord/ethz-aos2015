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
static struct ps_state *ps_states;

struct bootinfo *bi;
struct capref cap_spawndep;
struct lmp_chan main_channel;
struct ps_state spawnd_state;

struct serial_write_lock {
    bool lock;
    struct ps_state *cli;
};

struct serial_read_lock {
    bool lock;
    struct ps_state *cli;
};

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

static errval_t get_ram_cap(struct ps_state *ps_state, size_t req_bits)
{
    struct capref dest = NULL_CAP;
    
    // Perform the allocation
    errval_t err = ram_alloc(&dest, req_bits);
    if (err_is_fail(err)){
        debug_printf("Could not allocate ram.\n");
        err_print_calltrace(err);
        return err;
    }
    
    // Send cap and return bits back to caller
    ps_state->send_msg[0] = REQUEST_RAM_CAP;
    ps_state->send_msg[1] = req_bits;
    ps_state->send_cap = dest;

    return err;
}

static struct ps_state *create_ps_state(void)
{
    struct ps_state *new_state =
        (struct ps_state*)malloc(sizeof(struct ps_state));
    new_state->next = NULL;
    new_state->char_count = 0;
    new_state->send_cap = NULL_CAP;
    new_state->pending_request = false;
    new_state->status = WAITING;
    
    if (ps_states == NULL) {
        debug_printf("setting first process\n");
        ps_states = new_state;
    } else {
    
        struct ps_state *cur = ps_states;
        while (cur->next != NULL){
            cur = cur->next;
        }
        cur->next = new_state;
    }
    return new_state;
}

static struct ps_state *get_ps_state_by_pid(domainid_t pid)
{
    struct ps_state *cur = ps_states;
    while (cur != NULL && cur->pid != pid) {
        cur = cur->next;
    }
    if (cur == NULL){
        cur = create_ps_state();
        cur->pid = pid;
    }
    
    return cur;
}

static void set_active_ps_to_waiting(void)
{
    struct ps_state *cur = ps_states;
    while (cur != NULL) {
        if(cur->status == ACTIVE){
            cur->status = WAITING;
            break;
        }
        cur = cur->next;
    }
    
    if (cur == NULL){
        debug_printf("NO ACTIVE PROCESS\n");
    }
}


static void spawnd_recv_handler(void *lc_in)
{
    assert(lc_in == &spawnd_state.lc);
    
    struct capref remote_cap;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    uint32_t rpc_code;
    
    errval_t err = aos_retrieve_msg(&spawnd_state.lc, &remote_cap,
                                    &rpc_code, &msg);
    if (err_is_fail(err)) {
        debug_printf("Could not retrieve msg on main channel\n");
        err_print_calltrace(err);
        abort();
    }
    err = lmp_chan_register_recv(&spawnd_state.lc, get_default_waitset(),
        MKCLOSURE(spawnd_recv_handler, &spawnd_state.lc));
    if(err_is_fail(err)){
        debug_printf("Could not re-register for spawnd_recv_handler: %s\n",
            err_getstring(err));
        err_print_calltrace(err);
        abort();
    }
    
    switch(rpc_code){
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
            
            aos_chan_send_string(&spawnd_state.lc, "shell");
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
        case REQUEST_RAM_CAP:
        {
            err = get_ram_cap(&spawnd_state, msg.words[0]);
            if (err_is_fail(err)){
                debug_printf("Could not allocate ram for spawnd.\n");
                err_print_calltrace(err);
                abort();
            }
            
            err = lmp_chan_register_send(&spawnd_state.lc,
                get_default_waitset(), MKCLOSURE(send_handler, &spawnd_state));
            if (err_is_fail(err)) {
                err_print_calltrace(err);
                abort();
            }
            
            break;
        }
        
        case PROCESS_TO_FOREGROUND:
        {
            struct ps_state *state =
                    get_ps_state_by_pid(msg.words[0]);
            set_active_ps_to_waiting();
            state->status = ACTIVE;
            debug_printf("put pid %d in foreground\n",
                state->pid);
            if (state->pending_request) {
                debug_printf("Sending delayed msg\n");
                send_handler(state);
            }
            break;
        }
        
        case PROCESS_TO_BACKGROUND:
        {
            debug_printf("process to background called\n");
            struct ps_state *state =
                    get_ps_state_by_pid(msg.words[0]);
            state->status = BACKGROUND;
            break;
        }
        
        default:
        {
            debug_printf("Could not handle rpc code %d in "
                         "spawnd_recv_handler\n", rpc_code);
        }
    }
}


static void default_recv_handler(void *ps_state_in)
{
    errval_t err;
    struct capref remote_cap;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    uint32_t rpc_code;

    struct ps_state *ps_state = (struct ps_state *)ps_state_in;
    err = aos_retrieve_msg(&ps_state->lc, &remote_cap, &rpc_code, &msg);
    
    // Re-register
    lmp_chan_register_recv(&ps_state->lc, get_default_waitset(),
        MKCLOSURE(default_recv_handler, ps_state));
    
    bool reply = false;

    switch(rpc_code) {
        
        case SEND_TEXT:
        {
            for(uint8_t i = 0; i < 8; i++){
                 ps_state->mailbox[ps_state->char_count] = msg.buf.words[i];
                 ps_state->char_count++;
                 
                 if (msg.buf.words[i] == '\0') {
                    ps_state->char_count = 0;
                    break;
                }
            }
            break;
        }
        
        // Returns a RAM capability to the client
        case REQUEST_RAM_CAP:
        {
            err = get_ram_cap(ps_state, msg.words[0]);
            reply = true;            
            break;
        }
        
        case REQUEST_DEV_CAP:
        {
            debug_printf("handing out REQUEST_DEV_CAP\n");
            ps_state->send_msg[0] = REQUEST_DEV_CAP;
            ps_state->send_cap = cap_io;
            reply = true;
            
            break;
        }

        case SERIAL_PUT_CHAR:
        {
            if (ps_state->status == BACKGROUND) {
                break;
            } else if (ps_state->status == WAITING) {
                reply = true;
                break;
            }

            serial_put_char((char *) &msg.buf.words[0]);
            
            break;
        }

        case SERIAL_GET_CHAR:
        {
            if (ps_state->status == BACKGROUND) {
                break;
            } else if (ps_state->status == WAITING) {
                reply = true;
                break;
            }
            
            char c;
            serial_get_char(&c);
            
            ps_state->send_msg[0] = SERIAL_GET_CHAR;
            ps_state->send_msg[1] = c;
            ps_state->send_cap = NULL_CAP;
            reply = true;
            
            break;
        }
        
        default:
        {
            debug_printf("Could not handle code %d in "
                         "default_recv_handler\n", rpc_code);
            return;
        }
    }
    
    if(reply){
        if (ps_state->status == WAITING){
            ps_state->pending_request = true;
        } else {
            for(uint32_t i = 0; i < RETRYS; i++){
                err = lmp_chan_register_send(&ps_state->lc, get_default_waitset(),
                    MKCLOSURE(send_handler, ps_state));
                if (err_is_ok(err)) {
                    break;
                }
                debug_printf("retrying\n");
            }
            
            if(err_is_fail(err)){
                // TODO kill proc
                return;
            }     
        }
    } else {
        ps_state->pending_request = false;
    }
}

static void initial_recv_handler(void *null_ptr)
{
    assert(null_ptr == NULL);
    
    struct capref remote_cap;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    uint32_t rpc_code;
    
    errval_t err = aos_retrieve_msg(&main_channel, &remote_cap,
                                    &rpc_code, &msg);
    if (err_is_fail(err)) {
        debug_printf("Could not retrieve msg on main channel\n");
        err_print_calltrace(err);
        abort();
    }
    
    assert(rpc_code == REGISTER_CHANNEL); // FIXME handle gracefully

    struct ps_state *new_state = get_ps_state_by_pid(msg.words[0]);
    err = aos_setup_channel(&new_state->lc, remote_cap,
        MKCLOSURE(default_recv_handler, new_state));

    new_state->send_msg[0] = REGISTER_CHANNEL;
    new_state->send_cap = new_state->lc.local_cap;
    send_handler(new_state);
    
    // re-register receive handler
    err = lmp_chan_register_recv(&main_channel, get_default_waitset(),
        MKCLOSURE(initial_recv_handler, NULL));
    if (err_is_fail(err)){
        printf("Could not register receive handler!\n");
        abort();
    }
    
    return;
}

static errval_t spawn_spawnd(void)
{
    assert(bi != NULL);
    errval_t err;
    
    debug_printf("ram alloc state: 0x%08x\n", get_ram_alloc_state());
    
    spawnd_state.next = NULL; // FIXME ??
    spawnd_state.status = BACKGROUND;
        
    // Parse cmd line args
    char *argv[2];
    argv[0] = malloc(7);
    sprintf(argv[0], "spawnd");

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
        DEBUG_ERR(err, "Failed to copy EP for spawnd<->init "
                        "communication to new process' cspace\n");
        return err;
    }
    
    // Copy init's endpoint for spawnd to handout to new processes
    spawnd_initep.cnode = si.taskcn;
    spawnd_initep.slot = TASKCN_SLOT_SPAWNDEP;
    err = cap_copy(spawnd_initep, cap_initep);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to copy EP for process<->init "
                       "communication to new process' cspace\n");
        return err;
    }
    
    struct capref module_cap_init = {
        .cnode = cnode_root,
        .slot = ROOTCN_SLOT_MODULECN,
    };

    struct capref module_cap_spawnd = {
        .cnode = si.rootcn,
        .slot = ROOTCN_SLOT_MODULECN,
    };
    
    err = cap_copy(module_cap_spawnd, module_cap_init);
    if (err_is_fail(err)){
        err_print_calltrace(err);
        abort();
    }
    
    
    // Calculate bootinfo size
    size_t bi_size = sizeof(struct bootinfo) + sizeof(struct mem_region)*bi->regions_length;

    // Allocate frame for new bootinfo in own cspace
    // (FIXME we could skip this if we knew how to get a hold of
    //        the frame cap for the bootinfo at adress bi)
    struct capref bi_cap;
    size_t retbytes;
    err = frame_alloc(&bi_cap, bi_size, &retbytes);
    if(err_is_fail(err) || bi_size > retbytes){
        debug_printf("Could not allocate bootinfo for spawnd.\n");
        abort();
    }
    
    // Allocate virtual memory in own vspace
    lvaddr_t my_bi_addr;
    err = paging_map_frame_attr(get_current_paging_state(), (void**)&my_bi_addr,
        bi_size, bi_cap, VREGION_FLAGS_READ_WRITE_NOCACHE, NULL, NULL);
    
    // Map the frame cap in spawnd's memory space
    err = paging_map_frame_attr(si.vspace, (void**)&spawnd_bi_addr,
        bi_size, bi_cap, VREGION_FLAGS_READ_WRITE_NOCACHE, NULL, NULL);
    if(err_is_fail(err)){
        debug_printf("Could not map bootinfo frame\n");
        abort();
    }

    // Copy bootinfo data to new frame
    memcpy((void *)my_bi_addr, bi, bi_size);
    
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
    
    lmp_chan_init(&main_channel);
  
    struct lmp_endpoint *my_ep;
    lmp_endpoint_setup(0, FIRSTEP_BUFLEN, &my_ep);

    main_channel.endpoint = my_ep;
    main_channel.local_cap = cap_selfep;

    // allocate slot for incoming capability from memeatermultiboot_find_module(bi, name);
    err = lmp_chan_alloc_recv_slot(&main_channel);
    if (err_is_fail(err)){
        printf("Could not allocate receive slot!\n");
        exit(-1);
    }

    // register receive handler
    err = lmp_chan_register_recv(&main_channel, ws,
        MKCLOSURE(initial_recv_handler, NULL));
    if (err_is_fail(err)){
        printf("Could not register receive handler!\n");
        abort();
    }

    debug_printf("cap_initep, cap_selfep OK.\n");

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
        debug_printf("Spawning spawnd...\n");
        err = spawn_spawnd();
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "failed to spawn spawnd\n");
            abort();
        }
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
