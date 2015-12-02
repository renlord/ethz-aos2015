#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/debug.h>

#define BSP_URPC_BUF 	0
#define APP_URPC_BUF 	1

enum urpc_code {
	URPC_SPAWN,
};

// typically a command word has 32bits, where 
// 0:4 is reserved for COMMAND STATE
// 4:8 is reserved for owning coreid
// 8:12 is reserved for URPC CODE
// 12:32 is reserved for nothing
enum command_state{
	WRITTEN,
	READ
};

struct urpc_blob {
	lvaddr_t 	base;
	size_t		bufsize; // in bytes please.
	size_t 		blob_size;
	uint32_t 	*command;
	void		*content;
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
	char						appname[256];
};

// GENERIC URPC INSTRUCTION STRUCTURE
struct urpc_inst {
	enum urpc_code 	code;
	union {
		struct urpc_spawn spawn_inst;
		// more urpc instructions to come!
	} inst;
};

// well-known blobs in the buffer
struct urpc_blob *in;
struct urpc_blob *out;

static inline bool urpc_is_readable(struct urpc_blob *buf, coreid_t owner) 
{
	if ((*(buf->command) & WRITTEN) == WRITTEN) {
		if ((*(buf->command) & (owner << 4)) == (owner << 4)) {
			return true;
		} 
	} 
	return false;
}

static inline bool urpc_is_writable(struct urpc_blob *buf, coreid_t mycoreid) 
{
	if ((*(buf->command) & READ) == READ) {
		if ((*(buf->command) & (mycoreid << 4)) == (mycoreid << 4)) {
			return true;
		} 
	} 
	return false;
}

static inline void urpc_write_to_read(struct urpc_blob *buf, coreid_t othercore)
{
	uint32_t command = *(buf->command);
	command &= 0xFFFFFF00;
	command |= (othercore << 4) | READ;
}

static inline void urpc_read_to_write(struct urpc_blob *buf, coreid_t mycoreid)
{
	uint32_t command = *(buf->command);
	command &= 0xFFFFFF00;
	command |= (mycoreid << 4) | WRITTEN;
}

static inline errval_t urpc_read(struct urpc_blob *buf, 
								 struct urpc_inst *inst,
								 coreid_t othercore) 
{
	assert(buf != NULL);

	while(!urpc_is_readable(buf, othercore)) {
		thread_yield();
	}

	memcpy((void *) inst, buf->content, sizeof(struct urpc_inst));
	memcpy(buf->content, 0, buf->bufsize);

	//debug_printf("URPC RECEIVED: %s\n", buf->content);
	//debug_printf("URPC READ: %s\n", msg);
	urpc_write_to_read(buf, othercore);
	return SYS_ERR_OK;
}

static inline errval_t urpc_write(struct urpc_blob *buf, 
								  struct urpc_inst *inst, 
								  size_t write_size, 
								  coreid_t mycoreid) 
{
	assert(buf != NULL);

	if (write_size > buf->bufsize) {
		return URPC_ERR_BUF_LEN_EXCEEDED;
	}
	debug_printf("hello\n");
	while(!urpc_is_writable(out, mycoreid)) {
		thread_yield();
	}
	debug_printf("hello2\n");

	memcpy(buf->content, inst, write_size);

	urpc_read_to_write(buf, mycoreid);
	return SYS_ERR_OK;
}

void urpc_init(uintptr_t start, size_t framesize);
errval_t urpc_poll(coreid_t coreid);
errval_t urpc_remote_spawn(coreid_t exec_core, char *appname, domainid_t pid, 
						   bool background, enum urpc_spawn_status status);


