/*****************************************************************************\
 *  job_submit_lua.c - Set defaults in job submit request specifications.
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Portions Copyright (C) 2010-2015 SchedMD LLC <http://www.schedmd.com>.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#   include "config.h"
#endif

#if HAVE_STDINT_H
#  include <stdint.h>
#endif
#if HAVE_INTTYPES_H
#  include <inttypes.h>
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/slurm_xlator.h"
#include "src/common/assoc_mgr.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/reservation.h"

#define _DEBUG 0
#define MIN_ACCTG_FREQUENCY 30

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "auth" for SLURM authentication) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will
 * only load authentication plugins if the plugin_type string has a prefix
 * of "auth/".
 *
 * plugin_version   - specifies the version number of the plugin.
 * min_plug_version - specifies the minumum version number of incoming
 *                    messages that this plugin can accept
 */
const char plugin_name[]       	= "Job submit lua plugin";
const char plugin_type[]       	= "job_submit/lua";
const uint32_t plugin_version   = 110;
const uint32_t min_plug_version = 100;

static const char lua_script_path[] = DEFAULT_SCRIPT_DIR "/job_submit.lua";
static time_t lua_script_last_loaded = (time_t) 0;
static lua_State *L = NULL;
static char *user_msg = NULL;

time_t last_lua_jobs_update = (time_t) 0;
time_t last_lua_resv_update = (time_t) 0;

/*
 *  Mutex for protecting multi-threaded access to this plugin.
 *   (Only 1 thread at a time should be in here)
 */
#ifdef WITH_PTHREADS
static pthread_mutex_t lua_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/*****************************************************************************\
 * We've provided a simple example of the type of things you can do with this
 * plugin. If you develop another plugin that may be of interest to others
 * please post it to slurm-dev@schedmd.com  Thanks!
\*****************************************************************************/

/* Generic stack dump function for debugging purposes */
static void _stack_dump (char *header, lua_State *L)
{
#if _DEBUG
	int i;
	int top = lua_gettop(L);

	info("%s: dumping job_submit/lua stack, %d elements", header, top);
	for (i = 1; i <= top; i++) {  /* repeat for each level */
		int type = lua_type(L, i);
		switch (type) {
			case LUA_TSTRING:
				info("string[%d]:%s", i, lua_tostring(L, i));
				break;
			case LUA_TBOOLEAN:
				info("boolean[%d]:%s", i,
				     lua_toboolean(L, i) ? "true" : "false");
				break;
			case LUA_TNUMBER:
				info("number[%d]:%d", i,
				     (int) lua_tonumber(L, i));
				break;
			default:
				info("other[%d]:%s", i, lua_typename(L, type));
				break;
		}
	}
#endif
}

/*
 *  Lua interface to SLURM log facility:
 */
static int _log_lua_msg (lua_State *L)
{
	const char *prefix  = "job_submit.lua";
	int        level    = 0;
	const char *msg;

	/*
	 *  Optional numeric prefix indicating the log level
	 *  of the message.
	 */

	/*
	 *  Pop message off the lua stack
	 */
	msg = lua_tostring(L, -1);
	lua_pop (L, 1);
	/*
	 *  Pop level off stack:
	 */
	level = (int)lua_tonumber (L, -1);
	lua_pop (L, 1);

	/*
	 *  Call appropriate slurm log function based on log-level argument
	 */
	if (level > 4)
		debug4 ("%s: %s", prefix, msg);
	else if (level == 4)
		debug3 ("%s: %s", prefix, msg);
	else if (level == 3)
		debug2 ("%s: %s", prefix, msg);
	else if (level == 2)
		debug ("%s: %s", prefix, msg);
	else if (level == 1)
		verbose ("%s: %s", prefix, msg);
	else if (level == 0)
		info ("%s: %s", prefix, msg);
	return (0);
}

static int _log_lua_error (lua_State *L)
{
	const char *prefix  = "job_submit.lua";
	const char *msg     = lua_tostring (L, -1);
	error ("%s: %s", prefix, msg);
	return (0);
}

static int _log_lua_user_msg (lua_State *L)
{
	const char *msg = lua_tostring(L, -1);

	xfree(user_msg);
	user_msg = xstrdup(msg);
	return (0);
}

static const struct luaL_Reg slurm_functions [] = {
	{ "log",	_log_lua_msg   },
	{ "error",	_log_lua_error },
	{ "user_msg",	_log_lua_user_msg },
	{ NULL,		NULL        }
};

/* Get the default account for a user (or NULL if not present) */
static char *_get_default_account(uint32_t user_id)
{
	slurmdb_user_rec_t user;

	memset(&user, 0, sizeof(slurmdb_user_rec_t));
	user.uid = user_id;
	if (assoc_mgr_fill_in_user(acct_db_conn,
				   &user, 0, NULL) != SLURM_ERROR) {
		return user.default_acct;
	} else {
		return NULL;
	}
}

/* Get fields in an existing slurmctld job record.
 *
 * This is an incomplete list of job record fields. Add more as needed and
 * send patches to slurm-dev@schedmd.com.
 */
static int _job_rec_field(const struct job_record *job_ptr,
                          const char *name)
{
	if (job_ptr == NULL) {
		error("_job_rec_field: job_ptr is NULL");
		lua_pushnil (L);
	} else if (!strcmp(name, "account")) {
		lua_pushstring (L, job_ptr->account);
	} else if (!strcmp(name, "burst_buffer")) {
		lua_pushstring (L, job_ptr->burst_buffer);
	} else if (!strcmp(name, "comment")) {
		lua_pushstring (L, job_ptr->comment);
	} else if (!strcmp(name, "direct_set_prio")) {
		lua_pushnumber (L, job_ptr->direct_set_prio);
	} else if (!strcmp(name, "gres")) {
		lua_pushstring (L, job_ptr->gres);
	} else if (!strcmp(name, "job_id")) {
		lua_pushnumber (L, job_ptr->job_id);
	} else if (!strcmp(name, "job_state")) {
		lua_pushnumber (L, job_ptr->job_state);
	} else if (!strcmp(name, "licenses")) {
		lua_pushstring (L, job_ptr->licenses);
	} else if (!strcmp(name, "max_cpus")) {
		if (job_ptr->details)
			lua_pushnumber (L, job_ptr->details->max_cpus);
		else
			lua_pushnumber (L, 0);
	} else if (!strcmp(name, "max_nodes")) {
		if (job_ptr->details)
			lua_pushnumber (L, job_ptr->details->max_nodes);
		else
			lua_pushnumber (L, 0);
	} else if (!strcmp(name, "min_cpus")) {
		if (job_ptr->details)
			lua_pushnumber (L, job_ptr->details->min_cpus);
		else
			lua_pushnumber (L, 0);
	} else if (!strcmp(name, "min_nodes")) {
		if (job_ptr->details)
			lua_pushnumber (L, job_ptr->details->min_nodes);
		else
			lua_pushnumber (L, 0);
	} else if (!strcmp(name, "nice")) {
		if (job_ptr->details)
			lua_pushnumber (L, job_ptr->details->nice);
		else
			lua_pushnumber (L, (uint16_t)NO_VAL);
	} else if (!strcmp(name, "partition")) {
		lua_pushstring (L, job_ptr->partition);
	} else if (!strcmp(name, "priority")) {
		lua_pushnumber (L, job_ptr->priority);
	} else if (!strcmp(name, "req_switch")) {
		lua_pushnumber (L, job_ptr->req_switch);
	} else if (!strcmp(name, "time_limit")) {
		lua_pushnumber (L, job_ptr->time_limit);
	} else if (!strcmp(name, "time_min")) {
		lua_pushnumber (L, job_ptr->time_min);
	} else if (!strcmp(name, "wait4switch")) {
		lua_pushnumber (L, job_ptr->wait4switch);
	} else if (!strcmp(name, "wckey")) {
		lua_pushstring (L, job_ptr->wckey);
	} else {
		lua_pushnil (L);
	}

	return 1;
}

static int _get_job_rec_field(lua_State *L)
{
	const struct job_record *job_ptr = lua_touserdata(L, 1);
	const char *name = luaL_checkstring(L, 2);

	return _job_rec_field(job_ptr, name);
}

/* Get fields in an existing slurmctld job_record */
static int _job_rec_field_index(lua_State *L)
{
	const char *name = luaL_checkstring(L, 2);
	struct job_record *job_ptr;

	lua_getmetatable(L, -2);
	lua_getfield(L, -1, "_job_rec_ptr");
	job_ptr = lua_touserdata(L, -1);

	return _job_rec_field(job_ptr, name);
}

/* Get the list of existing slurmctld job records. */
static void _update_jobs_global(void)
{
	char job_id_buf[11]; /* Big enough for a uint32_t */
	ListIterator iter;
	struct job_record *job_ptr;

	if (last_lua_jobs_update >= last_job_update) {
		return;
	}

	lua_getglobal(L, "slurm");
	lua_newtable(L);

	iter = list_iterator_create(job_list);
	while ((job_ptr = (struct job_record *) list_next(iter))) {
		/* Create an empty table, with a metatable that looks up the
		 * data for the individual job.
		 */
		lua_newtable(L);

		lua_newtable(L);
		lua_pushcfunction(L, _job_rec_field_index);
		lua_setfield(L, -2, "__index");
		/* Store the job_record in the metatable, so the index
		 * function knows which job it's getting data for.
		 */
		lua_pushlightuserdata(L, job_ptr);
		lua_setfield(L, -2, "_job_rec_ptr");
		lua_setmetatable(L, -2);

		/* Lua copies passed strings, so we can reuse the buffer. */
		snprintf(job_id_buf, sizeof(job_id_buf),
		         "%d", job_ptr->job_id);
		lua_setfield(L, -2, job_id_buf);
	}
	last_lua_jobs_update = last_job_update;
	list_iterator_destroy(iter);

	lua_setfield(L, -2, "jobs");
	lua_pop(L, 1);
}

static int _resv_field(const slurmctld_resv_t *resv_ptr,
                       const char *name)
{
	if (resv_ptr == NULL) {
		error("_resv_field: resv_ptr is NULL");
		lua_pushnil(L);
	} else if (!strcmp(name, "accounts")) {
		lua_pushstring(L, resv_ptr->accounts);
	} else if (!strcmp(name, "assoc_list")) {
		lua_pushstring(L, resv_ptr->assoc_list);
	} else if (!strcmp(name, "cpu_cnt")) {
		lua_pushnumber(L, resv_ptr->cpu_cnt);
	} else if (!strcmp(name, "duration")) {
		lua_pushnumber(L, resv_ptr->duration);
	} else if (!strcmp(name, "end_time")) {
		lua_pushnumber(L, resv_ptr->end_time);
	} else if (!strcmp(name, "features")) {
		lua_pushstring(L, resv_ptr->features);
	} else if (!strcmp(name, "flags")) {
		lua_pushnumber(L, resv_ptr->flags);
	} else if (!strcmp(name, "full_nodes")) {
		lua_pushboolean(L, resv_ptr->full_nodes);
	} else if (!strcmp(name, "flags_set_node")) {
		lua_pushboolean(L, resv_ptr->flags_set_node);
	} else if (!strcmp(name, "job_pend_cnt")) {
		lua_pushnumber(L, resv_ptr->job_pend_cnt);
	} else if (!strcmp(name, "job_run_cnt")) {
		lua_pushnumber(L, resv_ptr->job_run_cnt);
	} else if (!strcmp(name, "licenses")) {
		lua_pushstring(L, resv_ptr->licenses);
	} else if (!strcmp(name, "node_cnt")) {
		lua_pushnumber(L, resv_ptr->node_cnt);
	} else if (!strcmp(name, "node_list")) {
		lua_pushstring(L, resv_ptr->node_list);
	} else if (!strcmp(name, "partition")) {
		lua_pushstring(L, resv_ptr->partition);
	} else if (!strcmp(name, "start_time")) {
		lua_pushnumber(L, resv_ptr->start_time);
	} else if (!strcmp(name, "users")) {
		lua_pushstring(L, resv_ptr->users);
	} else {
		lua_pushnil(L);
	}

	return 1;
}

/* Get fields in an existing slurmctld reservation record */
static int _resv_field_index(lua_State *L)
{
	const char *name = luaL_checkstring(L, 2);
	slurmctld_resv_t *resv_ptr;

	lua_getmetatable(L, -2);
	lua_getfield(L, -1, "_resv_ptr");
	resv_ptr = lua_touserdata(L, -1);

	return _resv_field(resv_ptr, name);
}

/* Get the list of existing slurmctld reservation records. */
static void _update_resvs_global(void)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;

	if (last_lua_resv_update >= last_resv_update) {
		return;
	}

	lua_getglobal(L, "slurm");
	lua_newtable(L);

	iter = list_iterator_create(resv_list);
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		/* Create an empty table, with a metatable that looks up the
		 * data for the individual reservation.
		 */
		lua_newtable(L);

		lua_newtable(L);
		lua_pushcfunction(L, _resv_field_index);
		lua_setfield(L, -2, "__index");
		/* Store the slurmctld_resv_t in the metatable, so the index
		 * function knows which reservation it's getting data for.
		 */
		lua_pushlightuserdata(L, resv_ptr->name);
		lua_setfield(L, -2, "_resv_ptr");
		lua_setmetatable(L, -2);

		lua_setfield(L, -2, resv_ptr->name);
	}
	last_lua_resv_update = last_resv_update;
	list_iterator_destroy(iter);

	lua_setfield(L, -2, "reservations");
	lua_pop(L, 1);
}

static int _get_job_req_field(const struct job_descriptor *job_desc,
			      const char *name)
{
	if (job_desc == NULL) {
		error("%s: job_desc is NULL", __func__);
		lua_pushnil (L);
	} else if (!strcmp(name, "account")) {
		lua_pushstring (L, job_desc->account);
	} else if (!strcmp(name, "acctg_freq")) {
		lua_pushstring (L, job_desc->acctg_freq);
	} else if (!strcmp(name, "alloc_node")) {
		lua_pushstring (L, job_desc->alloc_node);
	} else if (!strcmp(name, "begin_time")) {
		lua_pushnumber (L, job_desc->begin_time);
	} else if (!strcmp(name, "boards_per_node")) {
		lua_pushnumber (L, job_desc->boards_per_node);
	} else if (!strcmp(name, "burst_buffer")) {
		lua_pushstring (L, job_desc->burst_buffer);
	} else if (!strcmp(name, "clusters")) {
		lua_pushstring (L, job_desc->clusters);
	} else if (!strcmp(name, "comment")) {
		lua_pushstring (L, job_desc->comment);
	} else if (!strcmp(name, "contiguous")) {
		lua_pushnumber (L, job_desc->contiguous);
	} else if (!strcmp(name, "cores_per_socket")) {
		lua_pushnumber (L, job_desc->cores_per_socket);
	} else if (!strcmp(name, "cpu_freq_min")) {
		lua_pushnumber (L, job_desc->cpu_freq_min);
	} else if (!strcmp(name, "cpu_freq_max")) {
		lua_pushnumber (L, job_desc->cpu_freq_max);
	} else if (!strcmp(name, "cpu_freq_gov")) {
		lua_pushnumber (L, job_desc->cpu_freq_gov);
	} else if (!strcmp(name, "cpus_per_task")) {
		lua_pushnumber (L, job_desc->cpus_per_task);
	} else if (!strcmp(name, "default_account")) {
		lua_pushstring (L, _get_default_account(job_desc->user_id));
	} else if (!strcmp(name, "dependency")) {
		lua_pushstring (L, job_desc->dependency);
	} else if (!strcmp(name, "end_time")) {
		lua_pushnumber (L, job_desc->end_time);
	} else if (!strcmp(name, "exc_nodes")) {
		lua_pushstring (L, job_desc->exc_nodes);
	} else if (!strcmp(name, "features")) {
		lua_pushstring (L, job_desc->features);
	} else if (!strcmp(name, "gres")) {
		lua_pushstring (L, job_desc->gres);
	} else if (!strcmp(name, "group_id")) {
		lua_pushnumber (L, job_desc->group_id);
	} else if (!strcmp(name, "licenses")) {
		lua_pushstring (L, job_desc->licenses);
	} else if (!strcmp(name, "max_cpus")) {
		lua_pushnumber (L, job_desc->max_cpus);
	} else if (!strcmp(name, "max_nodes")) {
		lua_pushnumber (L, job_desc->max_nodes);
	} else if (!strcmp(name, "min_cpus")) {
		lua_pushnumber (L, job_desc->min_cpus);
	} else if (!strcmp(name, "min_nodes")) {
		lua_pushnumber (L, job_desc->min_nodes);
	} else if (!strcmp(name, "name")) {
		lua_pushstring (L, job_desc->name);
	} else if (!strcmp(name, "nice")) {
		lua_pushnumber (L, job_desc->nice);
	} else if (!strcmp(name, "ntasks_per_board")) {
		lua_pushnumber (L, job_desc->ntasks_per_board);
	} else if (!strcmp(name, "ntasks_per_core")) {
		lua_pushnumber (L, job_desc->ntasks_per_core);
	} else if (!strcmp(name, "ntasks_per_node")) {
		lua_pushnumber (L, job_desc->ntasks_per_node);
	} else if (!strcmp(name, "ntasks_per_socket")) {
		lua_pushnumber (L, job_desc->ntasks_per_socket);
	} else if (!strcmp(name, "num_tasks")) {
		lua_pushnumber (L, job_desc->num_tasks);
	} else if (!strcmp(name, "partition")) {
		lua_pushstring (L, job_desc->partition);
	} else if (!strcmp(name, "power_flags")) {
		lua_pushnumber (L, job_desc->power_flags);
	} else if (!strcmp(name, "pn_min_cpus")) {
		lua_pushnumber (L, job_desc->pn_min_cpus);
	} else if (!strcmp(name, "pn_min_memory")) {
		lua_pushnumber (L, job_desc->pn_min_memory);
	} else if (!strcmp(name, "pn_min_tmp_disk")) {
		lua_pushnumber (L, job_desc->pn_min_tmp_disk);
	} else if (!strcmp(name, "priority")) {
		lua_pushnumber (L, job_desc->priority);
	} else if (!strcmp(name, "qos")) {
		lua_pushstring (L, job_desc->qos);
	} else if (!strcmp(name, "req_nodes")) {
		lua_pushstring (L, job_desc->req_nodes);
	} else if (!strcmp(name, "req_switch")) {
		lua_pushnumber (L, job_desc->req_switch);
	} else if (!strcmp(name, "requeue")) {
		lua_pushnumber (L, job_desc->requeue);
	} else if (!strcmp(name, "reservation")) {
		lua_pushstring (L, job_desc->reservation);
	} else if (!strcmp(name, "script")) {
		lua_pushstring (L, job_desc->script);
	} else if (!strcmp(name, "shared")) {
		lua_pushnumber (L, job_desc->shared);
	} else if (!strcmp(name, "sicp_mode")) {
		lua_pushnumber (L, job_desc->sicp_mode);
	} else if (!strcmp(name, "sockets_per_board")) {
		lua_pushnumber (L, job_desc->sockets_per_board);
	} else if (!strcmp(name, "sockets_per_node")) {
		lua_pushnumber (L, job_desc->sockets_per_node);
	} else if (!strcmp(name, "std_err")) {
		lua_pushstring (L, job_desc->std_err);
	} else if (!strcmp(name, "std_in")) {
		lua_pushstring (L, job_desc->std_in);
	} else if (!strcmp(name, "std_out")) {
		lua_pushstring (L, job_desc->std_out);
	} else if (!strcmp(name, "threads_per_core")) {
		lua_pushnumber (L, job_desc->threads_per_core);
	} else if (!strcmp(name, "time_limit")) {
		lua_pushnumber (L, job_desc->time_limit);
	} else if (!strcmp(name, "time_min")) {
		lua_pushnumber (L, job_desc->time_min);
	} else if (!strcmp(name, "user_id")) {
		lua_pushnumber (L, job_desc->user_id);
	} else if (!strcmp(name, "wait4switch")) {
		lua_pushnumber (L, job_desc->wait4switch);
	} else if (!strcmp(name, "work_dir")) {
		lua_pushstring (L, job_desc->work_dir);
	} else if (!strcmp(name, "wckey")) {
		lua_pushstring (L, job_desc->wckey);
	} else {
		lua_pushnil (L);
	}

	return 1;
}

/* Get fields in the job request record on job submit or modify */
static int _get_job_req_field_name(lua_State *L)
{
	const struct job_descriptor *job_desc = lua_touserdata(L, 1);
	const char *name = luaL_checkstring(L, 2);

	return _get_job_req_field(job_desc, name);
}

/* Get fields in an existing slurmctld job_descriptor record */
static int _get_job_req_field_index(lua_State *L)
{
	const char *name;
	struct job_descriptor *job_desc;

	name = luaL_checkstring(L, 2);
	lua_getmetatable(L, -2);
	lua_getfield(L, -1, "_job_desc");
	job_desc = lua_touserdata(L, -1);

	return _get_job_req_field(job_desc, name);
}

/* Set fields in the job request structure on job submit or modify */
static int _set_job_req_field(lua_State *L)
{
	const char *name, *value_str;
	struct job_descriptor *job_desc;

	name = luaL_checkstring(L, 2);
	lua_getmetatable(L, -3);
	lua_getfield(L, -1, "_job_desc");
	job_desc = lua_touserdata(L, -1);
	if (job_desc == NULL) {
		error("%s: job_desc is NULL", __func__);
	} else if (!strcmp(name, "account")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->account);
		if (strlen(value_str))
			job_desc->account = xstrdup(value_str);
	} else if (!strcmp(name, "acctg_freq")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->acctg_freq);
		if (strlen(value_str))
			job_desc->acctg_freq = xstrdup(value_str);
	} else if (!strcmp(name, "begin_time")) {
		job_desc->begin_time = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "burst_buffer")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->burst_buffer);
		if (strlen(value_str))
			job_desc->burst_buffer = xstrdup(value_str);
	} else if (!strcmp(name, "clusters")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->clusters);
		if (strlen(value_str))
			job_desc->clusters = xstrdup(value_str);
	} else if (!strcmp(name, "comment")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->comment);
		if (strlen(value_str))
			job_desc->comment = xstrdup(value_str);
	} else if (!strcmp(name, "contiguous")) {
		job_desc->contiguous = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "cores_per_socket")) {
		job_desc->cores_per_socket = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "cpus_per_task")) {
		job_desc->cpus_per_task = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "cpu_freq_min")) {
		job_desc->cpu_freq_min = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "cpu_freq_max")) {
		job_desc->cpu_freq_max = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "cpu_freq_gov")) {
		job_desc->cpu_freq_gov = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "dependency")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->dependency);
		if (strlen(value_str))
			job_desc->dependency = xstrdup(value_str);
	} else if (!strcmp(name, "end_time")) {
		job_desc->end_time = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "exc_nodes")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->exc_nodes);
		if (strlen(value_str))
			job_desc->exc_nodes = xstrdup(value_str);
	} else if (!strcmp(name, "features")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->features);
		if (strlen(value_str))
			job_desc->features = xstrdup(value_str);
	} else if (!strcmp(name, "gres")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->gres);
		if (strlen(value_str))
			job_desc->gres = xstrdup(value_str);
	} else if (!strcmp(name, "licenses")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->licenses);
		if (strlen(value_str))
			job_desc->licenses = xstrdup(value_str);
	} else if (!strcmp(name, "max_cpus")) {
		job_desc->max_cpus = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "max_nodes")) {
		job_desc->max_nodes = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "min_cpus")) {
		job_desc->min_cpus = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "min_nodes")) {
		job_desc->min_nodes = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "name")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->name);
		if (strlen(value_str))
			job_desc->name = xstrdup(value_str);
	} else if (!strcmp(name, "nice")) {
		job_desc->nice = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "ntasks_per_node")) {
		job_desc->ntasks_per_node = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "ntasks_per_socket")) {
		job_desc->ntasks_per_socket = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "num_tasks")) {
		job_desc->num_tasks = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "partition")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->partition);
		if (strlen(value_str))
			job_desc->partition = xstrdup(value_str);
	} else if (!strcmp(name, "power_flags")) {
		job_desc->power_flags = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "pn_min_cpus")) {
		job_desc->pn_min_cpus = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "pn_min_memory")) {
		job_desc->pn_min_memory = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "pn_min_tmp_disk")) {
		job_desc->pn_min_tmp_disk = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "priority")) {
		job_desc->priority = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "qos")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->qos);
		if (strlen(value_str))
			job_desc->qos = xstrdup(value_str);
	} else if (!strcmp(name, "req_nodes")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->req_nodes);
		if (strlen(value_str))
			job_desc->req_nodes = xstrdup(value_str);
	} else if (!strcmp(name, "req_switch")) {
		job_desc->req_switch = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "requeue")) {
		job_desc->requeue = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "reservation")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->reservation);
		if (strlen(value_str))
			job_desc->reservation = xstrdup(value_str);
	} else if (!strcmp(name, "script")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->script);
		if (strlen(value_str))
			job_desc->script = xstrdup(value_str);
	} else if (!strcmp(name, "shared")) {
		job_desc->shared = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "sicp_mode")) {
		job_desc->sicp_mode = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "sockets_per_node")) {
		job_desc->sockets_per_node = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "std_err")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->std_err);
		if (strlen(value_str))
			job_desc->std_err = xstrdup(value_str);
	} else if (!strcmp(name, "std_in")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->std_in);
		if (strlen(value_str))
			job_desc->std_in = xstrdup(value_str);
	} else if (!strcmp(name, "std_out")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->std_out);
		if (strlen(value_str))
			job_desc->std_out = xstrdup(value_str);
	} else if (!strcmp(name, "threads_per_core")) {
		job_desc->threads_per_core = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "time_limit")) {
		job_desc->time_limit = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "time_min")) {
		job_desc->time_min = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "wait4switch")) {
		job_desc->wait4switch = luaL_checknumber(L, 3);
	} else if (!strcmp(name, "wckey")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->wckey);
		if (strlen(value_str))
			job_desc->wckey = xstrdup(value_str);
	} else if (!strcmp(name, "work_dir")) {
		value_str = luaL_checkstring(L, 3);
		xfree(job_desc->work_dir);
		if (strlen(value_str))
			job_desc->work_dir = xstrdup(value_str);
	} else {
		error("_set_job_field: unrecognized field: %s", name);
	}

	return 0;
}

static void _push_job_desc(struct job_descriptor *job_desc)
{
#if 0
	lua_newtable(L);
	lua_pushlightuserdata(L, job_desc);
	lua_setfield(L, -2, "job_desc_ptr");
#else
	lua_newtable(L);

	lua_newtable(L);
	lua_pushcfunction(L, _get_job_req_field_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, _set_job_req_field);
	lua_setfield(L, -2, "__newindex");
	/* Store the job descriptor in the metatable, so the index
	 * function knows which struct it's getting data for.
	 */
	lua_pushlightuserdata(L, job_desc);
	lua_setfield(L, -2, "_job_desc");
	lua_setmetatable(L, -2);
#endif
}

static void _push_job_rec(struct job_record *job_ptr)
{
#if 0
	lua_newtable(L);
	lua_pushlightuserdata(L, job_ptr);
	lua_setfield(L, -2, "job_rec_ptr");
#else
	lua_newtable(L);

	lua_newtable(L);
	lua_pushcfunction(L, _job_rec_field_index);
	lua_setfield(L, -2, "__index");
	/* Store the job_ptr in the metatable, so the index
	 * function knows which struct it's getting data for.
	 */
	lua_pushlightuserdata(L, job_ptr);
	lua_setfield(L, -2, "_job_rec_ptr");
	lua_setmetatable(L, -2);
#endif
}

/* Get fields in an existing slurmctld partition record
 *
 * This is an incomplete list of partition record fields. Add more as needed
 * and send patches to slurm-dev@schedmd.com
 */
static int _part_rec_field(const struct part_record *part_ptr,
                           const char *name)
{
	if (part_ptr == NULL) {
		error("_get_part_field: part_ptr is NULL");
		lua_pushnil (L);
	} else if (!strcmp(name, "default_time")) {
		lua_pushnumber (L, part_ptr->default_time);
	} else if (!strcmp(name, "flag_default")) {
		int is_default = 0;
		if (part_ptr->flags & PART_FLAG_DEFAULT)
			is_default = 1;
		lua_pushnumber (L, is_default);
	} else if (!strcmp(name, "flags")) {
		lua_pushnumber (L, part_ptr->flags);
	} else if (!strcmp(name, "max_nodes")) {
		lua_pushnumber (L, part_ptr->max_nodes);
	} else if (!strcmp(name, "max_nodes_orig")) {
		lua_pushnumber (L, part_ptr->max_nodes_orig);
	} else if (!strcmp(name, "max_time")) {
		lua_pushnumber (L, part_ptr->max_time);
	} else if (!strcmp(name, "min_nodes")) {
		lua_pushnumber (L, part_ptr->min_nodes);
	} else if (!strcmp(name, "min_nodes_orig")) {
		lua_pushnumber (L, part_ptr->min_nodes_orig);
	} else if (!strcmp(name, "name")) {
		lua_pushstring (L, part_ptr->name);
	} else if (!strcmp(name, "nodes")) {
		lua_pushstring (L, part_ptr->nodes);
	} else if (!strcmp(name, "priority")) {
		lua_pushnumber (L, part_ptr->priority);
	} else if (!strcmp(name, "state_up")) {
		lua_pushnumber (L, part_ptr->state_up);
	} else {
		lua_pushnil (L);
	}

	return 1;
}

static int _get_part_rec_field (lua_State *L)
{
	const struct part_record *part_ptr = lua_touserdata(L, 1);
	const char *name = luaL_checkstring(L, 2);

	return _part_rec_field(part_ptr, name);
}

static int _part_rec_field_index(lua_State *L)
{
	const char *name = luaL_checkstring(L, 2);
	struct part_record *part_ptr;

	lua_getmetatable(L, -2);
	lua_getfield(L, -1, "_part_rec_ptr");
	part_ptr = lua_touserdata(L, -1);

	return _part_rec_field(part_ptr, name);
}
#if 0
/* Filter before packing list of partitions */
	char *allow_groups;	/* comma delimited list of groups */
	uid_t *allow_uids;	/* zero terminated list of allowed users */
#endif

static bool _user_can_use_part(uint32_t user_id, uint32_t submit_uid,
			       struct part_record *part_ptr)
{
	int i;

	if (user_id == 0) {
		if (part_ptr->flags & PART_FLAG_NO_ROOT)
			return false;
		return true;
	}

	if ((part_ptr->flags & PART_FLAG_ROOT_ONLY) && (submit_uid != 0))
		return false;

	if (part_ptr->allow_uids == NULL)
		return true;	/* No user ID filters */

	for (i=0; part_ptr->allow_uids[i]; i++) {
		if (user_id == part_ptr->allow_uids[i])
			return true;
	}
	return false;
}

static void _push_partition_list(uint32_t user_id, uint32_t submit_uid)
{
	ListIterator part_iterator;
	struct part_record *part_ptr;

	lua_newtable(L);
	part_iterator = list_iterator_create(part_list);
	while ((part_ptr = (struct part_record *) list_next(part_iterator))) {
		if (!_user_can_use_part(user_id, submit_uid, part_ptr))
			continue;
#if 0
		lua_pushlightuserdata(L, part_ptr);
		lua_rawseti(L, -2, i++);
#else
		/* Create an empty table, with a metatable that looks up the
		 * data for the partition.
		 */
		lua_newtable(L);

		lua_newtable(L);
		lua_pushcfunction(L, _part_rec_field_index);
		lua_setfield(L, -2, "__index");
		/* Store the part_record in the metatable, so the index
		 * function knows which job it's getting data for.
		 */
		lua_pushlightuserdata(L, part_ptr);
		lua_setfield(L, -2, "_part_rec_ptr");
		lua_setmetatable(L, -2);

		lua_setfield(L, -2, part_ptr->name);
	}
#endif
	list_iterator_destroy(part_iterator);
}

static void _register_lua_slurm_output_functions (void)
{
	/*
	 *  Register slurm output functions in a global "slurm" table
	 */
	lua_newtable (L);
	luaL_register (L, NULL, slurm_functions);

	/*
	 *  Create more user-friendly lua versions of SLURM log functions.
	 */
	luaL_loadstring (L, "slurm.error (string.format(unpack({...})))");
	lua_setfield (L, -2, "log_error");
	luaL_loadstring (L, "slurm.log (0, string.format(unpack({...})))");
	lua_setfield (L, -2, "log_info");
	luaL_loadstring (L, "slurm.log (1, string.format(unpack({...})))");
	lua_setfield (L, -2, "log_verbose");
	luaL_loadstring (L, "slurm.log (2, string.format(unpack({...})))");
	lua_setfield (L, -2, "log_debug");
	luaL_loadstring (L, "slurm.log (3, string.format(unpack({...})))");
	lua_setfield (L, -2, "log_debug2");
	luaL_loadstring (L, "slurm.log (4, string.format(unpack({...})))");
	lua_setfield (L, -2, "log_debug3");
	luaL_loadstring (L, "slurm.log (5, string.format(unpack({...})))");
	lua_setfield (L, -2, "log_debug4");
	luaL_loadstring (L, "slurm.user_msg (string.format(unpack({...})))");
	lua_setfield (L, -2, "log_user");

	/*
	 * Error codes: slurm.SUCCESS, slurm.FAILURE, slurm.ERROR, etc.
	 */
	lua_pushnumber (L, SLURM_FAILURE);
	lua_setfield (L, -2, "FAILURE");
	lua_pushnumber (L, SLURM_ERROR);
	lua_setfield (L, -2, "ERROR");
	lua_pushnumber (L, SLURM_SUCCESS);
	lua_setfield (L, -2, "SUCCESS");
	lua_pushnumber (L, ESLURM_INVALID_LICENSES);
	lua_setfield (L, -2, "ESLURM_INVALID_LICENSES");

	/*
	 * Other definitions needed to interpret data
	 * slurm.MEM_PER_CPU, slurm.NO_VAL, etc.
	 */
	lua_pushnumber (L, ALLOC_SID_ADMIN_HOLD);
	lua_setfield (L, -2, "ALLOC_SID_ADMIN_HOLD");
	lua_pushnumber (L, ALLOC_SID_USER_HOLD);
	lua_setfield (L, -2, "ALLOC_SID_USER_HOLD");
	lua_pushnumber (L, MAIL_JOB_BEGIN);
	lua_setfield (L, -2, "MAIL_JOB_BEGIN");
	lua_pushnumber (L, MAIL_JOB_END);
	lua_setfield (L, -2, "MAIL_JOB_END");
	lua_pushnumber (L, MAIL_JOB_FAIL);
	lua_setfield (L, -2, "MAIL_JOB_FAIL");
	lua_pushnumber (L, MAIL_JOB_REQUEUE);
	lua_setfield (L, -2, "MAIL_JOB_REQUEUE");
	lua_pushnumber (L, MAIL_JOB_STAGE_OUT);
	lua_setfield (L, -2, "MAIL_JOB_STAGE_OUT");
	lua_pushnumber (L, MEM_PER_CPU);
	lua_setfield (L, -2, "MEM_PER_CPU");
	lua_pushnumber (L, NICE_OFFSET);
	lua_setfield (L, -2, "NICE_OFFSET");
	lua_pushnumber (L, NO_VAL);
	lua_setfield (L, -2, "NO_VAL");

	lua_setglobal (L, "slurm");

	last_lua_jobs_update = 0;
	_update_jobs_global();
	last_lua_resv_update = 0;
	_update_resvs_global();
}

static void _register_lua_slurm_struct_functions (void)
{
	lua_pushcfunction(L, _get_job_rec_field);
	lua_setglobal(L, "_get_job_rec_field");
	lua_pushcfunction(L, _get_job_req_field_name);
	lua_setglobal(L, "_get_job_req_field_name");
	lua_pushcfunction(L, _set_job_req_field);
	lua_setglobal(L, "_set_job_req_field");
	lua_pushcfunction(L, _get_part_rec_field);
	lua_setglobal(L, "_get_part_rec_field");
}

/*
 *  check that global symbol [name] in lua script is a function
 */
static int _check_lua_script_function(const char *name)
{
	int rc = 0;
	lua_getglobal(L, name);
	if (!lua_isfunction(L, -1))
		rc = -1;
	lua_pop(L, -1);
	return (rc);
}

/*
 *   Verify all required functions are defined in the job_submit/lua script
 */
static int _check_lua_script_functions(void)
{
	int rc = 0;
	int i;
	const char *fns[] = {
		"slurm_job_submit",
		"slurm_job_modify",
		NULL
	};

	i = 0;
	do {
		if (_check_lua_script_function(fns[i]) < 0) {
			error("job_submit/lua: %s: "
			      "missing required function %s",
			      lua_script_path, fns[i]);
			rc = -1;
		}
	} while (fns[++i]);

	return (rc);
}

static int _load_script(void)
{
	int rc = SLURM_SUCCESS;
	struct stat st;
	lua_State *L_orig = L;

	if (stat(lua_script_path, &st) != 0) {
		if (L_orig) {
			(void) error("Unable to stat %s, "
			             "using old script: %s",
			             lua_script_path, strerror(errno));
			return SLURM_SUCCESS;
		}
		return error("Unable to stat %s: %s",
		             lua_script_path, strerror(errno));
	}
	
	if (st.st_mtime <= lua_script_last_loaded) {
		return SLURM_SUCCESS;
	}

	/*
	 *  Initilize lua
	 */
	L = luaL_newstate();
	luaL_openlibs(L);
	if (luaL_loadfile(L, lua_script_path)) {
		if (L_orig) {
			(void) error("lua: %s: %s, using previous script",
			             lua_script_path, lua_tostring(L, -1));
			lua_close(L);
			L = L_orig;
			return SLURM_SUCCESS;
		}
		rc = error("lua: %s: %s", lua_script_path,
		           lua_tostring(L, -1));
		lua_pop(L, 1);
		return rc;
	}

	/*
	 *  Register SLURM functions in lua state:
	 *  logging and slurm structure read/write functions
	 */
	_register_lua_slurm_output_functions();
	_register_lua_slurm_struct_functions();

	/*
	 *  Run the user script:
	 */
	if (lua_pcall(L, 0, 1, 0) != 0) {
		if (L_orig) {
			(void) error("job_submit/lua: %s: %s, "
			             "using previous script",
			             lua_script_path, lua_tostring(L, -1));
			lua_close(L);
			L = L_orig;
			return SLURM_SUCCESS;
		}
		rc = error("job_submit/lua: %s: %s",
		           lua_script_path, lua_tostring(L, -1));
		lua_pop(L, 1);
		return rc;
	}

	/*
	 *  Get any return code from the lua script
	 */
	rc = (int) lua_tonumber(L, -1);
	if (rc != SLURM_SUCCESS) {
		if (L_orig) {
			(void) error("job_submit/lua: %s: returned %d "
			             "on load, using previous script",
			             lua_script_path, rc);
			lua_close(L);
			L = L_orig;
			return SLURM_SUCCESS;
		}
		(void) error("job_submit/lua: %s: returned %d on load",
		             lua_script_path, rc);
		lua_pop (L, 1);
		return rc;
	}

	/*
	 *  Check for required lua script functions:
	 */
	rc = _check_lua_script_functions();
	if (rc != SLURM_SUCCESS) {
		if (L_orig) {
			(void) error("job_submit/lua: %s: "
			             "required function(s) not present, "
			             "using previous script",
			             lua_script_path);
			lua_close(L);
			L = L_orig;
			return SLURM_SUCCESS;
		}
		return rc;
	}

	if (L_orig)
		lua_close(L_orig);
	lua_script_last_loaded = time(NULL);
	return SLURM_SUCCESS;
}

/*
 *  NOTE: The init callback should never be called multiple times,
 *   let alone called from multiple threads. Therefore, locking
 *   is unnecessary here.
 */
int init(void)
{
	/*
	 *  Need to dlopen() liblua.so with RTLD_GLOBAL in order to
	 *   ensure symbols from liblua are available to libs opened
	 *   by any lua scripts.
	 */
	if (!dlopen("liblua.so",       RTLD_NOW | RTLD_GLOBAL) &&
	    !dlopen("liblua-5.2.so",   RTLD_NOW | RTLD_GLOBAL) &&
	    !dlopen("liblua5.2.so",    RTLD_NOW | RTLD_GLOBAL) &&
	    !dlopen("liblua5.2.so.0",  RTLD_NOW | RTLD_GLOBAL) &&
	    !dlopen("liblua-5.1.so",   RTLD_NOW | RTLD_GLOBAL) &&
	    !dlopen("liblua5.1.so",    RTLD_NOW | RTLD_GLOBAL) &&
	    !dlopen("liblua5.1.so.0",  RTLD_NOW | RTLD_GLOBAL)) {
		return error("Failed to open liblua.so: %s", dlerror());
	}

	return _load_script();
}

int fini(void)
{
	lua_close (L);
	return SLURM_SUCCESS;
}


/* Lua script hook called for "submit job" event. */
extern int job_submit(struct job_descriptor *job_desc, uint32_t submit_uid,
		      char **err_msg)
{
	int rc = SLURM_ERROR;
	slurm_mutex_lock (&lua_lock);

	(void) _load_script();

	/*
	 *  All lua script functions should have been verified during
	 *   initialization:
	 */
	lua_getglobal(L, "slurm_job_submit");
	if (lua_isnil(L, -1))
		goto out;

	_update_jobs_global();
	_update_resvs_global();

	_push_job_desc(job_desc);
	_push_partition_list(job_desc->user_id, submit_uid);
	lua_pushnumber (L, submit_uid);
	_stack_dump("job_submit, before lua_pcall", L);
	if (lua_pcall (L, 3, 1, 0) != 0) {
		error("%s/lua: %s: %s",
		      __func__, lua_script_path, lua_tostring (L, -1));
	} else {
		if (lua_isnumber(L, -1)) {
			rc = lua_tonumber(L, -1);
		} else {
			info("%s/lua: %s: non-numeric return code",
			      __func__, lua_script_path);
			rc = SLURM_SUCCESS;
		}
		lua_pop(L, 1);
	}
	_stack_dump("job_submit, after lua_pcall", L);
	if (user_msg) {
		if (err_msg) {
			*err_msg = user_msg;
			user_msg = NULL;
		} else
			xfree(user_msg);
	}

out:	slurm_mutex_unlock (&lua_lock);
	return rc;
}

/* Lua script hook called for "modify job" event. */
extern int job_modify(struct job_descriptor *job_desc,
		      struct job_record *job_ptr, uint32_t submit_uid)
{
	int rc = SLURM_ERROR;
	slurm_mutex_lock (&lua_lock);

	/*
	 *  All lua script functions should have been verified during
	 *   initialization:
	 */
	lua_getglobal(L, "slurm_job_modify");
	if (lua_isnil(L, -1))
		goto out;

	_update_jobs_global();
	_update_resvs_global();

	_push_job_desc(job_desc);
	_push_job_rec(job_ptr);
	_push_partition_list(job_ptr->user_id, submit_uid);
	lua_pushnumber (L, submit_uid);
	_stack_dump("job_modify, before lua_pcall", L);
	if (lua_pcall (L, 4, 1, 0) != 0) {
		error("%s/lua: %s: %s",
		      __func__, lua_script_path, lua_tostring (L, -1));
	} else {
		if (lua_isnumber(L, -1)) {
			rc = lua_tonumber(L, -1);
		} else {
			info("%s/lua: %s: non-numeric return code",
			     __func__, lua_script_path);
			rc = SLURM_SUCCESS;
		}
		lua_pop(L, 1);
	}
	_stack_dump("job_modify, after lua_pcall", L);

out:	slurm_mutex_unlock (&lua_lock);
	return rc;
}
