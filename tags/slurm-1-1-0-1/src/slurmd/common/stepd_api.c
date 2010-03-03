/*****************************************************************************\
 *  src/slurmd/common/stepd_api.c - slurmstepd message API
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2005 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Christopher Morrone <morrone2@llnl.gov>
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

#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>
#include <regex.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/common/pack.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_cred.h"
#include "src/common/slurm_jobacct.h"
#include "src/common/list.h"
#include "src/common/slurm_protocol_api.h"
#include "src/slurmd/common/stepd_api.h"

/*
 * Should be called when a connect() to a socket returns ECONNREFUSED.
 * Presumably the ECONNREFUSED means that nothing is attached to the listening
 * side of the unix domain socket.
 * If the socket is at least five minutes old, go ahead an unlink it.
 */
static void
_handle_stray_socket(const char *socket_name)
{
	struct stat buf;
	uid_t uid;
	time_t now;

	if (stat(socket_name, &buf) == -1) {
		debug3("_handle_stray_socket: unable to stat %s: %m",
			socket_name);
		return;
	}

	if ((uid = getuid()) != buf.st_uid) {
		debug3("_handle_stray_socket: socket %s is not owned by uid %d",
		       uid);
		return;
	}

	now = time(NULL);
	if ((now-buf.st_mtime) > 300) {
		/* remove the socket */
		if (unlink(socket_name) == -1) {
			if (errno != ENOENT) {
				error("_handle_stray_socket: unable to clean up"
				      " stray socket %s: %m", socket_name);
			}
		} else {
			debug("Cleaned up stray socket %s", socket_name);
		}
	}
}

static int
_step_connect(char *directory, char *nodename, uint32_t jobid, uint32_t stepid)
{
	int fd;
	int len;
	struct sockaddr_un addr;
	char *name = NULL;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		debug("_step_connect: socket: %m");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	xstrfmtcat(name, "%s/%s_%u.%u", directory, nodename, jobid, stepid);
	strcpy(addr.sun_path, name);
	len = strlen(addr.sun_path)+1 + sizeof(addr.sun_family);

	if (connect(fd, (struct sockaddr *) &addr, len) < 0) {
		if (errno == ECONNREFUSED) {
			_handle_stray_socket(name);
		} else {
			debug("_step_connect: connect: %m");
		}
		xfree(name);
		close(fd);
		return -1;
	}

	xfree(name);
	return fd;
}


/*
 * Connect to a slurmstepd proccess by way of its unix domain socket.
 *
 * Returns a socket descriptor for the opened socket on success, 
 * and -1 on error.
 */
int
stepd_connect(char *directory, char *nodename, uint32_t jobid, uint32_t stepid)
{
	int req = REQUEST_CONNECT;
	int fd = -1;
	int rc;
	void *auth_cred;
	Buf buffer;
	int len;

	buffer = init_buf(0);
	/* Create an auth credential */
	auth_cred = g_slurm_auth_create(NULL, 2);
	if (auth_cred == NULL) {
		error("Creating authentication credential: %s",
		      g_slurm_auth_errstr(g_slurm_auth_errno(NULL)));
		slurm_seterrno(SLURM_PROTOCOL_AUTHENTICATION_ERROR);
		goto fail1;
	}

	/* Pack the auth credential */
	rc = g_slurm_auth_pack(auth_cred, buffer);
	(void) g_slurm_auth_destroy(auth_cred);
	if (rc) {
		error("Packing authentication credential: %s",
		      g_slurm_auth_errstr(g_slurm_auth_errno(auth_cred)));
		slurm_seterrno(SLURM_PROTOCOL_AUTHENTICATION_ERROR);
		goto fail1;
	}

	/* Connect to the step */
	fd = _step_connect(directory, nodename, jobid, stepid);
	if (fd == -1)
		goto fail1;

	safe_write(fd, &req, sizeof(int));
	len = size_buf(buffer);
	safe_write(fd, &len, sizeof(int));
	safe_write(fd, get_buf_data(buffer), len);

	safe_read(fd, &rc, sizeof(int));
	if (rc < 0) {
		error("slurmstepd refused authentication: %m");
		slurm_seterrno(SLURM_PROTOCOL_AUTHENTICATION_ERROR);
		goto rwfail;
	}

	free_buf(buffer);
	return fd;

rwfail:
	close(fd);
fail1:
	free_buf(buffer);
	return -1;
}


/*
 * Retrieve a job step's current state.
 */
slurmstepd_state_t
stepd_state(int fd)
{
	int req	= REQUEST_STATE;
	slurmstepd_state_t status = SLURMSTEPD_NOT_RUNNING;

	safe_write(fd, &req, sizeof(int));
	safe_read(fd, &status, sizeof(slurmstepd_state_t));
rwfail:
	return status;
}

/*
 * Retrieve slurmstepd_info_t structure for a job step.
 *
 * Must be xfree'd by the caller.
 */
slurmstepd_info_t *
stepd_get_info(int fd)
{
	int req = REQUEST_INFO;
	slurmstepd_info_t *info;

	info = xmalloc(sizeof(slurmstepd_info_t));
	safe_write(fd, &req, sizeof(int));
	safe_read(fd, &info->uid, sizeof(uid_t));
	safe_read(fd, &info->jobid, sizeof(uint32_t));
	safe_read(fd, &info->stepid, sizeof(uint32_t));

	return info;
rwfail:
	return NULL;
}

/*
 * Send a signal to the process group of a job step.
 */
int
stepd_signal(int fd, int signal)
{
	int req = REQUEST_SIGNAL_PROCESS_GROUP;
	int rc;

	safe_write(fd, &req, sizeof(int));
	safe_write(fd, &signal, sizeof(int));

	/* Receive the return code */
	safe_read(fd, &rc, sizeof(int));
	return rc;
rwfail:
	return -1;
}

/*
 * Send a signal to a single task in a job step.
 */
int
stepd_signal_task_local(int fd, int signal, int ltaskid)
{
	int req = REQUEST_SIGNAL_TASK_LOCAL;
	int rc;

	safe_write(fd, &req, sizeof(int));
	safe_write(fd, &signal, sizeof(int));
	safe_write(fd, &ltaskid, sizeof(int));

	/* Receive the return code */
	safe_read(fd, &rc, sizeof(int));

	return rc;
rwfail:
	return -1;
}

/*
 * Send a signal to the proctrack container of a job step.
 */
int
stepd_signal_container(int fd, int signal)
{
	int req = REQUEST_SIGNAL_CONTAINER;
	int rc;
	int errnum = 0;

	safe_write(fd, &req, sizeof(int));
	safe_write(fd, &signal, sizeof(int));

	/* Receive the return code and errno */
	safe_read(fd, &rc, sizeof(int));
	safe_read(fd, &errnum, sizeof(int));

	errno = errnum;
	return rc;
rwfail:
	return -1;
}


/*
 * Attach a client to a running job step.
 *
 * On success returns SLURM_SUCCESS and fills in resp->local_pids,
 * resp->gtids, resp->ntasks, and resp->executable.
 */
int
stepd_attach(int fd, slurm_addr *ioaddr, slurm_addr *respaddr,
	     void *job_cred_sig, reattach_tasks_response_msg_t *resp)
{
	int req = REQUEST_ATTACH;
	int rc = SLURM_SUCCESS;

	safe_write(fd, &req, sizeof(int));
	safe_write(fd, ioaddr, sizeof(slurm_addr));
	safe_write(fd, respaddr, sizeof(slurm_addr));
	safe_write(fd, job_cred_sig, SLURM_CRED_SIGLEN);

	/* Receive the return code */
	safe_read(fd, &rc, sizeof(int));

	if (rc == SLURM_SUCCESS) {
		/* Receive response info */
		uint32_t ntasks;
		int len;

		safe_read(fd, &ntasks, sizeof(uint32_t));
		resp->ntasks = ntasks;
		len = ntasks * sizeof(uint32_t);

		resp->local_pids = xmalloc(len);
		safe_read(fd, resp->local_pids, len);

		resp->gtids = xmalloc(len);
		safe_read(fd, resp->gtids, len);

		safe_read(fd, &len, sizeof(int));
		resp->executable_name = xmalloc(len);
		safe_read(fd, resp->executable_name, len);
	}

	return rc;
rwfail:
	return SLURM_ERROR;
}

static void
_free_step_loc_t(step_loc_t *loc)
{
	if (loc->directory)
		xfree(loc->directory);
	if (loc->nodename)
		xfree(loc->nodename);
	xfree(loc);
}

static int
_sockname_regex_init(regex_t *re, const char *nodename)
{
	char *pattern = NULL;

	xstrcat(pattern, "^");
	xstrcat(pattern, nodename);
	xstrcat(pattern, "_([[:digit:]]*)\\.([[:digit:]]*)$");

	if (regcomp(re, pattern, REG_EXTENDED) != 0) {
                error("sockname regex compilation failed\n");
                return -1;
        }

	xfree(pattern);

	return 0;
}

static int
_sockname_regex(regex_t *re, const char *filename,
		uint32_t *jobid, uint32_t *stepid)
{
        size_t nmatch = 5;
        regmatch_t pmatch[5];
        char *match;

	memset(pmatch, 0, sizeof(regmatch_t)*nmatch);
	if (regexec(re, filename, nmatch, pmatch, 0) == REG_NOMATCH) {
		return -1;
	}

	match = strndup(filename + pmatch[1].rm_so,
			(size_t)(pmatch[1].rm_eo - pmatch[1].rm_so));
	*jobid = (uint32_t)atoll(match);
	free(match);

	match = strndup(filename + pmatch[2].rm_so,
			(size_t)(pmatch[2].rm_eo - pmatch[2].rm_so));
	*stepid = (uint32_t)atoll(match);
	free(match);

	return 0;
}		     

/*
 * Scan for available running slurm step daemons by checking
 * "directory" for unix domain sockets with names beginning in "nodename".
 *
 * Returns a List of pointers to step_loc_t structures.
 */
List
stepd_available(const char *directory, const char *nodename)
{
	List l;
	DIR *dp;
	struct dirent *ent;
	regex_t re;
	struct stat stat_buf;

	l = list_create((ListDelF) _free_step_loc_t);
	_sockname_regex_init(&re, nodename);

	/*
	 * Make sure that "directory" exists and is a directory.
	 */
	if (stat(directory, &stat_buf) < 0) {
		error("Domain socket directory %s: %m", directory);
		goto done;
	} else if (!S_ISDIR(stat_buf.st_mode)) {
		error("%s is not a directory", directory);
		goto done;
	}

	if ((dp = opendir(directory)) == NULL) {
		error("Unable to open directory: %m");
		goto done;
	}

	while ((ent = readdir(dp)) != NULL) {
		step_loc_t *loc;
		uint32_t jobid, stepid;

		if (_sockname_regex(&re, ent->d_name, &jobid, &stepid) == 0) {
			debug4("found jobid = %u, stepid = %u", jobid, stepid);
			loc = xmalloc(sizeof(step_loc_t));
			loc->directory = xstrdup(directory);
			loc->nodename = xstrdup(nodename);
			loc->jobid = jobid;
			loc->stepid = stepid;
			list_append(l, (void *)loc);
		}
	}

	closedir(dp);
done:
	regfree(&re);
	return l;
}

/*
 * Send the termination signal to all of the unix domain socket files
 * for a given directory and nodename, and then unlink the files.
 * Returns SLURM_ERROR if any sockets could not be unlinked.
 */
int
stepd_cleanup_sockets(const char *directory, const char *nodename)
{
	DIR *dp;
	struct dirent *ent;
	regex_t re;
	struct stat stat_buf;
	int rc = SLURM_SUCCESS;

	_sockname_regex_init(&re, nodename);

	/*
	 * Make sure that "directory" exists and is a directory.
	 */
	if (stat(directory, &stat_buf) < 0) {
		error("Domain socket directory %s: %m", directory);
		goto done;
	} else if (!S_ISDIR(stat_buf.st_mode)) {
		error("%s is not a directory", directory);
		goto done;
	}

	if ((dp = opendir(directory)) == NULL) {
		error("Unable to open directory: %m");
		goto done;
	}

	while ((ent = readdir(dp)) != NULL) {
		uint32_t jobid, stepid;
		if (_sockname_regex(&re, ent->d_name, &jobid, &stepid) == 0) {
			char *path;
			int fd;

			path = NULL;
			xstrfmtcat(path, "%s/%s", directory, ent->d_name);
			verbose("Cleaning up stray job step %u.%u", 
				jobid, stepid);

			/* signal the slurmstepd to terminate its step */
			fd = stepd_connect((char *) directory, (char *) nodename, 
					jobid, stepid);
			if (fd == -1) {
				debug("Unable to connect to socket %s", path);
			} else {
				stepd_signal_container(fd, SIGKILL);
				close(fd);
			}

			/* make sure that the socket has been removed */
			if (unlink(path) == -1 && errno != ENOENT) {
				error("Unable to clean up stray socket %s: %m",
				      path);
				rc = SLURM_ERROR;
			}
			xfree(path);
		}
	}

	closedir(dp);
done:
	regfree(&re);
	return rc;
}

/*
 * Return true if the process with process ID "pid" is found in
 * the proctrack container of the slurmstepd "step".
 */
bool
stepd_pid_in_container(int fd, pid_t pid)
{
	int req = REQUEST_PID_IN_CONTAINER;
	bool rc;

	safe_write(fd, &req, sizeof(int));
	safe_write(fd, &pid, sizeof(pid_t));

	/* Receive the return code */
	safe_read(fd, &rc, sizeof(bool));

	debug("Leaving stepd_pid_in_container");
	return rc;
rwfail:
	return false;
}

/*
 * Return the process ID of the slurmstepd.
 */
pid_t
stepd_daemon_pid(int fd)
{
	int req	= REQUEST_DAEMON_PID;
	pid_t pid;

	safe_write(fd, &req, sizeof(int));
	safe_read(fd, &pid, sizeof(pid_t));

	return pid;
rwfail:
	return (pid_t)-1;
}

/*
 * Suspend execution of the job step.  Only root or SlurmUser is
 * authorized to use this call.
 *
 * Returns SLURM_SUCCESS is successful.  On error returns SLURM_ERROR
 * and sets errno.
 */
int
stepd_suspend(int fd)
{
	int req = REQUEST_STEP_SUSPEND;
	int rc;
	int errnum = 0;

	safe_write(fd, &req, sizeof(int));

	/* Receive the return code and errno */
	safe_read(fd, &rc, sizeof(int));
	safe_read(fd, &errnum, sizeof(int));

	errno = errnum;
	return rc;
rwfail:
	return -1;
}

/*
 * Resume execution of the job step that has been suspended by a
 * call to stepd_suspend().  Only root or SlurmUser is
 * authorized to use this call.
 *
 * Returns SLURM_SUCCESS is successful.  On error returns SLURM_ERROR
 * and sets errno.
 */
int
stepd_resume(int fd)
{
	int req = REQUEST_STEP_RESUME;
	int rc;
	int errnum = 0;

	safe_write(fd, &req, sizeof(int));

	/* Receive the return code and errno */
	safe_read(fd, &rc, sizeof(int));
	safe_read(fd, &errnum, sizeof(int));

	errno = errnum;
	return rc;
rwfail:
	return -1;
}

/*
 * Terminate the job step.
 *
 * Returns SLURM_SUCCESS is successful.  On error returns SLURM_ERROR
 * and sets errno.
 */
int
stepd_terminate(int fd)
{
	int req = REQUEST_STEP_TERMINATE;
	int rc;
	int errnum = 0;

	safe_write(fd, &req, sizeof(int));

	/* Receive the return code and errno */
	safe_read(fd, &rc, sizeof(int));
	safe_read(fd, &errnum, sizeof(int));

	errno = errnum;
	return rc;
rwfail:
	return -1;
}

/*
 *
 * Returns SLURM_SUCCESS is successful.  On error returns SLURM_ERROR
 * and sets errno.
 */
int
stepd_completion(int fd, step_complete_msg_t *sent)
{
	int req = REQUEST_STEP_COMPLETION;
	int rc;
	int errnum = 0;

	debug("Entering stepd_completion, range_first = %d, range_last = %d",
	      sent->range_first, sent->range_last);
	safe_write(fd, &req, sizeof(int));
	safe_write(fd, &sent->range_first, sizeof(int));
	safe_write(fd, &sent->range_last, sizeof(int));
	safe_write(fd, &sent->step_rc, sizeof(int));
	jobacct_g_setinfo(sent->jobacct, JOBACCT_DATA_PIPE, &fd);	
	/* Receive the return code and errno */
	safe_read(fd, &rc, sizeof(int));
	safe_read(fd, &errnum, sizeof(int));

	errno = errnum;
	return rc;
rwfail:
	return -1;
}

/*
 *
 * Returns jobacctinfo_t struct on success, NULL if error.  
 * jobacctinfo_t must be freed after calling this function.
 */
int 
stepd_stat_jobacct(int fd, stat_jobacct_msg_t *sent, stat_jobacct_msg_t *resp)
{
	int req = MESSAGE_STAT_JOBACCT;
	int rc = SLURM_SUCCESS;
	//jobacctinfo_t *jobacct = NULL;
	int tasks = 0;
	debug("Entering stepd_stat_jobacct for job %u.%u", 
	      sent->job_id, sent->step_id);
	safe_write(fd, &req, sizeof(int));
	
	/* Receive the jobacct struct and return */
	resp->jobacct = jobacct_g_alloc((uint16_t)NO_VAL);
	
	rc = jobacct_g_getinfo(resp->jobacct, JOBACCT_DATA_PIPE, &fd);	
	safe_read(fd, &tasks, sizeof(int));
	resp->num_tasks = tasks;
	return rc;
rwfail:
	error("an error occured %d", rc);
	jobacct_g_free(resp->jobacct);
	resp->jobacct = NULL;
	return rc;
}
