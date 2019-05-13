/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "lua_common.h"
#include "lptree.h"
#include "utlist.h"
#include "unix-std.h"
#include "ottery.h"
#include "lua_thread_pool.h"
#include "libstat/stat_api.h"
#include "libserver/rspamd_control.h"

#include <math.h>


/* Lua module init function */
#define MODULE_INIT_FUNC "module_init"

#ifdef WITH_LUA_TRACE
ucl_object_t *lua_traces;
#endif

const luaL_reg null_reg[] = {
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

static const char rspamd_modules_state_global[] = "rspamd_plugins_state";

static GQuark
lua_error_quark (void)
{
	return g_quark_from_static_string ("lua-routines");
}

/* Util functions */
/**
 * Create new class and store metatable on top of the stack
 * @param L
 * @param classname name of class
 * @param func table of class methods
 */
void
rspamd_lua_new_class (lua_State * L,
	const gchar *classname,
	const struct luaL_reg *methods)
{
	luaL_newmetatable (L, classname);   /* mt */
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);      /* pushes the metatable */
	lua_settable (L, -3);       /* metatable.__index = metatable */

	lua_pushstring (L, "class");    /* mt,"__index",it,"class" */
	lua_pushstring (L, classname);  /* mt,"__index",it,"class",classname */
	lua_rawset (L, -3);         /* mt,"__index",it */
	luaL_register (L, NULL, methods);
}

/**
 * Create and register new class with static methods and store metatable on top of the stack
 */
void
rspamd_lua_new_class_full (lua_State *L,
	const gchar *classname,
	const gchar *static_name,
	const struct luaL_reg *methods,
	const struct luaL_reg *func)
{
	rspamd_lua_new_class (L, classname, methods);
	luaL_register (L, static_name, func);
}

static const gchar *
rspamd_lua_class_tostring_buf (lua_State *L, gboolean print_pointer, gint pos)
{
	static gchar buf[64];
	const gchar *ret = NULL;
	gint pop = 0;

	if (!lua_getmetatable (L, pos)) {
		goto err;
	}

	lua_pushstring (L, "__index");
	lua_gettable (L, -2);
	pop ++;

	if (!lua_istable (L, -1)) {
		goto err;
	}

	lua_pushstring (L, "class");
	lua_gettable (L, -2);
	pop ++;

	if (!lua_isstring (L, -1)) {
		goto err;
	}

	if (print_pointer) {
		rspamd_snprintf (buf, sizeof (buf), "%s(%p)", lua_tostring (L, -1),
				lua_touserdata (L, 1));
	}
	else {
		rspamd_snprintf (buf, sizeof (buf), "%s", lua_tostring (L, -1));
	}

	ret = buf;

err:
	lua_pop (L, pop);

	return ret;
}

gint
rspamd_lua_class_tostring (lua_State * L)
{
	const gchar *p;

	p = rspamd_lua_class_tostring_buf (L, TRUE, 1);

	if (!p) {
		lua_pushstring (L, "invalid object passed to 'lua_common.c:__tostring'");
		return lua_error (L);
	}

	lua_pushstring (L, p);

	return 1;
}


void
rspamd_lua_setclass (lua_State * L, const gchar *classname, gint objidx)
{
	luaL_getmetatable (L, classname);
	if (objidx < 0) {
		objidx--;
	}
	lua_setmetatable (L, objidx);
}

/* assume that table is at the top */
void
rspamd_lua_table_set (lua_State * L, const gchar *index, const gchar *value)
{

	lua_pushstring (L, index);
	if (value) {
		lua_pushstring (L, value);
	}
	else {
		lua_pushnil (L);
	}
	lua_settable (L, -3);
}

const gchar *
rspamd_lua_table_get (lua_State *L, const gchar *index)
{
	const gchar *result;

	lua_pushstring (L, index);
	lua_gettable (L, -2);
	if (!lua_isstring (L, -1)) {
		return NULL;
	}
	result = lua_tostring (L, -1);
	lua_pop (L, 1);
	return result;
}

static void
lua_add_actions_global (lua_State *L)
{
	gint i;

	lua_newtable (L);

	for (i = METRIC_ACTION_REJECT; i <= METRIC_ACTION_NOACTION; i++) {
		lua_pushstring (L, rspamd_action_to_str (i));
		lua_pushinteger (L, i);
		lua_settable (L, -3);
	}
	/* Set global table */
	lua_setglobal (L, "rspamd_actions");
}

#ifndef __APPLE__
#define OS_SO_SUFFIX ".so"
#else
#define OS_SO_SUFFIX ".dylib"
#endif

void
rspamd_lua_set_path (lua_State *L, const ucl_object_t *cfg_obj, GHashTable *vars)
{
	const gchar *old_path, *additional_path = NULL;
	const ucl_object_t *opts = NULL;
	const gchar *pluginsdir = RSPAMD_PLUGINSDIR,
			*rulesdir = RSPAMD_RULESDIR,
			*lualibdir = RSPAMD_LUALIBDIR,
			*libdir = RSPAMD_LIBDIR,
			*sharedir = RSPAMD_SHAREDIR;
	const gchar *t;

	gchar path_buf[PATH_MAX];

	lua_getglobal (L, "package");
	lua_getfield (L, -1, "path");
	old_path = luaL_checkstring (L, -1);

	if (strstr (old_path, RSPAMD_LUALIBDIR) != NULL) {
		/* Path has been already set, do not touch it */
		lua_pop (L, 2);
		return;
	}

	if (cfg_obj) {
		opts = ucl_object_lookup (cfg_obj, "options");
		if (opts != NULL) {
			opts = ucl_object_lookup (opts, "lua_path");
			if (opts != NULL && ucl_object_type (opts) == UCL_STRING) {
				additional_path = ucl_object_tostring (opts);
			}
		}
	}

	if (additional_path) {
		rspamd_snprintf (path_buf, sizeof (path_buf),
				"%s;"
				"%s",
				additional_path, old_path);
	}
	else {
		/* Try environment */
		t = getenv ("SHAREDIR");
		if (t) {
			sharedir = t;
		}

		t = getenv ("PLUGINSDIR");
		if (t) {
			pluginsdir = t;
		}

		t = getenv ("RULESDIR");
		if (t) {
			rulesdir = t;
		}

		t = getenv ("LUALIBDIR");
		if (t) {
			lualibdir = t;
		}

		t = getenv ("LIBDIR");
		if (t) {
			libdir = t;
		}

		t = getenv ("RSPAMD_LIBDIR");
		if (t) {
			libdir = t;
		}

		if (vars) {
			t = g_hash_table_lookup (vars, "PLUGINSDIR");
			if (t) {
				pluginsdir = t;
			}

			t = g_hash_table_lookup (vars, "SHAREDIR");
			if (t) {
				sharedir = t;
			}

			t = g_hash_table_lookup (vars, "RULESDIR");
			if (t) {
				rulesdir = t;
			}

			t = g_hash_table_lookup (vars, "LUALIBDIR");
			if (t) {
				lualibdir = t;
			}

			t = g_hash_table_lookup (vars, "LIBDIR");
			if (t) {
				libdir = t;
			}

			t = g_hash_table_lookup (vars, "RSPAMD_LIBDIR");
			if (t) {
				libdir = t;
			}
		}

		rspamd_snprintf (path_buf, sizeof (path_buf),
				"%s/lua/?.lua;"
				"%s/?.lua;"
				"%s/?.lua;"
				"%s/?/init.lua;"
				"%s",
				RSPAMD_CONFDIR,
				rulesdir,
				lualibdir, lualibdir,
				old_path);
	}

	lua_pop (L, 1);
	lua_pushstring (L, path_buf);
	lua_setfield (L, -2, "path");

	lua_getglobal (L, "package");
	lua_getfield (L, -1, "cpath");
	old_path = luaL_checkstring (L, -1);

	additional_path = NULL;

	if (opts != NULL) {
		opts = ucl_object_lookup (opts, "lua_cpath");
		if (opts != NULL && ucl_object_type (opts) == UCL_STRING) {
			additional_path = ucl_object_tostring (opts);
		}
	}

	if (additional_path) {
		rspamd_snprintf (path_buf, sizeof (path_buf),
				"%s/?%s;"
				"%s",
				additional_path,
				OS_SO_SUFFIX,
				old_path);
	}
	else {
		rspamd_snprintf (path_buf, sizeof (path_buf),
				"%s/?%s;"
				"%s",
				libdir,
				OS_SO_SUFFIX,
				old_path);
	}

	lua_pop (L, 1);
	lua_pushstring (L, path_buf);
	lua_setfield (L, -2, "cpath");

	lua_pop (L, 1);
}

static gint
rspamd_lua_cmp_version_components (const gchar *comp1, const gchar *comp2)
{
	guint v1, v2;

	v1 = strtoul (comp1, NULL, 10);
	v2 = strtoul (comp2, NULL, 10);

	return v1 - v2;
}

static int
rspamd_lua_rspamd_version_cmp (lua_State *L)
{
	const gchar *ver;
	gchar **components;
	gint ret = 0;

	if (lua_type (L, 2) == LUA_TSTRING) {
		ver = lua_tostring (L, 2);

		components = g_strsplit_set (ver, ".-_", -1);

		if (!components) {
			return luaL_error (L, "invalid arguments to 'cmp': %s", ver);
		}

		if (components[0]) {
			ret = rspamd_lua_cmp_version_components (components[0],
					RSPAMD_VERSION_MAJOR);
		}

		if (ret) {
			goto set;
		}

		if (components[1]) {
			ret = rspamd_lua_cmp_version_components (components[1],
					RSPAMD_VERSION_MINOR);
		}

		if (ret) {
			goto set;
		}

		if (components[2]) {
			ret = rspamd_lua_cmp_version_components (components[2],
					RSPAMD_VERSION_PATCH);
		}

		/*
		 * XXX: we don't compare git releases assuming that it is meaningless
		 */
	}
	else {
		return luaL_error (L, "invalid arguments to 'cmp'");
	}

set:
	g_strfreev (components);
	lua_pushinteger (L, ret);

	return 1;
}

static int
rspamd_lua_rspamd_version_numeric (lua_State *L)
{
	static gint64 version_num = RSPAMD_VERSION_NUM;
	const gchar *type;

	if (lua_gettop (L) >= 2 && lua_type (L, 1) == LUA_TSTRING) {
		type = lua_tostring (L, 1);
		if (g_ascii_strcasecmp (type, "short") == 0) {
			version_num = RSPAMD_VERSION_MAJOR_NUM * 1000 +
						  RSPAMD_VERSION_MINOR_NUM * 100 +
						  RSPAMD_VERSION_PATCH_NUM * 10;
		}
		else if (g_ascii_strcasecmp (type, "main") == 0) {
			version_num = RSPAMD_VERSION_MAJOR_NUM * 1000 +
						  RSPAMD_VERSION_MINOR_NUM * 100;
		}
		else if (g_ascii_strcasecmp (type, "major") == 0) {
			version_num = RSPAMD_VERSION_MAJOR_NUM;
		}
		else if (g_ascii_strcasecmp (type, "minor") == 0) {
			version_num = RSPAMD_VERSION_MINOR_NUM;
		}
		else if (g_ascii_strcasecmp (type, "patch") == 0) {
			version_num = RSPAMD_VERSION_PATCH_NUM;
		}
	}

	lua_pushinteger (L, version_num);

	return 1;
}

static int
rspamd_lua_rspamd_version (lua_State *L)
{
	const gchar *result = NULL, *type;

	if (lua_gettop (L) == 0) {
		result = RVERSION;
	}
	else if (lua_gettop (L) >= 1 && lua_type (L, 1) == LUA_TSTRING) {
		/* We got something like string */
		type = lua_tostring (L, 1);

		if (g_ascii_strcasecmp (type, "short") == 0) {
			result = RSPAMD_VERSION_MAJOR
					 "." RSPAMD_VERSION_MINOR
					 "." RSPAMD_VERSION_PATCH;
		}
		else if (g_ascii_strcasecmp (type, "main") == 0) {
			result = RSPAMD_VERSION_MAJOR "." RSPAMD_VERSION_MINOR;
		}
		else if (g_ascii_strcasecmp (type, "major") == 0) {
			result = RSPAMD_VERSION_MAJOR;
		}
		else if (g_ascii_strcasecmp (type, "minor") == 0) {
			result = RSPAMD_VERSION_MINOR;
		}
		else if (g_ascii_strcasecmp (type, "patch") == 0) {
			result = RSPAMD_VERSION_PATCH;
		}
		else if (g_ascii_strcasecmp (type, "id") == 0) {
			result = RID;
		}
		else if (g_ascii_strcasecmp (type, "num") == 0) {
			return rspamd_lua_rspamd_version_numeric (L);
		}
		else if (g_ascii_strcasecmp (type, "cmp") == 0) {
			return rspamd_lua_rspamd_version_cmp (L);
		}
	}

	lua_pushstring (L, result);

	return 1;
}

static gboolean
rspamd_lua_load_env (lua_State *L, const char *fname, gint tbl_pos, GError **err)
{
	gint orig_top = lua_gettop (L), err_idx;
	gboolean ret = TRUE;

	lua_pushcfunction (L, &rspamd_lua_traceback);
	err_idx = lua_gettop (L);

	if (luaL_loadfile (L, fname) != 0) {
		g_set_error (err, g_quark_from_static_string ("lua_env"), errno,
				"cannot load lua file %s: %s",
				fname,
				lua_tostring (L, -1));
		ret = FALSE;
	}

	if (ret && lua_pcall (L, 0, 1, err_idx) != 0) {
		GString *tb = lua_touserdata (L, -1);
		g_set_error (err, g_quark_from_static_string ("lua_env"), errno,
				"cannot init lua file %s: %s",
				fname,
				tb->str);
		g_string_free (tb, TRUE);

		ret = FALSE;
	}

	if (ret && lua_type (L, -1) == LUA_TTABLE) {
		for (lua_pushnil (L); lua_next (L, -2); lua_pop (L, 1)) {
			lua_pushvalue (L, -2); /* Store key */
			lua_pushvalue (L, -2); /* Store value */
			lua_settable (L, tbl_pos);
		}
	}
	else if (ret) {
		g_set_error (err, g_quark_from_static_string ("lua_env"), errno,
				"invalid return type when loading env from %s: %s",
				fname,
				lua_typename (L, lua_type (L, -1)));
		ret = FALSE;
	}

	lua_settop (L, orig_top);

	return ret;
}

gboolean
rspamd_lua_set_env (lua_State *L, GHashTable *vars, char **lua_env, GError **err)
{
	gint orig_top = lua_gettop (L);
	gchar **env = g_get_environ ();

	/* Set known paths as rspamd_paths global */
	lua_getglobal (L, "rspamd_paths");
	if (lua_isnil (L, -1)) {
		const gchar *confdir = RSPAMD_CONFDIR,
				*local_confdir = RSPAMD_LOCAL_CONFDIR,
				*rundir = RSPAMD_RUNDIR,
				*dbdir = RSPAMD_DBDIR,
				*logdir = RSPAMD_LOGDIR,
				*wwwdir = RSPAMD_WWWDIR,
				*pluginsdir = RSPAMD_PLUGINSDIR,
				*rulesdir = RSPAMD_RULESDIR,
				*lualibdir = RSPAMD_LUALIBDIR,
				*prefix = RSPAMD_PREFIX,
				*sharedir = RSPAMD_SHAREDIR;
		const gchar *t;

		/* Try environment */
		t = g_environ_getenv (env, "SHAREDIR");
		if (t) {
			sharedir = t;
		}

		t = g_environ_getenv (env, "PLUGINSDIR");
		if (t) {
			pluginsdir = t;
		}

		t = g_environ_getenv (env, "RULESDIR");
		if (t) {
			rulesdir = t;
		}

		t = g_environ_getenv (env, "DBDIR");
		if (t) {
			dbdir = t;
		}

		t = g_environ_getenv (env, "RUNDIR");
		if (t) {
			rundir = t;
		}

		t = g_environ_getenv (env, "LUALIBDIR");
		if (t) {
			lualibdir = t;
		}

		t = g_environ_getenv (env, "LOGDIR");
		if (t) {
			logdir = t;
		}

		t = g_environ_getenv (env, "WWWDIR");
		if (t) {
			wwwdir = t;
		}

		t = g_environ_getenv (env, "CONFDIR");
		if (t) {
			confdir = t;
		}

		t = g_environ_getenv (env, "LOCAL_CONFDIR");
		if (t) {
			local_confdir = t;
		}


		if (vars) {
			t = g_hash_table_lookup (vars, "SHAREDIR");
			if (t) {
				sharedir = t;
			}

			t = g_hash_table_lookup (vars, "PLUGINSDIR");
			if (t) {
				pluginsdir = t;
			}

			t = g_hash_table_lookup (vars, "RULESDIR");
			if (t) {
				rulesdir = t;
			}

			t = g_hash_table_lookup (vars, "LUALIBDIR");
			if (t) {
				lualibdir = t;
			}

			t = g_hash_table_lookup (vars, "RUNDIR");
			if (t) {
				rundir = t;
			}

			t = g_hash_table_lookup (vars, "WWWDIR");
			if (t) {
				wwwdir = t;
			}

			t = g_hash_table_lookup (vars, "CONFDIR");
			if (t) {
				confdir = t;
			}

			t = g_hash_table_lookup (vars, "LOCAL_CONFDIR");
			if (t) {
				local_confdir = t;
			}

			t = g_hash_table_lookup (vars, "DBDIR");
			if (t) {
				dbdir = t;
			}

			t = g_hash_table_lookup (vars, "LOGDIR");
			if (t) {
				logdir = t;
			}
		}

		lua_createtable (L, 0, 9);

		rspamd_lua_table_set (L, RSPAMD_SHAREDIR_INDEX, sharedir);
		rspamd_lua_table_set (L, RSPAMD_CONFDIR_INDEX, confdir);
		rspamd_lua_table_set (L, RSPAMD_LOCAL_CONFDIR_INDEX, local_confdir);
		rspamd_lua_table_set (L, RSPAMD_RUNDIR_INDEX, rundir);
		rspamd_lua_table_set (L, RSPAMD_DBDIR_INDEX, dbdir);
		rspamd_lua_table_set (L, RSPAMD_LOGDIR_INDEX, logdir);
		rspamd_lua_table_set (L, RSPAMD_WWWDIR_INDEX, wwwdir);
		rspamd_lua_table_set (L, RSPAMD_PLUGINSDIR_INDEX, pluginsdir);
		rspamd_lua_table_set (L, RSPAMD_RULESDIR_INDEX, rulesdir);
		rspamd_lua_table_set (L, RSPAMD_LUALIBDIR_INDEX, lualibdir);
		rspamd_lua_table_set (L, RSPAMD_PREFIX_INDEX, prefix);

		lua_setglobal (L, "rspamd_paths");
	}

	lua_getglobal (L, "rspamd_env");
	if (lua_isnil (L, -1)) {
		lua_newtable (L);

		if (vars != NULL) {
			GHashTableIter it;
			gpointer k, v;

			g_hash_table_iter_init (&it, vars);

			while (g_hash_table_iter_next (&it, &k, &v)) {
				rspamd_lua_table_set (L, k, v);
			}
		}

		gint hostlen = sysconf (_SC_HOST_NAME_MAX);

		if (hostlen <= 0) {
			hostlen = 256;
		}
		else {
			hostlen ++;
		}

		gchar *hostbuf = g_alloca (hostlen);
		memset (hostbuf, 0, hostlen);
		gethostname (hostbuf, hostlen - 1);

		rspamd_lua_table_set (L, "hostname", hostbuf);

		rspamd_lua_table_set (L, "version", RVERSION);
		rspamd_lua_table_set (L, "ver_major", RSPAMD_VERSION_MAJOR);
		rspamd_lua_table_set (L, "ver_minor", RSPAMD_VERSION_MINOR);
		rspamd_lua_table_set (L, "ver_patch", RSPAMD_VERSION_PATCH);
		rspamd_lua_table_set (L, "ver_id", RID);
		lua_pushstring (L, "ver_num");
		lua_pushinteger (L, RSPAMD_VERSION_NUM);
		lua_settable (L, -3);

		if (env) {
			gint lim = g_strv_length (env);

			for (gint i = 0; i < lim; i++) {
				if (RSPAMD_LEN_CHECK_STARTS_WITH(env[i], strlen (env[i]), "RSPAMD_")) {
					const char *var = env[i] + sizeof ("RSPAMD_") - 1, *value;
					gint varlen;

					varlen = strcspn (var, "=");
					value = var + varlen;

					if (*value == '=') {
						value ++;

						lua_pushlstring (L, var, varlen);
						lua_pushstring (L, value);
						lua_settable (L, -3);
					}

				}
			}
		}

		if (lua_env) {
			gint lim = g_strv_length (lua_env);

			for (gint i = 0; i < lim; i ++) {
				if (!rspamd_lua_load_env (L, lua_env[i], lua_gettop (L), err)) {
					return FALSE;
				}
			}
		}

		lua_setglobal (L, "rspamd_env");
	}

	lua_settop (L, orig_top);
	g_strfreev (env);

	return TRUE;
}

void
rspamd_lua_set_globals (struct rspamd_config *cfg, lua_State *L)
{
	struct rspamd_config **pcfg;
	gint orig_top = lua_gettop (L);

	/* First check for global variable 'config' */
	lua_getglobal (L, "config");
	if (lua_isnil (L, -1)) {
		/* Assign global table to set up attributes */
		lua_newtable (L);
		lua_setglobal (L, "config");
	}

	lua_getglobal (L, "metrics");
	if (lua_isnil (L, -1)) {
		lua_newtable (L);
		lua_setglobal (L, "metrics");
	}

	lua_getglobal (L, "composites");
	if (lua_isnil (L, -1)) {
		lua_newtable (L);
		lua_setglobal (L, "composites");
	}

	lua_getglobal (L, "rspamd_classifiers");
	if (lua_isnil (L, -1)) {
		lua_newtable (L);
		lua_setglobal (L, "rspamd_classifiers");
	}

	lua_getglobal (L, "classifiers");
	if (lua_isnil (L, -1)) {
		lua_newtable (L);
		lua_setglobal (L, "classifiers");
	}

	lua_getglobal (L, "rspamd_version");
	if (lua_isnil (L, -1)) {
		lua_pushcfunction (L, rspamd_lua_rspamd_version);
		lua_setglobal (L, "rspamd_version");
	}

	if (cfg != NULL) {
		pcfg = lua_newuserdata (L, sizeof (struct rspamd_config *));
		rspamd_lua_setclass (L, "rspamd{config}", -1);
		*pcfg = cfg;
		lua_setglobal (L, "rspamd_config");
	}

	lua_settop (L, orig_top);
}

#ifdef WITH_LUA_TRACE
static gint
lua_push_trace_data (lua_State *L)
{
	if (lua_traces) {
		ucl_object_push_lua (L, lua_traces, true);
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}
#endif



static void *
rspamd_lua_wipe_realloc (void *ud,
						 void *ptr,
						 size_t osize,
						 size_t nsize) RSPAMD_ATTR_ALLOC_SIZE(4);
static void *
rspamd_lua_wipe_realloc (void *ud,
						 void *ptr,
						 size_t osize,
						 size_t nsize)
{
	if (nsize == 0) {
		if (ptr) {
			rspamd_explicit_memzero (ptr, osize);
		}

		free (ptr);
	}
	else if (ptr == NULL) {
		return malloc (nsize);
	}
	else {
		if (nsize < osize) {
			/* Wipe on shrinking (actually never used) */
			rspamd_explicit_memzero (((unsigned char *)ptr) + nsize, osize - nsize);
		}

		return realloc (ptr, nsize);
	}

	return NULL;
}

lua_State *
rspamd_lua_init (bool wipe_mem)
{
	lua_State *L;

	if (wipe_mem) {
#ifdef WITH_LUAJIT
		/* TODO: broken on luajit without GC64 */
		L = luaL_newstate ();
#else
		L = lua_newstate (rspamd_lua_wipe_realloc, NULL);
#endif
	}
	else {
		L = luaL_newstate ();
	}

	lua_gc (L, LUA_GCSTOP, 0);
	luaL_openlibs (L);
	luaopen_logger (L);
	luaopen_mempool (L);
	luaopen_config (L);
	luaopen_map (L);
	luaopen_trie (L);
	luaopen_task (L);
	luaopen_textpart (L);
	luaopen_mimepart (L);
	luaopen_image (L);
	luaopen_url (L);
	luaopen_classifier (L);
	luaopen_statfile (L);
	luaopen_regexp (L);
	luaopen_cdb (L);
	luaopen_xmlrpc (L);
	luaopen_http (L);
	luaopen_redis (L);
	luaopen_upstream (L);
	lua_add_actions_global (L);
	luaopen_dns_resolver (L);
	luaopen_rsa (L);
	luaopen_ip (L);
	luaopen_expression (L);
	luaopen_text (L);
	luaopen_util (L);
	luaopen_tcp (L);
	luaopen_html (L);
	luaopen_fann (L);
	luaopen_sqlite3 (L);
	luaopen_cryptobox (L);
	luaopen_dns (L);
	luaopen_udp (L);
	luaopen_worker (L);

	luaL_newmetatable (L, "rspamd{ev_base}");
	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{ev_base}");
	lua_rawset (L, -3);
	lua_pop (L, 1);

	luaL_newmetatable (L, "rspamd{session}");
	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{session}");
	lua_rawset (L, -3);
	lua_pop (L, 1);

	rspamd_lua_add_preload (L, "lpeg", luaopen_lpeg);
	luaopen_ucl (L);
	rspamd_lua_add_preload (L, "ucl", luaopen_ucl);

	/* Add plugins global */
	lua_newtable (L);
	lua_setglobal (L, "rspamd_plugins");

	/* Set PRNG */
	lua_getglobal (L, "math");
	lua_pushstring (L, "randomseed");
	lua_gettable (L, -2);
	lua_pushinteger (L, ottery_rand_uint64 ());
	lua_pcall (L, 1, 0, 0);
	lua_pop (L, 1); /* math table */

	/* Modules state */
	lua_newtable (L);
	/*
	 * rspamd_plugins_state = {
	 *   enabled = {},
	 *   disabled_unconfigured = {},
	 *   disabled_redis = {},
	 *   disabled_explicitly = {},
	 *   disabled_failed = {},
	 *   disabled_experimental = {},
	 * }
	 */
#define ADD_TABLE(name) do { \
	lua_pushstring (L, #name); \
	lua_newtable (L); \
	lua_settable (L, -3); \
} while (0)

	ADD_TABLE (enabled);
	ADD_TABLE (disabled_unconfigured);
	ADD_TABLE (disabled_redis);
	ADD_TABLE (disabled_explicitly);
	ADD_TABLE (disabled_failed);
	ADD_TABLE (disabled_experimental);

#undef ADD_TABLE
	lua_setglobal (L, rspamd_modules_state_global);

#ifdef WITH_LUA_TRACE
	lua_pushcfunction (L, lua_push_trace_data);
	lua_setglobal (L, "get_traces");
#endif

	return L;
}

void
rspamd_lua_start_gc (struct rspamd_config *cfg)
{
	lua_State *L = (lua_State *)cfg->lua_state;

	lua_settop (L, 0);
	/* Set up GC */
	lua_gc (L, LUA_GCCOLLECT, 0);
	lua_gc (L, LUA_GCSETSTEPMUL, cfg->lua_gc_step);
	lua_gc (L, LUA_GCSETPAUSE, cfg->lua_gc_pause);
	lua_gc (L, LUA_GCRESTART, 0);
}

/**
 * Initialize new locked lua_State structure
 */
struct lua_locked_state *
rspamd_init_lua_locked (struct rspamd_config *cfg)
{
	struct lua_locked_state *new;

	new = g_malloc0 (sizeof (struct lua_locked_state));
	new->L = rspamd_lua_init (false);
	new->m = rspamd_mutex_new ();

	return new;
}

/**
 * Free locked state structure
 */
void
rspamd_free_lua_locked (struct lua_locked_state *st)
{
	g_assert (st != NULL);

	lua_close (st->L);

	rspamd_mutex_free (st->m);

	g_free (st);
}


void
rspamd_plugins_table_push_elt (lua_State *L, const gchar *field_name,
		const gchar *new_elt)
{
	lua_getglobal (L, rspamd_modules_state_global);
	lua_pushstring (L, field_name);
	lua_gettable (L, -2);
	lua_pushstring (L, new_elt);
	lua_newtable (L);
	lua_settable (L, -3);
	lua_pop (L, 2); /* Global + element */
}

gboolean
rspamd_init_lua_filters (struct rspamd_config *cfg, gboolean force_load)
{
	struct rspamd_config **pcfg;
	GList *cur;
	struct script_module *module;
	lua_State *L = cfg->lua_state;
	GString *tb;
	gint err_idx;

	cur = g_list_first (cfg->script_modules);

	while (cur) {
		module = cur->data;

		if (module->path) {
			if (!force_load) {
				if (!rspamd_config_is_module_enabled (cfg, module->name)) {
					cur = g_list_next (cur);
					continue;
				}
			}

			lua_pushcfunction (L, &rspamd_lua_traceback);
			err_idx = lua_gettop (L);

			if (luaL_loadfile (L, module->path) != 0) {
				msg_err_config ("load of %s failed: %s", module->path,
					lua_tostring (L, -1));
				lua_pop (L, 1); /*  Error function */

				rspamd_plugins_table_push_elt (L, "disabled_failed",
						module->name);

				cur = g_list_next (cur);
				continue;
			}

			/* Initialize config structure */
			pcfg = lua_newuserdata (L, sizeof (struct rspamd_config *));
			rspamd_lua_setclass (L, "rspamd{config}", -1);
			*pcfg = cfg;
			lua_setglobal (L, "rspamd_config");

			if (lua_pcall (L, 0, 0, err_idx) != 0) {
				tb = lua_touserdata (L, -1);
				msg_err_config ("init of %s failed: %v",
						module->path,
						tb);

				g_string_free (tb, TRUE);
				lua_pop (L, 2); /* Result and error function */

				rspamd_plugins_table_push_elt (L, "disabled_failed",
						module->name);

				cur = g_list_next (cur);
				continue;
			}

			if (!force_load) {
				msg_info_config ("init lua module %s", module->name);
			}

			lua_pop (L, 1); /* Error function */
		}

		cur = g_list_next (cur);
	}

	return TRUE;
}

void
rspamd_lua_dumpstack (lua_State *L)
{
	gint i, t, r = 0;
	gint top = lua_gettop (L);
	gchar buf[BUFSIZ];

	r += rspamd_snprintf (buf + r, sizeof (buf) - r, "lua stack: ");
	for (i = 1; i <= top; i++) { /* repeat for each level */
		t = lua_type (L, i);
		switch (t)
		{
		case LUA_TSTRING: /* strings */
			r += rspamd_snprintf (buf + r,
					sizeof (buf) - r,
					"str: %s",
					lua_tostring (L, i));
			break;

		case LUA_TBOOLEAN: /* booleans */
			r += rspamd_snprintf (buf + r, sizeof (buf) - r,
					lua_toboolean (L, i) ? "bool: true" : "bool: false");
			break;

		case LUA_TNUMBER: /* numbers */
			r += rspamd_snprintf (buf + r,
					sizeof (buf) - r,
					"number: %.2f",
					lua_tonumber (L, i));
			break;

		default: /* other values */
			r += rspamd_snprintf (buf + r,
					sizeof (buf) - r,
					"type: %s",
					lua_typename (L, t));
			break;

		}
		if (i < top) {
			r += rspamd_snprintf (buf + r, sizeof (buf) - r, " -> "); /* put a separator */
		}
	}
	msg_info (buf);
}

gpointer
rspamd_lua_check_class (lua_State *L, gint index, const gchar *name)
{
	gpointer p;

	if (lua_type (L, index) == LUA_TUSERDATA) {
		p = lua_touserdata (L, index);
		if (p) {
			if (lua_getmetatable (L, index)) {
				lua_getfield (L, LUA_REGISTRYINDEX, name);  /* get correct metatable */
				if (lua_rawequal (L, -1, -2)) {  /* does it have the correct mt? */
					lua_pop (L, 2);  /* remove both metatables */
					return p;
				}
				lua_pop (L, 2);
			}
		}
	}
	return NULL;
}

int
rspamd_lua_typerror (lua_State *L, int narg, const char *tname)
{
	const char *msg = lua_pushfstring (L, "%s expected, got %s", tname,
			luaL_typename (L, narg));
	return luaL_argerror (L, narg, msg);
}


void
rspamd_lua_add_preload (lua_State *L, const gchar *name, lua_CFunction func)
{
	lua_getglobal (L, "package");
	lua_pushstring (L, "preload");
	lua_gettable (L, -2);
	lua_pushcfunction (L, func);
	lua_setfield (L, -2, name);
	lua_pop (L, 2); /* preload key + global package */
}


gboolean
rspamd_lua_parse_table_arguments (lua_State *L, gint pos,
		GError **err, const gchar *extraction_pattern, ...)
{
	const gchar *p, *key = NULL, *end, *cls;
	va_list ap;
	gboolean required = FALSE, failed = FALSE, is_table;
	gchar classbuf[128];
	enum {
		read_key = 0,
		read_arg,
		read_class_start,
		read_class,
		read_semicolon
	} state = read_key;
	gsize keylen = 0, *valuelen, clslen;
	gint idx = 0, t;

	g_assert (extraction_pattern != NULL);

	if (pos < 0) {
		/* Get absolute pos */
		pos = lua_gettop (L) + pos + 1;
	}

	if (lua_type (L, pos) == LUA_TTABLE) {
		is_table = TRUE;
	}
	else {
		is_table = FALSE;
		idx = pos;
	}

	p = extraction_pattern;
	end = p + strlen (extraction_pattern);

	va_start (ap, extraction_pattern);

	while (p <= end) {
		switch (state) {
		case read_key:
			if (*p == '=') {
				if (key == NULL) {
					g_set_error (err, lua_error_quark (), 1, "cannot read key");
					va_end (ap);

					return FALSE;
				}

				state = read_arg;
				keylen = p - key;
			}
			else if (*p == '*' && key == NULL) {
				required = TRUE;
			}
			else if (key == NULL) {
				key = p;
			}
			p ++;
			break;
		case read_arg:
			g_assert (keylen != 0);

			if (is_table) {
				lua_pushlstring (L, key, keylen);
				lua_gettable (L, pos);
				idx = -1;
			}

			t = lua_type (L, idx);

			switch (g_ascii_toupper (*p)) {
			case 'S':
				if (t == LUA_TSTRING) {
					*(va_arg (ap, const gchar **)) = lua_tostring (L, idx);
				}
				else if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap, const gchar **)) = NULL;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)), "string");
					va_end (ap);

					return FALSE;
				}

				if (is_table) {
					lua_pop (L, 1);
				}
				break;

			case 'I':
				if (t == LUA_TNUMBER) {
					*(va_arg (ap, gint64 *)) = lua_tonumber (L, idx);
				}
				else if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap,  gint64 *)) = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)),
							"int64");
					va_end (ap);

					return FALSE;
				}
				if (is_table) {
					lua_pop (L, 1);
				}
				break;

			case 'F':
				if (t == LUA_TFUNCTION) {
					if (!is_table) {
						lua_pushvalue (L, idx);
					}

					*(va_arg (ap, gint *)) = luaL_ref (L, LUA_REGISTRYINDEX);
				}
				else if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap,  gint *)) = -1;
					if (is_table) {
						lua_pop (L, 1);
					}
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)),
							"function");
					va_end (ap);
					if (is_table) {
						lua_pop (L, 1);
					}

					return FALSE;
				}

				/* luaL_ref pops argument from the stack */
				break;

			case 'B':
				if (t == LUA_TBOOLEAN) {
					*(va_arg (ap, gboolean *)) = lua_toboolean (L, idx);
				}
				else if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap,  gboolean *)) = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)),
							"bool");
					va_end (ap);

					return FALSE;
				}

				if (is_table) {
					lua_pop (L, 1);
				}
				break;

			case 'N':
				if (t == LUA_TNUMBER) {
					*(va_arg (ap, gdouble *)) = lua_tonumber (L, idx);
				}
				else if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap,  gdouble *)) = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)),
							"double");
					va_end (ap);

					return FALSE;
				}

				if (is_table) {
					lua_pop (L, 1);
				}
				break;

			case 'D':
				if (t == LUA_TNUMBER) {
					*(va_arg (ap, gdouble *)) = lua_tonumber (L, idx);
				}
				else if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap,  gdouble *)) = NAN;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
							" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)),
							"double");
					va_end (ap);

					return FALSE;
				}

				if (is_table) {
					lua_pop (L, 1);
				}
				break;

			case 'V':
				valuelen = va_arg (ap, gsize *);

				if (t == LUA_TSTRING) {
					*(va_arg (ap, const gchar **)) = lua_tolstring (L, idx,
							valuelen);
				}
				else if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap, const char **)) = NULL;
					*valuelen = 0;
				}
				else {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)),
							"string");
					va_end (ap);

					return FALSE;
				}

				if (is_table) {
					lua_pop (L, 1);
				}
				break;
			case 'O':
				if (t != LUA_TNONE) {
					*(va_arg (ap, ucl_object_t **)) = ucl_object_lua_import (L,
							idx);
				}
				else {
					failed = TRUE;
					*(va_arg (ap, ucl_object_t **)) = NULL;
				}

				if (is_table) {
					lua_pop (L, 1);
				}
				break;
			case 'U':
				if (t == LUA_TNIL || t == LUA_TNONE) {
					failed = TRUE;
					*(va_arg (ap, void **)) = NULL;
				}
				else if (t != LUA_TUSERDATA) {
					g_set_error (err,
							lua_error_quark (),
							1,
							"bad type for key:"
									" %.*s: '%s', '%s' is expected",
							(gint) keylen,
							key,
							lua_typename (L, lua_type (L, idx)),
							"int64");
					va_end (ap);

					return FALSE;
				}

				state = read_class_start;
				clslen = 0;
				cls = NULL;
				p ++;
				continue;
			default:
				g_assert (0);
				break;
			}

			if (failed && required) {
				g_set_error (err, lua_error_quark (), 2, "required parameter "
						"%.*s is missing", (gint)keylen, key);
				va_end (ap);

				return FALSE;
			}

			if (!is_table) {
				idx ++;
			}

			/* Reset read params */
			state = read_semicolon;
			failed = FALSE;
			required = FALSE;
			keylen = 0;
			key = NULL;
			p ++;
			break;

		case read_class_start:
			if (*p == '{') {
				cls = p + 1;
				state = read_class;
			}
			else {
				if (is_table) {
					lua_pop (L, 1);
				}

				g_set_error (err, lua_error_quark (), 2, "missing classname for "
						"%.*s", (gint)keylen, key);
				va_end (ap);

				return FALSE;
			}
			p ++;
			break;

		case read_class:
			if (*p == '}') {
				clslen = p - cls;
				if (clslen == 0) {
					if (is_table) {
						lua_pop (L, 1);
					}

					g_set_error (err,
							lua_error_quark (),
							2,
							"empty classname for "
									"%*.s",
							(gint) keylen,
							key);
					va_end (ap);

					return FALSE;
				}

				rspamd_snprintf (classbuf, sizeof (classbuf), "rspamd{%*s}",
						(gint) clslen, cls);


				/*
				 * We skip class check here for speed in non-table mode
				 */
				if (!failed && (!is_table ||
						rspamd_lua_check_class (L, idx, classbuf))) {
					*(va_arg (ap, void **)) = *(void **)lua_touserdata (L, idx);
				}
				else {
					if (!failed) {
						g_set_error (err,
								lua_error_quark (),
								2,
								"invalid class for key %.*s, expected %s, got %s",
								(gint) keylen,
								key,
								classbuf,
								rspamd_lua_class_tostring_buf (L, FALSE, idx));
						va_end (ap);

						return FALSE;
					}
				}

				if (is_table) {
					lua_pop (L, 1);
				}
				else {
					idx ++;
				}

				if (failed && required) {
					g_set_error (err,
							lua_error_quark (),
							2,
							"required parameter "
									"%.*s is missing",
							(gint) keylen,
							key);
					va_end (ap);

					return FALSE;
				}

				/* Reset read params */
				state = read_semicolon;
				failed = FALSE;
				required = FALSE;
				keylen = 0;
				key = NULL;
			}
			p ++;
			break;

		case read_semicolon:
			if (*p == ';' || p == end) {
				state = read_key;
				key = NULL;
				keylen = 0;
				failed = FALSE;
			}
			else {
				g_set_error (err, lua_error_quark (), 2, "bad format string: %s,"
								" at char %c, position %d",
						extraction_pattern, *p, (int)(p - extraction_pattern));
				va_end (ap);

				return FALSE;
			}

			p++;
			break;
		}
	}

	va_end (ap);

	return TRUE;
}

static void
rspamd_lua_traceback_string (lua_State *L, GString *s)
{
	gint i = 1;
	lua_Debug d;

	while (lua_getstack (L, i++, &d)) {
		lua_getinfo (L, "nSl", &d);
		g_string_append_printf (s, " [%d]:{%s:%d - %s [%s]};",
				i - 1, d.short_src, d.currentline,
				(d.name ? d.name : "<unknown>"), d.what);
	}
}

gint
rspamd_lua_traceback (lua_State *L)
{

	GString *tb;

	tb = rspamd_lua_get_traceback_string (L);

	lua_pushlightuserdata (L, tb);

	return 1;
}

GString *
rspamd_lua_get_traceback_string (lua_State *L)
{
	GString *tb;
	const gchar *msg = lua_tostring (L, -1);

	tb = g_string_sized_new (100);
	g_string_append_printf (tb, "%s; trace:", msg);

	rspamd_lua_traceback_string (L, tb);

	return tb;
}

guint
rspamd_lua_table_size (lua_State *L, gint tbl_pos)
{
	guint tbl_size = 0;

	if (!lua_istable (L, tbl_pos)) {
		return 0;
	}

#if LUA_VERSION_NUM >= 502
	tbl_size = lua_rawlen (L, tbl_pos);
#else
	tbl_size = lua_objlen (L, tbl_pos);
#endif

	return tbl_size;
}

static void *
rspamd_lua_check_udata_common (lua_State *L, gint pos, const gchar *classname,
		gboolean fatal)
{
	void *p = lua_touserdata (L, pos);
	GString *err_msg;
	guint i, top = lua_gettop (L);

	if (p == NULL) {
		goto err;
	}
	else {
		/* Match class */
		if (lua_getmetatable (L, pos)) {
			luaL_getmetatable (L, classname);

			if (!lua_rawequal (L, -1, -2)) {
				goto err;
			}
		}
		else {
			goto err;
		}
	}

	lua_settop (L, top);

	return p;

err:
	if (fatal) {
		const gchar *actual_classname = NULL;

		if (lua_type (L, pos) == LUA_TUSERDATA && lua_getmetatable (L, pos)) {
			lua_pushstring (L, "__index");
			lua_gettable (L, -2);
			lua_pushstring (L, "class");
			lua_gettable (L, -2);
			actual_classname = lua_tostring (L, -1);
		}
		else {
			actual_classname = lua_typename (L, lua_type (L, pos));
		}

		err_msg = g_string_sized_new (100);
		rspamd_printf_gstring (err_msg, "expected %s at position %d, but userdata has "
										"%s metatable; trace: ",
				classname, pos, actual_classname);
		rspamd_lua_traceback_string (L, err_msg);
		rspamd_printf_gstring (err_msg, " stack(%d): ", top);

		for (i = 1; i <= MIN (top, 10); i ++) {
			if (lua_type (L, i) == LUA_TUSERDATA) {
				const char *clsname;

				if (lua_getmetatable (L, i)) {
					lua_pushstring (L, "__index");
					lua_gettable (L, -2);
					lua_pushstring (L, "class");
					lua_gettable (L, -2);
					clsname = lua_tostring (L, -1);
				}
				else {
					clsname = lua_typename (L, lua_type (L, i));
				}

				rspamd_printf_gstring (err_msg, "[%d: ud=%s] ", i,
						clsname);
			}
			else {
				rspamd_printf_gstring (err_msg, "[%d: %s] ", i,
						lua_typename (L, lua_type (L, i)));
			}
		}

		msg_err ("lua type error: %v", err_msg);
		g_string_free (err_msg, TRUE);
	}

	lua_settop (L, top);

	return NULL;
}

void *
rspamd_lua_check_udata (lua_State *L, gint pos, const gchar *classname)
{
	return rspamd_lua_check_udata_common (L, pos, classname, TRUE);
}

void *
rspamd_lua_check_udata_maybe (lua_State *L, gint pos, const gchar *classname)
{
	return rspamd_lua_check_udata_common (L, pos, classname, FALSE);
}

struct rspamd_async_session*
lua_check_session (lua_State * L, gint pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{session}");
	luaL_argcheck (L, ud != NULL, pos, "'session' expected");
	return ud ? *((struct rspamd_async_session **)ud) : NULL;
}

struct event_base*
lua_check_ev_base (lua_State * L, gint pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{ev_base}");
	luaL_argcheck (L, ud != NULL, pos, "'event_base' expected");
	return ud ? *((struct event_base **)ud) : NULL;
}

static void rspamd_lua_run_postloads_error (struct thread_entry *thread, int ret, const char *msg);

void
rspamd_lua_run_postloads (lua_State *L, struct rspamd_config *cfg,
		struct event_base *ev_base, struct rspamd_worker *w)
{
	struct rspamd_config_post_load_script *sc;
	struct rspamd_config **pcfg;
	struct event_base **pev_base;
	struct rspamd_worker **pw;

	/* Execute post load scripts */
	LL_FOREACH (cfg->on_load, sc) {
		struct thread_entry *thread = lua_thread_pool_get_for_config (cfg);
		thread->error_callback = rspamd_lua_run_postloads_error;
		L = thread->lua_state;

		lua_rawgeti (L, LUA_REGISTRYINDEX, sc->cbref);
		pcfg = lua_newuserdata (L, sizeof (*pcfg));
		*pcfg = cfg;
		rspamd_lua_setclass (L, "rspamd{config}", -1);

		pev_base = lua_newuserdata (L, sizeof (*pev_base));
		*pev_base = ev_base;
		rspamd_lua_setclass (L, "rspamd{ev_base}", -1);

		pw = lua_newuserdata (L, sizeof (*pw));
		*pw = w;
		rspamd_lua_setclass (L, "rspamd{worker}", -1);

		lua_thread_call (thread, 3);
	}
}


static void
rspamd_lua_run_postloads_error (struct thread_entry *thread, int ret, const char *msg)
{
	struct rspamd_config *cfg = thread->cfg;

	msg_err_config ("error executing post load code: %s", msg);
}


struct rspamd_lua_ref_cbdata {
	lua_State *L;
	gint cbref;
};

static void
rspamd_lua_ref_dtor (gpointer p)
{
	struct rspamd_lua_ref_cbdata *cbdata = p;

	luaL_unref (cbdata->L, LUA_REGISTRYINDEX, cbdata->cbref);
}

void
rspamd_lua_add_ref_dtor (lua_State *L, rspamd_mempool_t *pool,
		gint ref)
{
	struct rspamd_lua_ref_cbdata *cbdata;

	if (ref != -1) {
		cbdata = rspamd_mempool_alloc (pool, sizeof (*cbdata));
		cbdata->cbref = ref;
		cbdata->L = L;

		rspamd_mempool_add_destructor (pool, rspamd_lua_ref_dtor, cbdata);
	}
}

gboolean
rspamd_lua_require_function (lua_State *L, const gchar *modname,
		const gchar *funcname)
{
	gint table_pos;

	lua_getglobal (L, "require");

	if (lua_isnil (L, -1)) {
		lua_pop (L, 1);

		return FALSE;
	}

	lua_pushstring (L, modname);

	/* Now try to call */
	if (lua_pcall (L, 1, 1, 0) != 0) {
		lua_pop (L, 1);

		return FALSE;
	}

	/* Now we should have a table with results */
	if (!lua_istable (L, -1)) {
		lua_pop (L, 1);

		return FALSE;
	}

	table_pos = lua_gettop (L);
	lua_pushstring (L, funcname);
	lua_gettable (L, -2);

	if (lua_type (L, -1) == LUA_TFUNCTION) {
		/* Remove table, preserve just a function */
		lua_remove (L, table_pos);

		return TRUE;
	}

	lua_pop (L, 2);

	return FALSE;
}


gboolean
rspamd_lua_try_load_redis (lua_State *L, const ucl_object_t *obj,
									struct rspamd_config *cfg, gint *ref_id)
{
	gint err_idx;
	struct rspamd_config **pcfg;

	lua_pushcfunction (L, &rspamd_lua_traceback);
	err_idx = lua_gettop (L);

	/* Obtain function */
	if (!rspamd_lua_require_function (L, "lua_redis", "try_load_redis_servers")) {
		msg_err_config ("cannot require lua_redis");
		lua_pop (L, 2);

		return FALSE;
	}

	/* Function arguments */
	ucl_object_push_lua (L, obj, false);
	pcfg = lua_newuserdata (L, sizeof (*pcfg));
	rspamd_lua_setclass (L, "rspamd{config}", -1);
	*pcfg = cfg;
	lua_pushboolean (L, false); /* no_fallback */

	if (lua_pcall (L, 3, 1, err_idx) != 0) {
		GString *tb;

		tb = lua_touserdata (L, -1);
		msg_err_config ("cannot call lua try_load_redis_servers script: %s", tb->str);
		g_string_free (tb, TRUE);
		lua_settop (L, 0);

		return FALSE;
	}

	if (lua_istable (L, -1)) {
		if (ref_id) {
			/* Ref table */
			lua_pushvalue (L, -1);
			*ref_id = luaL_ref (L, LUA_REGISTRYINDEX);
			lua_settop (L, 0);
		}
		else {
			/* Leave it on the stack */
			lua_insert (L, err_idx);
			lua_settop (L, err_idx);
		}

		return TRUE;
	}
	else {
		lua_settop (L, 0);
	}

	return FALSE;
}

void
rspamd_lua_push_full_word (lua_State *L, rspamd_stat_token_t *w)
{
	gint fl_cnt;

	lua_createtable (L, 4, 0);

	if (w->stemmed.len > 0) {
		lua_pushlstring (L, w->stemmed.begin, w->stemmed.len);
		lua_rawseti (L, -2, 1);
	}
	else {
		lua_pushstring (L, "");
		lua_rawseti (L, -2, 1);
	}

	if (w->normalized.len > 0) {
		lua_pushlstring (L, w->normalized.begin, w->normalized.len);
		lua_rawseti (L, -2, 2);
	}
	else {
		lua_pushstring (L, "");
		lua_rawseti (L, -2, 2);
	}

	if (w->original.len > 0) {
		lua_pushlstring (L, w->original.begin, w->original.len);
		lua_rawseti (L, -2, 3);
	}
	else {
		lua_pushstring (L, "");
		lua_rawseti (L, -2, 3);
	}

	/* Flags part */
	fl_cnt = 1;
	lua_createtable (L, 4, 0);

	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_NORMALISED) {
		lua_pushstring (L, "normalised");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_BROKEN_UNICODE) {
		lua_pushstring (L, "broken_unicode");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_UTF) {
		lua_pushstring (L, "utf");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_TEXT) {
		lua_pushstring (L, "text");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_HEADER) {
		lua_pushstring (L, "header");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & (RSPAMD_STAT_TOKEN_FLAG_META|RSPAMD_STAT_TOKEN_FLAG_LUA_META)) {
		lua_pushstring (L, "meta");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_STOP_WORD) {
		lua_pushstring (L, "stop_word");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_INVISIBLE_SPACES) {
		lua_pushstring (L, "invisible_spaces");
		lua_rawseti (L, -2, fl_cnt ++);
	}
	if (w->flags & RSPAMD_STAT_TOKEN_FLAG_STEMMED) {
		lua_pushstring (L, "stemmed");
		lua_rawseti (L, -2, fl_cnt ++);
	}

	lua_rawseti (L, -2, 4);
}

gint
rspamd_lua_push_words (lua_State *L, GArray *words,
							enum rspamd_lua_words_type how)
{
	rspamd_stat_token_t *w;
	guint i, cnt;

	lua_createtable (L, words->len, 0);

	for (i = 0, cnt = 1; i < words->len; i ++) {
		w = &g_array_index (words, rspamd_stat_token_t, i);

		switch (how) {
		case RSPAMD_LUA_WORDS_STEM:
			if (w->stemmed.len > 0) {
				lua_pushlstring (L, w->stemmed.begin, w->stemmed.len);
				lua_rawseti (L, -2, cnt ++);
			}
			break;
		case RSPAMD_LUA_WORDS_NORM:
			if (w->normalized.len > 0) {
				lua_pushlstring (L, w->normalized.begin, w->normalized.len);
				lua_rawseti (L, -2, cnt ++);
			}
			break;
		case RSPAMD_LUA_WORDS_RAW:
			if (w->original.len > 0) {
				lua_pushlstring (L, w->original.begin, w->original.len);
				lua_rawseti (L, -2, cnt ++);
			}
			break;
		case RSPAMD_LUA_WORDS_FULL:
			rspamd_lua_push_full_word (L, w);
			/* Push to the resulting vector */
			lua_rawseti (L, -2, cnt ++);
			break;
		}
	}

	return 1;
}