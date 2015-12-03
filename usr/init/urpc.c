#include "init.h"

static coreid_t mycoreid;

static bool sync_process_wait_finish; // we use this to hold the SHELL, when a process
							   // does not run in background.

static void _urpc_init(struct urpc_blob blob, enum blob_state state, 
						uintptr_t start, coreid_t owner) 
{
	blob.base 		= (lvaddr_t) start;
	blob.state		= state;
	blob.owner	 	= owner;

	memcpy((void *) blob.base, &blob, sizeof(struct urpc_blob));
}

void urpc_init(uintptr_t start, coreid_t target_core) 
{
	mycoreid = disp_get_core_id();
	assert(target_core != mycoreid);

	if (mycoreid == 0) {
		_urpc_init(out, READ, start, mycoreid);
		_urpc_init(in, WRITTEN, start + 4096, target_core);
	} else {
		_urpc_init(in, WRITTEN, start, target_core);
		_urpc_init(out, READ, start, mycoreid);
	}

	sync_process_wait_finish = true;
}

static void urpc_process_spawn(struct urpc_spawn *inst) {

	errval_t err;

	switch(inst->status) {
		case SPAWN:
		{
			// call init's spawn.
			err = spawn(inst->appname, &(inst->pid), inst->exec_core);
			if (err_is_fail(err)) {
				DEBUG_ERR(err, "failed to spawn at destination core\n");
				abort();
			}
			break;
		}
		case SPAWN_DONE:
		{
			// this core is being informed that a process has been spawned in 
			// the other core!
			debug_printf("Process ID: %d spawned in Core: %d\n", inst->pid, 
						 inst->exec_core);
			break;
		}
		case EXEC_DONE:
		{
			// this core is being informed that a foreground process terminated
			// on the other core!
			sync_process_wait_finish = true;
			break;
		}
	}
}

errval_t urpc_poll(coreid_t coreid)
{
	errval_t err;
	struct urpc_inst inst;
	while (true) {
		err = urpc_read(&inst); 
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "failed to read from urpc frame\n");
			return err;
		}

		switch(inst.code) {
			case URPC_SPAWN: {
				urpc_process_spawn(&(inst.inst.spawn_inst));
				break;
			}
			default: {
				debug_printf("no such instruction!\n");
			}
		}
	}
    return SYS_ERR_OK;
}

errval_t urpc_remote_spawn(coreid_t exec_core, 
						   char *appname, 
						   domainid_t pid, 
						   bool background,
						   enum urpc_spawn_status status) {

	assert(appname != NULL);

	if (strlen(appname) > 256) {
		return URPC_ERR_APP_NAME_EXCEEDED;
	}

	struct urpc_spawn instr = {
		.background 	= background,
		.exec_core 		= exec_core,
		.pid 			= pid,
		.status 		= status
	};

	struct urpc_inst _inst = {
		.code 			= URPC_SPAWN,
		.inst 			= {
			.spawn_inst = instr
		}
	};

	debug_printf("HELLO \n");
	debug_printf("received appname: %s\n", appname);

	memcpy(&(instr.appname), 0, 256);
	memcpy(&(instr.appname), appname, strlen(appname));

	debug_printf("HELLO 1\n");

	errval_t err = urpc_write(&_inst);
	if (err_is_fail(err)) {
		DEBUG_ERR(err, "failed to write urpc frame\n");
		return err;
	}

	if (!background) {
		while(sync_process_wait_finish) {
			thread_yield();
		}
	}

	sync_process_wait_finish = true;

	return SYS_ERR_OK;
}
