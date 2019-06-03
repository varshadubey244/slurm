/*
 * apinfo.c - Cray Shasta PMI apinfo file creation
 * Copyright 2019 Cray Inc. All Rights Reserved.
 */
#include <assert.h>
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
 * Return an array of pals_pe_t structures. Returns NULL on error.
 */
static pals_pe_t *_setup_pals_pes(const stepd_step_rec_t *job)
{
	pals_pe_t *pes = NULL;
	int nodeidx, localidx, taskid;

	assert(job->ntasks > 0);

	pes = xmalloc(job->ntasks * sizeof(pals_pe_t));
	for (nodeidx = 0; nodeidx < job->nnodes; nodeidx++) {
		for (localidx = 0;
		     localidx < job->msg->tasks_to_launch[nodeidx];
		     localidx++) {
			taskid = job->msg->global_task_ids[nodeidx][localidx];
			assert(taskid < job->ntasks);
			pes[taskid].nodeidx = nodeidx;
			pes[taskid].localidx = localidx;

			// TODO: MPMD support
			pes[taskid].cmdidx = 0;
		}
	}
	return pes;
}

/*
 * Fill in the apinfo header
 */
static void _build_header(pals_header_t *hdr, const stepd_step_rec_t *job)
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
	hdr->ncmds = 1;
	offset += hdr->cmd_size * hdr->ncmds;

	hdr->pe_size = sizeof(pals_pe_t);
	hdr->pe_offset = offset;
	hdr->npes = job->ntasks;
	offset += hdr->pe_size * hdr->npes;

	hdr->node_size = sizeof(pals_node_t);
	hdr->node_offset = offset;
	hdr->nnodes = job->nnodes;
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
static int _write_pals_nodes(const stepd_step_rec_t *job, int fd)
{
	hostlist_t hl;
	char *host;
	pals_node_t node;

	memset(&node, 0, sizeof(pals_node_t));

	hl = hostlist_create(job->msg->complete_nodelist);
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
	pals_cmd_t cmd;
	pals_pe_t *pes = NULL;

	// Make sure the application spool directory has been created
	if (appdir == NULL) {
		return SLURM_ERROR;
	}

	// Get information to write
	_build_header(&hdr, job);

	memset(&cmd, 0, sizeof(pals_cmd_t));
	cmd.npes = job->ntasks;
	cmd.pes_per_node = job->ntasks / job->nnodes;
	cmd.cpus_per_pe = job->cpus_per_task;

	pes = _setup_pals_pes(job);
	if (pes == NULL) {
		return SLURM_ERROR;
	}

	// Create the file
	fd = _open_apinfo(job);
	if (fd == -1) {
		xfree(pes);
		return SLURM_ERROR;
	}

	// Write info
	if (_writen(fd, &hdr, sizeof(pals_header_t)) == SLURM_ERROR ||
	    _writen(fd, &cmd, sizeof(pals_cmd_t)) == SLURM_ERROR ||
	    _writen(fd, pes, job->ntasks * sizeof(pals_pe_t)) ==
		    SLURM_ERROR ||
	    _write_pals_nodes(job, fd) == SLURM_ERROR) {
		xfree(pes);
		close(fd);
		return SLURM_ERROR;
	}
	// TODO: Write communication profiles
	// TODO write nics

	// Flush changes to disk
	if (fsync(fd) == -1) {
		error("mpi/cray_shasta: Couldn't sync %s to disk: %m", apinfo);
		xfree(pes);
		close(fd);
		return SLURM_ERROR;
	}

	debug("mpi/cray_shasta: Wrote apinfo file %s", apinfo);

	// Clean up and return
	xfree(pes);
	close(fd);
	return SLURM_SUCCESS;
}
