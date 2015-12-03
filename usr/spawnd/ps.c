#include "ps.h"


errval_t ps_register(struct ps_entry *entry, domainid_t *domainid, 
                     domainid_t parentid)
{
    assert(parentid < MAX_DOMAINS);

    struct ps_entry **child_slot;
    bool child_slot_avail = false;

    for (int i = 0; i < MAX_CHILD_PROCESS; i++) {
        if (registry[parentid]->child[i] == NULL) {
            child_slot = &registry[parentid]->child[i];
            child_slot_avail = true;
            break;
        }
    }

    if (!child_slot_avail) {
        return SPAWN_ERR_DOMAIN_ALLOCATE;
    }

    for (domainid_t i = 1; i < MAX_DOMAINS; i++) {
        if (registry[i] == NULL) {
            registry[i] = entry;
            *child_slot = registry[i];
            *domainid = i;
            return SYS_ERR_OK;
        }
    }

    return SPAWN_ERR_DOMAIN_ALLOCATE;
}

void ps_remove(domainid_t domain_id) 
{
    assert(domain_id < MAX_DOMAINS);
    for (uint8_t i = 0; i < MAX_CHILD_PROCESS; i++) {
        assert(registry[domain_id]->child[i] != NULL);
        ps_remove(registry[domain_id]->child[i]->pid);
        registry[domain_id]->child[i] = NULL;
    }
    registry[domain_id] = NULL;
};

bool ps_exists(domainid_t domain_id) 
{
    assert(domain_id < MAX_DOMAINS);
    return (registry[domain_id] == NULL) ? false : true;
}

struct ps_entry *ps_get(domainid_t domain_id) 
{
    if (domain_id > MAX_DOMAINS) { 
        return NULL;
    }
    return registry[domain_id];
}
