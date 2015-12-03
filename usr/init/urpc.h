#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/debug.h>

#define BSP_URPC_BUF 	0
#define APP_URPC_BUF 	1

enum urpc_code {
	URPC_NOP, //just for placeholder sake.
	URPC_SPAWN,
};

enum blob_state {
	WRITTEN,
	READ
};

// URPC SPAWN STRUCTURE
enum urpc_spawn_status {
	SPAWN,
	SPAWN_DONE,
	EXEC_DONE,
};

struct urpc_spawn {
	bool 						background;
	enum urpc_spawn_status 		status;
	coreid_t					exec_core;
	domainid_t 					pid;
	char						appname[256]; //app name + argv
};

// GENERIC URPC INSTRUCTION STRUCTURE
struct urpc_inst {
	enum urpc_code 	code;
	union {
		struct urpc_spawn spawn_inst;
		// more urpc instructions to come!
	} inst;
};

struct urpc_blob {
	lvaddr_t 			base; 
	enum blob_state		state;
	uint32_t			owner; 			
	struct urpc_inst	inst;
};

// well-known blobs in the buffer
struct urpc_blob in;
struct urpc_blob out;

static inline bool urpc_check_inst_nop(struct urpc_blob *real)
{
	assert(real != NULL);
	if (real->inst.code == URPC_NOP) {
		return true;
	} else {
		return false;
	}
}

static inline void set_nop_inst(struct urpc_inst *inst_slot)
{
	assert(inst_slot != NULL);
	inst_slot->code = URPC_NOP;
}

static inline bool urpc_is_readable(void) 
{
	struct urpc_blob *real = (struct urpc_blob *) in.base;
	assert(real != NULL);

	return (!urpc_check_inst_nop(real) && (real->state == WRITTEN)) ? 
				true : false;
}

static inline bool urpc_is_writable(void) 
{
	struct urpc_blob *real = (struct urpc_blob *) out.base;
	assert(real != NULL);

	return (!urpc_check_inst_nop(real) && (real->state == READ)) ? 
				true : false;
}

static inline void urpc_mark_blob_read(struct urpc_blob *buf) 
{
	assert(buf != NULL);

	struct urpc_blob *real = (struct urpc_blob *) buf->base;
	assert(real != NULL);

	real->state = READ;
	set_nop_inst(&(real->inst));
}

static inline void urpc_mark_blob_written(struct urpc_blob *buf)
{
	assert(buf != NULL);

	struct urpc_blob *real = (struct urpc_blob *) buf->base;
	assert(real != NULL);

	real->state = WRITTEN;
	set_nop_inst(&(real->inst));
}

static inline errval_t urpc_read(struct urpc_inst *inst) 
{
	assert(inst != NULL);
	while(!urpc_is_readable()) {
		thread_yield();
	}

	struct urpc_blob *real = (struct urpc_blob *) in.base;
	assert(real != NULL); 

	memcpy((void *) inst, &(real->inst), sizeof(struct urpc_inst));

	//debug_printf("URPC RECEIVED: %s\n", buf->content);
	//debug_printf("URPC READ: %s\n", msg);
	urpc_mark_blob_read(real);
	return SYS_ERR_OK;
}

static inline errval_t urpc_write(struct urpc_inst *inst) 
{
	assert(inst != NULL);
	while(!urpc_is_writable()) {
		thread_yield();
	}

	struct urpc_blob *real = (struct urpc_blob *) out.base;
	assert(real != NULL);

	memcpy((void *) &(real->inst), inst, sizeof(struct urpc_inst));

	urpc_mark_blob_written(real);
	return SYS_ERR_OK;
}

void urpc_init(uintptr_t start, coreid_t target_core);
errval_t urpc_poll(coreid_t coreid);
errval_t urpc_remote_spawn(coreid_t exec_core, char *appname, domainid_t pid, 
						   bool background, enum urpc_spawn_status status);


