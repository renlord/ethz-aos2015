#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>
#include <barrelfish/lmp_chan.h>
#include <barrelfish/aos_rpc.h>
#include <barrelfish/paging.h>
#include <omap44xx_map.h>
#include <barrelfish/deferred.h>

#define GPIO1_BASE 0x4a310000

// modes supported by application
#define LED_ON_MODE 	1
#define LED_OFF_MODE 	0
