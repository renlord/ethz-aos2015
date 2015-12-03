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
	SPAWN_FAIL,
	SPAWN_UNKNOWN,
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
	enum blob_state		state;
	uint32_t			owner; 			
	struct urpc_inst	inst;
};

struct urpc_blob *in;
struct urpc_blob *out;

static inline bool urpc_check_inst_nop(struct urpc_blob *real)
{
	assert(real != NULL);
	return real->inst.code == URPC_NOP;
}

static inline void set_nop_inst(struct urpc_inst *inst_slot)
{
	assert(inst_slot != NULL);
	inst_slot->code = URPC_NOP;
}

static inline bool urpc_is_readable(void) 
{
	assert(in != NULL);
	return !urpc_check_inst_nop(in) && (in->state == WRITTEN);
}

static inline bool urpc_is_writable(void) 
{
	assert(out != NULL);
	return urpc_check_inst_nop(out) && (out->state == READ);
}

static inline void urpc_mark_blob_read(struct urpc_blob *buf) 
{
	assert(buf != NULL);
	buf->state = READ;
	set_nop_inst(&(buf->inst));
}

static inline void urpc_mark_blob_written(struct urpc_blob *buf)
{
	assert(buf != NULL);
	buf->state = WRITTEN;
}

static inline void spawn_inst_copy(struct urpc_spawn *s1, struct urpc_spawn *s2)
{
	*s2 = *s1;
	strcpy(s2->appname, s1->appname);
}

static inline errval_t urpc_read(struct urpc_inst *inst) 
{
	assert(inst != NULL && in != NULL);
	while(!urpc_is_readable()) {
		thread_yield();
	}
	memcpy((void *) inst, &(in->inst), sizeof(struct urpc_inst));

	//debug_printf("URPC RECEIVED: %s\n", buf->content);
	//debug_printf("URPC READ: %s\n", msg);
	debug_printf("GOT PROCESS NAME: %s\n", in->inst.inst.spawn_inst.appname);
	urpc_mark_blob_read(in);
	return SYS_ERR_OK;
}

static inline errval_t urpc_write(struct urpc_inst *_inst) 
{
	assert(out != NULL);
	while(!urpc_is_writable()) {
		thread_yield();
	}

	debug_printf("before copy appname: %s\n", _inst->inst.spawn_inst.appname);
	memcpy(&(out->inst), _inst, sizeof(struct urpc_inst));
	debug_printf("FINAL out appname: %s\n", out->inst.inst.spawn_inst.appname);
	urpc_mark_blob_written(out);
	//debug_printf("wrote to urpc frame\n");
	return SYS_ERR_OK;
}

void urpc_init(uintptr_t start, coreid_t target_core);
int urpc_poll(void);
errval_t urpc_remote_spawn(coreid_t exec_core, char *appname, domainid_t pid, 
						   bool background, enum urpc_spawn_status status);


