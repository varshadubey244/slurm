/*****************************************************************************\
 *  job_info.c - get/print the job state information of slurm
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002-2006 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov> et. al.
 *  UCRL-CODE-217948.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include <slurm/slurm.h>
#include <slurm/slurm_errno.h>

#include "src/api/job_info.h"
#include "src/common/node_select.h"
#include "src/common/parse_time.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/uid.h"
#include "src/common/xstring.h"
#include "src/common/forward.h"

/*
 * slurm_print_job_info_msg - output information about all Slurm 
 *	jobs based upon message as loaded using slurm_load_jobs
 * IN out - file to write to
 * IN job_info_msg_ptr - job information message pointer
 * IN one_liner - print as a single line if true
 */
extern void 
slurm_print_job_info_msg ( FILE* out, job_info_msg_t *jinfo, int one_liner )
{
	int i;
	job_info_t *job_ptr = jinfo->job_array;
	char time_str[32];

	slurm_make_time_str ((time_t *)&jinfo->last_update, time_str, 
		sizeof(time_str));
	fprintf( out, "Job data as of %s, record count %d\n",
		 time_str, jinfo->record_count);

	for (i = 0; i < jinfo->record_count; i++) 
		slurm_print_job_info(out, &job_ptr[i], one_liner);
}

/*
 * slurm_print_job_info - output information about a specific Slurm 
 *	job based upon message as loaded using slurm_load_jobs
 * IN out - file to write to
 * IN job_ptr - an individual job information record pointer
 * IN one_liner - print as a single line if true
 */
extern void
slurm_print_job_info ( FILE* out, job_info_t * job_ptr, int one_liner )
{
	char *print_this = slurm_sprint_job_info(job_ptr, one_liner);
	fprintf ( out, "%s", print_this);
	xfree(print_this);
}

/*
 * slurm_sprint_job_info - output information about a specific Slurm 
 *	job based upon message as loaded using slurm_load_jobs
 * IN job_ptr - an individual job information record pointer
 * IN one_liner - print as a single line if true
 * RET out - char * containing formatted output (must be freed after call)
 *           NULL is returned on failure.
 */
extern char *
slurm_sprint_job_info ( job_info_t * job_ptr, int one_liner )
{
	int j;
	char time_str[32], select_buf[128];
	struct group *group_info = NULL;
	char tmp1[7], tmp2[7], tmp3[7];
	char tmp_line[128];
	uint16_t quarter = (uint16_t) NO_VAL;
	uint16_t nodecard = (uint16_t) NO_VAL;
	char *out = NULL;
	
#ifdef HAVE_BG
	char *nodelist = "BP_List";
	select_g_get_jobinfo(job_ptr->select_jobinfo, 
			     SELECT_DATA_QUARTER, 
			     &quarter);
	select_g_get_jobinfo(job_ptr->select_jobinfo, 
			     SELECT_DATA_NODECARD, 
			     &nodecard);
#else
	char *nodelist = "NodeList";
#endif	

	/****** Line 1 ******/
	snprintf(tmp_line, sizeof(tmp_line), 
		"JobId=%u UserId=%s(%u) ", 
		job_ptr->job_id, 
		uid_to_string((uid_t) job_ptr->user_id), job_ptr->user_id);
	out = xstrdup(tmp_line);
	group_info = getgrgid((gid_t) job_ptr->group_id );
	if ( group_info && group_info->gr_name[ 0 ] ) {
		snprintf(tmp_line, sizeof(tmp_line), "GroupId=%s(%u)",
			 group_info->gr_name, job_ptr->group_id );
	} else {
		snprintf(tmp_line, sizeof(tmp_line), "GroupId=(%u)", 
			job_ptr->group_id );
	}
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 2 ******/
	snprintf(tmp_line, sizeof(tmp_line), "Name=%s", job_ptr->name);
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 3 ******/
	snprintf(tmp_line, sizeof(tmp_line), 
		"Priority=%u Partition=%s BatchFlag=%u", 
		job_ptr->priority, job_ptr->partition, 
		job_ptr->batch_flag);
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 4 ******/
	snprintf(tmp_line, sizeof(tmp_line),
		"AllocNode:Sid=%s:%u TimeLimit=", 
		job_ptr->alloc_node, job_ptr->alloc_sid);
	xstrcat(out, tmp_line);
	if (job_ptr->time_limit == INFINITE)
		sprintf(tmp_line, "UNLIMITED");
	else if (job_ptr->time_limit == NO_VAL)
		sprintf(tmp_line, "Partition_Limit");
	else
		sprintf(tmp_line, "%u", job_ptr->time_limit);
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 5 ******/
	slurm_make_time_str((time_t *)&job_ptr->start_time, time_str,
		sizeof(time_str));
	snprintf(tmp_line, sizeof(tmp_line),
		"JobState=%s StartTime=%s EndTime=",
		job_state_string(job_ptr->job_state), time_str);
	xstrcat(out, tmp_line);
	if ((job_ptr->time_limit == INFINITE) && 
	    (job_ptr->end_time > time(NULL)))
		sprintf(tmp_line, "NONE");
	else {
		slurm_make_time_str ((time_t *)&job_ptr->end_time, time_str,
			sizeof(time_str));
		sprintf(tmp_line, "%s", time_str);
	}
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 6 ******/
	snprintf(tmp_line, sizeof(tmp_line), "%s=%s", nodelist, job_ptr->nodes);
	xstrcat(out, tmp_line);
	if(job_ptr->nodes) {
		if(quarter != (uint16_t) NO_VAL) {
			if(nodecard != (uint16_t) NO_VAL) 
				sprintf(tmp_line, ".%d.%d", quarter, nodecard);
			else
				sprintf(tmp_line, ".%d", quarter);
			xstrcat(out, tmp_line);
		} 
	}
	sprintf(tmp_line, " %sIndices=", nodelist);
	xstrcat(out, tmp_line);
	for (j = 0; job_ptr->node_inx; j++) {
		if (j > 0)
			sprintf(tmp_line, ",%d", job_ptr->node_inx[j]);
		else
			sprintf(tmp_line, "%d", job_ptr->node_inx[j]);
		xstrcat(out, tmp_line);
		if (job_ptr->node_inx[j] == -1)
			break;
	}
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 7 ******/
	convert_num_unit((float)job_ptr->num_procs, tmp1, UNIT_NONE);
	convert_num_unit((float)job_ptr->num_nodes, tmp3, UNIT_NONE);
#ifdef HAVE_BG
	sprintf(tmp_line, "ReqProcs=%s MinBPs=%s ", tmp1, tmp3);
#else
	sprintf(tmp_line, "ReqProcs=%s MinNodes=%s ", tmp1, tmp3);
#endif
	xstrcat(out, tmp_line);
	convert_num_unit((float)job_ptr->shared, tmp1, UNIT_NONE);
	convert_num_unit((float)job_ptr->contiguous, tmp2, UNIT_NONE);
	convert_num_unit((float)job_ptr->cpus_per_task, tmp3, UNIT_NONE);
	snprintf(tmp_line, sizeof(tmp_line),
		"Shared=%s Contiguous=%s CPUs/task=%s", tmp1, tmp2, tmp3);
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 8 ******/
	convert_num_unit((float)job_ptr->min_procs, tmp1, UNIT_NONE);
	convert_num_unit((float)job_ptr->min_memory, tmp2, UNIT_NONE);
	convert_num_unit((float)job_ptr->min_tmp_disk, tmp3, UNIT_NONE);
	snprintf(tmp_line, sizeof(tmp_line), 
		"MinProcs=%s MinMemory=%s Features=%s MinTmpDisk=%s", 
		tmp1, tmp2, job_ptr->features, tmp3);
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 9 ******/
	snprintf(tmp_line, sizeof(tmp_line), 
		"Dependency=%u Account=%s Reason=%s Network=%s",
		job_ptr->dependency, job_ptr->account,
		job_reason_string(job_ptr->wait_reason), job_ptr->network);
	xstrcat(out, tmp_line);
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 10 ******/
	snprintf(tmp_line, sizeof(tmp_line), "Req%s=%s Req%sIndices=", 
		nodelist, job_ptr->req_nodes, nodelist);
	xstrcat(out, tmp_line);
	for (j = 0; job_ptr->req_node_inx; j++) {
		if (j > 0)
			sprintf(tmp_line, ",%d", job_ptr->req_node_inx[j]);
		else
			sprintf(tmp_line, "%d", job_ptr->req_node_inx[j]);
		xstrcat(out, tmp_line);
		if (job_ptr->req_node_inx[j] == -1)
			break;
	}
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 11 ******/
	snprintf(tmp_line, sizeof(tmp_line), "Exc%s=%s Exc%sIndices=", 
		nodelist, job_ptr->exc_nodes, nodelist);
	xstrcat(out, tmp_line);
	for (j = 0; job_ptr->exc_node_inx; j++) {
		if (j > 0)
			sprintf(tmp_line, ",%d", job_ptr->exc_node_inx[j]);
		else
			sprintf(tmp_line, "%d", job_ptr->exc_node_inx[j]);
		xstrcat(out, tmp_line);
		if (job_ptr->exc_node_inx[j] == -1)
			break;
	}
	if (one_liner)
		xstrcat(out, " ");
	else
		xstrcat(out, "\n   ");

	/****** Line 12 ******/
	slurm_make_time_str((time_t *)&job_ptr->submit_time, time_str, 
		sizeof(time_str));
	sprintf(tmp_line, "SubmitTime=%s ", time_str);
	xstrcat(out, tmp_line);
	if (job_ptr->suspend_time) {
		slurm_make_time_str ((time_t *)&job_ptr->suspend_time, 
			time_str, sizeof(time_str));
	} else {
		strncpy(time_str, "None", sizeof(time_str));
	}
	sprintf(tmp_line, "SuspendTime=%s PreSusTime=%ld", 
		  time_str, (long int)job_ptr->pre_sus_time);
	xstrcat(out, tmp_line);

	/****** Line 13 (optional) ******/
	select_g_sprint_jobinfo(job_ptr->select_jobinfo,
		select_buf, sizeof(select_buf), SELECT_PRINT_MIXED);
	if (select_buf[0] != '\0') {
		if (one_liner)
			xstrcat(out, " ");
		else
			xstrcat(out, "\n   ");
		xstrcat(out, select_buf);
	}
	xstrcat(out, select_buf);
	xstrcat(out, "\n\n");

	return out;

}
/*
 * slurm_load_jobs - issue RPC to get slurm all job configuration  
 *	information if changed since update_time 
 * IN update_time - time of current configuration data
 * IN job_info_msg_pptr - place to store a job configuration pointer
 * IN show_flags -  job filtering options
 * RET 0 or -1 on error
 * NOTE: free the response using slurm_free_job_info_msg
 */
extern int
slurm_load_jobs (time_t update_time, job_info_msg_t **resp,
		uint16_t show_flags)
{
	int rc;
	slurm_msg_t resp_msg;
	slurm_msg_t req_msg;
	job_info_request_msg_t req;

	req.last_update  = update_time;
	req.show_flags = show_flags;
	req_msg.msg_type = REQUEST_JOB_INFO;
	req_msg.data     = &req;

	if (slurm_send_recv_controller_msg(&req_msg, &resp_msg) < 0)
		return SLURM_ERROR;

	switch (resp_msg.msg_type) {
	case RESPONSE_JOB_INFO:
		*resp = (job_info_msg_t *)resp_msg.data;
		break;
	case RESPONSE_SLURM_RC:
		rc = ((return_code_msg_t *) resp_msg.data)->return_code;
		slurm_free_return_code_msg(resp_msg.data);	
		if (rc) 
			slurm_seterrno_ret(rc);
		break;
	default:
		slurm_seterrno_ret(SLURM_UNEXPECTED_MSG_ERROR);
		break;
	}

	return SLURM_PROTOCOL_SUCCESS ;
}


/*
 * slurm_pid2jobid - issue RPC to get the slurm job_id given a process_id 
 *	on this machine
 * IN job_pid     - process_id of interest on this machine
 * OUT job_id_ptr - place to store a slurm job_id
 * RET 0 or -1 on error
 */
extern int
slurm_pid2jobid (pid_t job_pid, uint32_t *jobid)
{
	int rc;
	slurm_msg_t req_msg;
	slurm_msg_t resp_msg;
	job_id_request_msg_t req;
	List ret_list;

	memset(&req_msg, 0, sizeof(slurm_msg_t));
	memset(&resp_msg, 0, sizeof(slurm_msg_t));

	/*
	 *  Set request message address to slurmd on localhost
	 */
	slurm_set_addr(&req_msg.address, (uint16_t)slurm_get_slurmd_port(), 
		       "localhost");

	req.job_pid      = job_pid;
	req_msg.msg_type = REQUEST_JOB_ID;
	req_msg.data     = &req;
	forward_init(&req_msg.forward, NULL);
	req_msg.ret_list = NULL;
	req_msg.orig_addr.sin_addr.s_addr = 0; 
	req_msg.forward_struct_init = 0;
	
	ret_list = slurm_send_recv_node_msg(&req_msg, &resp_msg, 0);

	if(!ret_list || !resp_msg.auth_cred) {
		error("slurm_pid2jobid: %m");
		if(ret_list)
			list_destroy(ret_list);
		if(resp_msg.auth_cred)
			g_slurm_auth_destroy(resp_msg.auth_cred);
		return SLURM_ERROR;
	}
	if(list_count(ret_list)>0) {
		error("slurm_pid2jobid: got %d from receive, expecting 0",
		      list_count(ret_list));
	}
	list_destroy(ret_list);
	g_slurm_auth_destroy(resp_msg.auth_cred);	
	switch (resp_msg.msg_type) {
	case RESPONSE_JOB_ID:
		*jobid = ((job_id_response_msg_t *) resp_msg.data)->job_id;
		slurm_free_job_id_response_msg(resp_msg.data);
		break;
	case RESPONSE_SLURM_RC:
	        rc = ((return_code_msg_t *) resp_msg.data)->return_code;
		slurm_free_return_code_msg(resp_msg.data);	
		if (rc) 
			slurm_seterrno_ret(rc);
		break;
	default:
		slurm_seterrno_ret(SLURM_UNEXPECTED_MSG_ERROR);
		break;
	}

	return SLURM_PROTOCOL_SUCCESS;
}

/*
 * slurm_get_rem_time - get the expected time remaining for a given job
 * IN jobid     - slurm job id
 * RET remaining time in seconds or -1 on error 
 */
extern long slurm_get_rem_time(uint32_t jobid)
{
	time_t now = time(NULL);
	time_t end_time;
	long rc;

	if (slurm_get_end_time(jobid, &end_time) != SLURM_SUCCESS)
		return -1L;

	rc = difftime(end_time, now);
	if (rc < 0)
		rc = 0L;
	return rc;
}

/*
 * slurm_get_end_time - get the expected end time for a given slurm job
 * IN jobid     - slurm job id
 * end_time_ptr - location in which to store scheduled end time for job 
 * RET 0 or -1 on error
 */
extern int
slurm_get_end_time(uint32_t jobid, time_t *end_time_ptr)
{
	int rc;
	slurm_msg_t resp_msg;
	slurm_msg_t req_msg;
	job_alloc_info_msg_t job_msg;
	srun_timeout_msg_t *timeout_msg;
	time_t now = time(NULL);
	static uint32_t jobid_cache = 0;
	static uint32_t jobid_env = 0;
	static time_t endtime_cache = 0;
	static time_t last_test_time = 0;

	if (!end_time_ptr)
		slurm_seterrno_ret(EINVAL);

	if (jobid == 0) {
		if (jobid_env) {
			jobid = jobid_env;
		} else {
			char *env = getenv("SLURM_JOBID");
			if (env) {
				jobid = (uint32_t) atol(env);
				jobid_env = jobid;
			}
		}
		if (jobid == 0) {
			slurm_seterrno(ESLURM_INVALID_JOB_ID);
			return SLURM_ERROR;
		}
	}

	/* Just use cached data if data less than 60 seconds old */
	if ((jobid == jobid_cache)
	&&  (difftime(now, last_test_time) < 60)) {
		*end_time_ptr  = endtime_cache;
		return SLURM_SUCCESS;
	}

	job_msg.job_id     = jobid;
	req_msg.msg_type   = REQUEST_JOB_END_TIME;
	req_msg.data       = &job_msg;

	if (slurm_send_recv_controller_msg(&req_msg, &resp_msg) < 0)
		return SLURM_ERROR;

	switch (resp_msg.msg_type) {
	case SRUN_TIMEOUT:
		timeout_msg = (srun_timeout_msg_t *) resp_msg.data;
		last_test_time = time(NULL);
		jobid_cache    = jobid;
		endtime_cache  = timeout_msg->timeout;
		*end_time_ptr  = endtime_cache;
		slurm_free_srun_timeout_msg(resp_msg.data);
		break;
	case RESPONSE_SLURM_RC:
		rc = ((return_code_msg_t *) resp_msg.data)->return_code;
		slurm_free_return_code_msg(resp_msg.data);
		if (endtime_cache)
			*end_time_ptr  = endtime_cache;
		else if (rc)
			slurm_seterrno_ret(rc);
		break;
	default:
		if (endtime_cache)
		*end_time_ptr  = endtime_cache;
		else
			slurm_seterrno_ret(SLURM_UNEXPECTED_MSG_ERROR);
		break;
	}

	return SLURM_SUCCESS;
}

/*
 * slurm_get_select_jobinfo - get data from a select job credential
 * IN jobinfo  - updated select job credential
 * IN data_type - type of data to enter into job credential
 * IN/OUT data - the data to enter into job credential
 * RET 0 or -1 on error
 */
extern int slurm_get_select_jobinfo (select_jobinfo_t jobinfo,
		enum select_data_type data_type, void *data)
{
	return select_g_get_jobinfo (jobinfo, data_type, data);
}

/*
 * slurm_job_node_ready - report if nodes are ready for job to execute now
 * IN job_id - slurm job id
 * RET: READY_* values as defined in api/job_info.h
 */
extern int slurm_job_node_ready(uint32_t job_id)
{
	slurm_msg_t req, resp;
	job_id_msg_t msg;
	int rc;

	req.msg_type = REQUEST_JOB_READY;
	req.data     = &msg;
	msg.job_id   = job_id;

	if (slurm_send_recv_controller_msg(&req, &resp) < 0)
		return -1;

	if (resp.msg_type == RESPONSE_JOB_READY) {
		rc = ((return_code_msg_t *) resp.data)->return_code;
		slurm_free_return_code_msg(resp.data);
	} else if (resp.msg_type == RESPONSE_SLURM_RC) {
		int job_rc = ((return_code_msg_t *) resp.data) -> 
				return_code;
		if ((job_rc == ESLURM_INVALID_PARTITION_NAME)
		||  (job_rc == ESLURM_INVALID_JOB_ID))
			rc = READY_JOB_FATAL;
		else	/* EAGAIN */
			rc = READY_JOB_ERROR;
		slurm_free_return_code_msg(resp.data);
	} else
		rc = READY_JOB_ERROR;

	return rc;
}

