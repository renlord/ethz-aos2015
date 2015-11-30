#include <stdio.h>
#include <barrelfish/barrelfish.h>
#include <spawndomain/spawndomain.h>

#define MAX_DOMAINS 256
#define MAX_CHILD_PROCESS 10 // quick hack to not deal with realloc

enum ps_status {
	PS_STATUS_RUNNING,
	PS_STATUS_ZOMBIE,
	PS_STATUS_SLEEP
};

enum ps_exit_cond {
	PS_EXIT_NORMAL,
	PS_EXIT_KILLED,
	PS_EXIT_ERROR
};

struct ps_entry {
	domainid_t pid;
	enum ps_status status;
	char *argv[MAX_CMDLINE_ARGS];
	char *argbuf;
	size_t argbytes;
	struct capref rootcn_cap, dcb;
	struct cnoderef rootcn;
	uint8_t exitcode;
	struct ps_entry *child[10];
};

struct ps_entry *registry[MAX_DOMAINS];

static inline void get_ps_info(uint32_t ps_info, 
							   domainid_t *domainid,
							   enum ps_status *status)
{
	// ps_info is a 32 bit integer which contains information about a process.
	*domainid = ps_info >> 26;
	*status = ps_info & 3;
}

static inline void put_ps_info(uint32_t *ps_info, 
							   struct ps_entry *ps)
{
	// ps_info is a 32 bit integer which contains information about a process.
	*ps_info = ps->pid << 26 | ps->status;
}

errval_t ps_register(struct ps_entry *entry, domainid_t *domainid, 
                     domainid_t parentid);
void ps_remove(domainid_t domain_id);

struct ps_entry *ps_get(domainid_t domain_id);
bool ps_exists(domainid_t domain_id);
