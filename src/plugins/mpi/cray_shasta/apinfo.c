/*
 * apinfo.c - Cray Shasta PMI apinfo file creation
 * Copyright 2019 Cray Inc. All Rights Reserved.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>

#include "src/common/xstring.h"

#include "mpi_cray_shasta.h"

/*
 * Get a NID from a hostname, in format nidXXXXXX.
 * Trailing characters are ignored.
 * Returns -1 if the hostname is not in the expected format.
 */
static int _get_nid(const char *hostname)
{
	int nid = -1;
	if (sscanf(hostname, "nid%d", &nid) < 1 || nid < 0) {
		return -1;
	}
	return nid;
}

/*
 * Parse an MPMD file to determine the number of MPMD commands and task->cmd
 * mapping. Adopted from multi_prog_parse in src/slurmd/slurmstepd/multi_prog.c.
 *
 * The file's contents are stored in job->argv[1], and follow this format:
 * <taskids> <command> <arguments>
 *
 * taskids is a range list of task IDs or * (for all remaining task IDs).
 * command and arguments give the argv to run for those tasks.
 * Empty lines and lines starting with # are ignored.
 * Newlines may be escaped with \.
 */
void _multi_prog_parse(const stepd_step_rec_t *job, int *ncmds,
		       uint32_t **tid_offsets)
{
	int i = 0, line_num = 0, rank_id = 0, num_cmds = 0, nranks = 0;
	char *line = NULL, *local_data = NULL;
	char *end_ptr = NULL, *save_ptr = NULL, *tmp_str = NULL;
	char *rank_spec = NULL, *p = NULL, *one_rank = NULL;
	hostlist_t hl;
	uint32_t *offsets = NULL;

	offsets = xmalloc(job->ntasks * sizeof(uint32_t));
	for (i = 0; i < job->ntasks; i++) {
		offsets[i] = NO_VAL;
	}

	// Copy contents of MPMD file so we can tokenize it
	local_data = xstrdup(job->argv[1]);

	// Replace escaped newlines with spaces
	while ((p = xstrstr(local_data, "\\\n")) != NULL) {
		p[0] = ' ';
		p[1] = ' ';
	}

	while (1) {
		// Get the next line
		if (line_num)
			line = strtok_r(NULL, "\n", &save_ptr);
		else
			line = strtok_r(local_data, "\n", &save_ptr);
		if (!line)
			break;
		line_num++;

		// Get task IDs from the line
		p = line;
		while ((*p != '\0') && isspace(*p)) /* remove leading spaces */
			p++;
		if (*p == '#') /* only whole-line comments handled */
			continue;
		if (*p == '\0') /* blank line ignored */
			continue;

		rank_spec = p; /* Rank specification for this line */
		while ((*p != '\0') && !isspace(*p))
			p++;
		if (*p == '\0')
			goto fail;
		*p++ = '\0';

		while ((*p != '\0') && isspace(*p)) /* remove leading spaces */
			p++;
		if (*p == '\0') /* blank line ignored */
			continue;

		nranks = 0;
		// If rank_spec is '*', set all remaining ranks to this cmd
		if (!xstrcmp(rank_spec, "*")) {
			for (i = 0; i < job->ntasks; i++) {
				if (offsets[i] == NO_VAL) {
					offsets[i] = num_cmds;
					nranks++;
				}
			}
		} else {
			// Parse rank list into individual ranks
			xstrfmtcat(tmp_str, "[%s]", rank_spec);
			hl = hostlist_create(tmp_str);
			xfree(tmp_str);
			if (!hl)
				goto fail;
			while ((one_rank = hostlist_pop(hl))) {
				rank_id = strtol(one_rank, &end_ptr, 10);
				if ((end_ptr[0] != '\0') || (rank_id < 0) ||
				    (rank_id >= job->ntasks)) {
					free(one_rank);
					hostlist_destroy(hl);
					error("mpi/cray_shasta: invalid rank "
					      "id %s",
					      one_rank);
					goto fail;
				}
				free(one_rank);

				offsets[rank_id] = num_cmds;
				nranks++;
			}
			hostlist_destroy(hl);
		}
		// Only count this command if it had at least one rank
		if (nranks > 0) {
			num_cmds++;
		}
	}

	// Make sure we've initialized all ranks
	for (i = 0; i < job->ntasks; i++) {
		if (offsets[i] == NO_VAL) {
			error("mpi/cray_shasta: no command for task id %d", i);
			goto fail;
		}
	}

	xfree(local_data);
	*ncmds = num_cmds;
	*tid_offsets = offsets;
	return;

fail:
	xfree(offsets);
	xfree(local_data);
	*ncmds = 0;
	*tid_offsets = NULL;
	return;
}

/*
 * Return an array of pals_pe_t structures.
 */
static pals_pe_t *_setup_pals_pes(int ntasks, int nnodes, uint16_t *task_cnts,
				  uint32_t **tids, uint32_t *tid_offsets)
{
	pals_pe_t *pes = NULL;
	int nodeidx, localidx, taskid;

	pes = xmalloc(ntasks * sizeof(pals_pe_t));
	for (nodeidx = 0; nodeidx < nnodes; nodeidx++) {
		for (localidx = 0; localidx < task_cnts[nodeidx]; localidx++) {
			taskid = tids[nodeidx][localidx];
			if (taskid >= ntasks) {
				error("mpi/cray_shasta: task %d node %d >= "
				      "ntasks %d; skipping",
				      taskid, nodeidx, ntasks);
				continue;
			}
			pes[taskid].nodeidx = nodeidx;
			pes[taskid].localidx = localidx;

			if (tid_offsets == NULL) {
				pes[taskid].cmdidx = 0;
			} else {
				pes[taskid].cmdidx = tid_offsets[taskid];
			}
		}
	}
	return pes;
}

/*
 * Return an array of pals_cmd_t structures.
 */
static pals_cmd_t *_setup_pals_cmds(int ncmds, int ntasks, int nnodes,
				    int cpus_per_task, pals_pe_t *pes)
{
	pals_cmd_t *cmds;
	int peidx, cmdidx, nodeidx, max_ppn;
	int **cmd_ppn;

	// Allocate and initialize arrays
	cmds = xcalloc(ncmds, sizeof(pals_cmd_t));
	cmd_ppn = xmalloc(ncmds * sizeof(int *));
	for (cmdidx = 0; cmdidx < ncmds; cmdidx++) {
		cmd_ppn[cmdidx] = xcalloc(nnodes, sizeof(int));
	}

	// Count number of PEs for each command/node
	for (peidx = 0; peidx < ntasks; peidx++) {
		cmdidx = pes[peidx].cmdidx;
		nodeidx = pes[peidx].nodeidx;
		if (cmdidx >= 0 && cmdidx < ncmds && nodeidx >= 0 &&
		    nodeidx < nnodes) {
			cmd_ppn[cmdidx][nodeidx]++;
		}
	}

	// Fill in command information
	for (cmdidx = 0; cmdidx < ncmds; cmdidx++) {
		// NOTE: we don't know each job's depth for a heterogeneous job
		cmds[cmdidx].cpus_per_pe = cpus_per_task;

		// Find the total PEs and max PEs/node for this command
		max_ppn = 0;
		for (nodeidx = 0; nodeidx < nnodes; nodeidx++) {
			cmds[cmdidx].npes += cmd_ppn[cmdidx][nodeidx];
			if (cmd_ppn[cmdidx][nodeidx] > max_ppn) {
				max_ppn = cmd_ppn[cmdidx][nodeidx];
			}
		}
		xfree(cmd_ppn[cmdidx]);

		cmds[cmdidx].pes_per_node = max_ppn;
	}

	xfree(cmd_ppn);
	return cmds;
}

/*
 * Fill in the apinfo header
 */
static void _build_header(pals_header_t *hdr, int ncmds, int npes, int nnodes)
{
	size_t offset = sizeof(pals_header_t);

	memset(hdr, 0, sizeof(pals_header_t));
	hdr->version = PALS_APINFO_VERSION;

	hdr->comm_profile_size = sizeof(pals_comm_profile_t);
	hdr->comm_profile_offset = offset;
	hdr->ncomm_profiles = 0;
	offset += hdr->comm_profile_size * hdr->ncomm_profiles;

	hdr->cmd_size = sizeof(pals_cmd_t);
	hdr->cmd_offset = offset;
	hdr->ncmds = ncmds;
	offset += hdr->cmd_size * hdr->ncmds;

	hdr->pe_size = sizeof(pals_pe_t);
	hdr->pe_offset = offset;
	hdr->npes = npes;
	offset += hdr->pe_size * hdr->npes;

	hdr->node_size = sizeof(pals_node_t);
	hdr->node_offset = offset;
	hdr->nnodes = nnodes;
	offset += hdr->node_size * hdr->nnodes;

	hdr->nic_size = sizeof(pals_nic_t);
	hdr->nic_offset = offset;
	hdr->nnics = 0;
	offset += hdr->nic_size * hdr->nnics;

	hdr->total_size = offset;
}

/*
 * Open the apinfo file and return a writeable fd, or -1 on failure
 */
static int _open_apinfo(const stepd_step_rec_t *job)
{
	int fd = -1;

	// Create apinfo name - put in per-application spool directory
	xstrfmtcat(apinfo, "%s/apinfo", appdir);

	// Create file
	fd = creat(apinfo, 0600);
	if (fd == -1) {
		error("mpi/cray_shasta: Couldn't open apinfo file %s: %m",
		      apinfo);
		close(fd);
		return -1;
	}

	// Change ownership of file to application user
	if (fchown(fd, job->uid, job->gid) == -1 && getuid() == 0) {
		error("mpi/cray_shasta: Couldn't chown %s to uid %d gid %d: %m",
		      apinfo, job->uid, job->gid);
		close(fd);
		return -1;
	}

	return fd;
}

/*
 * Write len bytes from buf to the given file.
 */
static int _writen(int fd, void *buf, size_t len)
{
	ssize_t ret;
	size_t nwrote = 0;

	while (nwrote < len) {
		ret = write(fd, buf + nwrote, len - nwrote);
		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				error("mpi/cray_shasta: couldn't write %zu "
				      "bytes to %s: %m",
				      len - nwrote, apinfo);
				return SLURM_ERROR;
			}
		} else {
			nwrote += ret;
		}
	}
	return SLURM_SUCCESS;
}

/*
 * Write the job's node list to the file
 */
static int _write_pals_nodes(int fd, char *nodelist)
{
	hostlist_t hl;
	char *host;
	pals_node_t node;

	memset(&node, 0, sizeof(pals_node_t));
	hl = hostlist_create(nodelist);
	if (hl == NULL) {
		error("mpi/cray: Couldn't create hostlist");
		return SLURM_ERROR;
	}
	while ((host = hostlist_shift(hl)) != NULL) {
		snprintf(node.hostname, sizeof(node.hostname), host);
		node.nid = _get_nid(host);

		if (_writen(fd, &node, sizeof(pals_node_t)) == SLURM_ERROR) {
			hostlist_destroy(hl);
			return SLURM_ERROR;
		}
	}

	hostlist_destroy(hl);
	return SLURM_SUCCESS;
}

/*
 * Write the application information file
 */
int create_apinfo(const stepd_step_rec_t *job)
{
	int fd = -1;
	pals_header_t hdr;
	pals_cmd_t *cmds = NULL;
	pals_pe_t *pes = NULL;
	int ntasks, ncmds, nnodes;
	uint16_t *task_cnts;
	uint32_t **tids;
	uint32_t *tid_offsets;
	char *nodelist;

	// Make sure the application spool directory has been created
	if (appdir == NULL) {
		return SLURM_ERROR;
	}

	// Get relevant information from job
	if (job->pack_jobid != NO_VAL) {
		ntasks = job->pack_ntasks;
		ncmds = job->pack_step_cnt;
		nnodes = job->pack_nnodes;
		task_cnts = job->pack_task_cnts;
		tids = job->pack_tids;
		tid_offsets = job->pack_tid_offsets;
		nodelist = job->pack_node_list;
	} else {
		ntasks = job->ntasks;
		nnodes = job->nnodes;
		task_cnts = job->msg->tasks_to_launch;
		tids = job->msg->global_task_ids;
		nodelist = job->msg->complete_nodelist;

		if (job->flags & LAUNCH_MULTI_PROG) {
			_multi_prog_parse(job, &ncmds, &tid_offsets);
		} else {
			ncmds = 1;
			tid_offsets = NULL;
		}
	}

	// Make sure we've got everything
	if (ntasks <= 0) {
		error("mpi/cray_shasta: no tasks found");
		goto error;
	}
	if (ncmds <= 0) {
		error("mpi/cray_shasta: no cmds found");
		goto error;
	}
	if (nnodes <= 0) {
		error("mpi/cray_shasta: no nodes found");
		goto error;
	}
	if (task_cnts == NULL) {
		error("mpi/cray_shasta: no per-node task counts");
		goto error;
	}
	if (tids == NULL) {
		error("mpi/cray_shasta: no task IDs found");
		goto error;
	}
	if (nodelist == NULL) {
		error("mpi/cray_shasta: no nodelist found");
		goto error;
	}

	// Get information to write
	_build_header(&hdr, ncmds, ntasks, nnodes);
	pes = _setup_pals_pes(ntasks, nnodes, task_cnts, tids, tid_offsets);
	cmds = _setup_pals_cmds(ncmds, ntasks, nnodes, job->cpus_per_task, pes);

	// Create the file
	fd = _open_apinfo(job);
	if (fd == -1) {
		goto error;
	}

	// Write info
	if (_writen(fd, &hdr, sizeof(pals_header_t)) == SLURM_ERROR ||
	    _writen(fd, cmds, hdr.ncmds * sizeof(pals_cmd_t)) == SLURM_ERROR ||
	    _writen(fd, pes, hdr.npes * sizeof(pals_pe_t)) == SLURM_ERROR ||
	    _write_pals_nodes(fd, nodelist) == SLURM_ERROR) {
		goto error;
	}
	// TODO: Write communication profiles
	// TODO write nics

	// Flush changes to disk
	if (fsync(fd) == -1) {
		error("mpi/cray_shasta: Couldn't sync %s to disk: %m", apinfo);
		goto error;
	}

	debug("mpi/cray_shasta: Wrote apinfo file %s", apinfo);

	// Clean up and return
	if (job->flags & LAUNCH_MULTI_PROG) {
		xfree(tid_offsets);
	}
	xfree(pes);
	xfree(cmds);
	close(fd);
	return SLURM_SUCCESS;

error:
	if (job->flags & LAUNCH_MULTI_PROG) {
		xfree(tid_offsets);
	}
	xfree(pes);
	xfree(cmds);
	close(fd);
	return SLURM_ERROR;
}
