#include "init.h"

static coreid_t mycoreid;

static bool sync_process_wait_finish; // we use this to hold the SHELL, when a process
							   // does not run in background.

static void _urpc_init(enum blob_state state, uintptr_t start, coreid_t owner) 
{
	struct urpc_blob temp = {
		.state 	= state,
		.owner 	= owner,
		.inst 	= {
			.code = URPC_NOP
		}
	};

	memcpy((void *) start, &temp, sizeof(struct urpc_blob));
}

void urpc_init(uintptr_t start, coreid_t target_core) 
{
	mycoreid = disp_get_core_id();
	assert(target_core != mycoreid);

	if (mycoreid == 0) {
		_urpc_init(READ, start, mycoreid);
		out = (struct urpc_blob *) start;
		_urpc_init(WRITTEN, start + 0x1000, target_core);
		in = (struct urpc_blob *) (start + 0x1000);
	} else {
		_urpc_init(READ, start, target_core);
		in = (struct urpc_blob *) start;
		_urpc_init(WRITTEN, start + 0x1000, mycoreid);
		out = (struct urpc_blob *) (start + 0x1000);
	}

	//debug_printf("OUT virtual memory address: 0x%08x\n", out);
	//debug_printf("IN virtual memory address: 0x%08x\n", in);

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

int urpc_poll(void)
{
	errval_t err;
	struct urpc_inst inst;

	debug_printf("urpc polling...\n");

	while (true) {
		err = urpc_read(&inst); 
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "failed to read from urpc frame\n");
			return err;
		}

		switch(inst.code) {
			case URPC_NOP: {
				debug_printf("URPC NOP!\n");
				break;
			}
			case URPC_SPAWN: {
				urpc_process_spawn(&(inst.inst.spawn_inst));
				break;
			}
			default: {
				debug_printf("no such instruction!\n");
			}
		}
	}
	return 0;
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

	strcpy((char *) instr.appname, appname);

	debug_printf("OUT STATUS: %d\n", out->state); // want 1 instead of 0;

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
