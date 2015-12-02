#include "init.h"

static bool sync_process_wait_finish; // we use this to hold the SHELL, when a process
							   // does not run in background.
static coreid_t mycoreid;

static void _urpc_init(struct urpc_blob *blob, uintptr_t start, size_t bufsize) 
{
	blob->base 		= (lvaddr_t) start;
	blob->bufsize 	= bufsize;
	blob->command 	= (uint32_t *) start;
	blob->content 	= (void *) start + 4;
}

void uprc_init(uintptr_t start, size_t framesize) 
{
	mycoreid = disp_get_core_id();

	// very unscalable design, but this will have to make do for now
	if (mycoreid == 0) {
		_urpc_init(in, start, framesize / 2);
		_urpc_init(out, start + 4096, framesize / 2);
	} else {
		_urpc_init(out, start, framesize / 2);
		_urpc_init(in, start, framesize / 2);
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
		err = urpc_read(in, &inst, coreid); 
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

	memcpy(&(instr.appname), 0, 256);
	memcpy(&(instr.appname), appname, strlen(appname));

	errval_t err = urpc_write(out, &_inst, sizeof(struct urpc_spawn), exec_core);
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
