#include <stdio.h>
#include "config.h"
#include <signal.h>
#include <sys/types.h>


const char plugin_name[]        = "topology 4d_torus plugin";
const char plugin_type[]        = "topology/4d_torus";

extern int init(void)
{
	verbose("%s loaded", plugin_name);
	return SLURM_SUCCESS;
}
