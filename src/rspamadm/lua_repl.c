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

#include "config.h"
#include "rspamadm.h"
#include "libutil/http_connection.h"
#include "libutil/http_private.h"
#include "libutil/http_router.h"
#include "printf.h"
#include "lua/lua_common.h"
#include "lua/lua_thread_pool.h"
#include "message.h"
#include "unix-std.h"
#include "linenoise.h"
#include "worker_util.h"
#ifdef WITH_LUAJIT
#include <luajit.h>
#endif

static gchar **paths = NULL;
static gchar **scripts = NULL;
static gchar **lua_args = NULL;
static gchar *histfile = NULL;
static guint max_history = 2000;
static gchar *serve = NULL;
static gchar *exec_line = NULL;
static gint batch = -1;
static gboolean per_line = FALSE;
extern struct rspamd_async_session *rspamadm_session;

static const char *default_history_file = ".rspamd_repl.hist";

#ifdef WITH_LUAJIT
#define MAIN_PROMPT LUAJIT_VERSION "> "
#else
#define MAIN_PROMPT LUA_VERSION "> "
#endif
#define MULTILINE_PROMPT "... "

static void rspamadm_lua (gint argc, gchar **argv,
		const struct rspamadm_command *cmd);
static const char *rspamadm_lua_help (gboolean full_help,
									  const struct rspamadm_command *cmd);

struct rspamadm_command lua_command = {
		.name = "lua",
		.flags = 0,
		.help = rspamadm_lua_help,
		.run = rspamadm_lua,
		.lua_subrs = NULL,
};

/*
 * Dot commands
 */
typedef void (*rspamadm_lua_dot_handler)(lua_State *L, gint argc, gchar **argv);
struct rspamadm_lua_dot_command {
	const gchar *name;
	const gchar *description;
	rspamadm_lua_dot_handler handler;
};

static void rspamadm_lua_help_handler (lua_State *L, gint argc, gchar **argv);
static void rspamadm_lua_load_handler (lua_State *L, gint argc, gchar **argv);
static void rspamadm_lua_exec_handler (lua_State *L, gint argc, gchar **argv);
static void rspamadm_lua_message_handler (lua_State *L, gint argc, gchar **argv);

static void lua_thread_error_cb (struct thread_entry *thread, int ret, const char *msg);
static void lua_thread_finish_cb (struct thread_entry *thread, int ret);

static struct rspamadm_lua_dot_command cmds[] = {
	{
		.name = "help",
		.description = "shows help for commands",
		.handler = rspamadm_lua_help_handler
	},
	{
		.name = "load",
		.description = "load lua file",
		.handler = rspamadm_lua_load_handler
	},
	{
		.name = "exec",
		.description = "exec lua file",
		.handler = rspamadm_lua_exec_handler
	},
	{
		.name = "message",
		.description = "scans message using specified callback: .message <callback_name> <file>...",
		.handler = rspamadm_lua_message_handler
	},
};

static GHashTable *cmds_hash = NULL;

static GOptionEntry entries[] = {
		{"script", 's', 0, G_OPTION_ARG_STRING_ARRAY, &scripts,
				"Load specified scripts", NULL},
		{"path", 'P', 0, G_OPTION_ARG_STRING_ARRAY, &paths,
				"Add specified paths to lua paths", NULL},
		{"history-file", 'H', 0, G_OPTION_ARG_FILENAME, &histfile,
				"Load history from the specified file", NULL},
		{"max-history", 'm', 0, G_OPTION_ARG_INT, &max_history,
				"Store this number of history entries", NULL},
		{"serve", 'S', 0, G_OPTION_ARG_STRING, &serve,
				"Serve http lua server", NULL},
		{"batch", 'b', 0, G_OPTION_ARG_NONE, &batch,
				"Batch execution mode", NULL},
		{"per-line", 'p', 0, G_OPTION_ARG_NONE, &per_line,
				"Pass each line of input to the specified lua script", NULL},
		{"exec", 'e', 0, G_OPTION_ARG_STRING, &exec_line,
				"Execute specified script", NULL},
		{"args", 'a', 0, G_OPTION_ARG_STRING_ARRAY, &lua_args,
				"Arguments to pass to Lua", NULL},
		{NULL,       0,   0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static const char *
rspamadm_lua_help (gboolean full_help, const struct rspamadm_command *cmd)
{
	const char *help_str;

	if (full_help) {
		help_str = "Run lua read/execute/print loop\n\n"
				"Usage: rspamadm lua [-P paths] [-s scripts]\n"
				"Where options are:\n\n"
				"-P: add additional lua paths (may be repeated)\n"
				"-p: split input to lines and feed each line to the script\n"
				"-s: load scripts on start from specified files (may be repeated)\n"
				"-S: listen on a specified address as HTTP server\n"
				"-a: pass argument to lua (may be repeated)\n"
				"-e: execute script specified in command line"
				"--help: shows available options and commands";
	}
	else {
		help_str = "Run LUA interpreter";
	}

	return help_str;
}

static void
rspamadm_lua_add_path (lua_State *L, const gchar *path)
{
	const gchar *old_path;
	gsize len;
	GString *new_path;

	lua_getglobal (L, "package");
	lua_getfield (L, -1, "path");
	old_path = luaL_checklstring (L, -1, &len);

	new_path = g_string_sized_new (len + strlen (path) + sizeof("/?.lua"));

	if (strstr (path, "?.lua") == NULL) {
		rspamd_printf_gstring (new_path, "%s/?.lua;%s", path, old_path);
	}
	else {
		rspamd_printf_gstring (new_path, "%s;%s", path, old_path);
	}

	lua_pushlstring (L, new_path->str, new_path->len);
	lua_setfield (L, -2, "path");
	lua_settop (L, 0);
	g_string_free (new_path, TRUE);
}


static void
lua_thread_finish_cb (struct thread_entry *thread, int ret)
{
	struct lua_call_data *cd = thread->cd;

	cd->ret = ret;
}

static void
lua_thread_error_cb (struct thread_entry *thread, int ret, const char *msg)
{
	struct lua_call_data *cd = thread->cd;

	rspamd_fprintf (stderr, "call failed: %s\n", msg);

	cd->ret = ret;
}

static void
lua_thread_str_error_cb (struct thread_entry *thread, int ret, const char *msg)
{
	struct lua_call_data *cd = thread->cd;
	const char *what = cd->ud;

	rspamd_fprintf (stderr, "call to %s failed: %s\n", what, msg);

	cd->ret = ret;
}

static gboolean
rspamadm_lua_load_script (lua_State *L, const gchar *path)
{
	struct thread_entry *thread = lua_thread_pool_get_for_config (rspamd_main->cfg);
	L = thread->lua_state;

	if (luaL_loadfile (L, path) != 0) {
		rspamd_fprintf (stderr, "cannot load script %s: %s\n",
				path, lua_tostring (L, -1));
		lua_settop (L, 0);

		return FALSE;
	}

	if (!per_line) {

		if (lua_repl_thread_call (thread, 0, (void *)path, lua_thread_str_error_cb) != 0) {
			return FALSE;
		}

		lua_settop (L, 0);
	}

	return TRUE;
}

static void
rspamadm_exec_input (lua_State *L, const gchar *input)
{
	GString *tb;
	gint i, cbref;
	int top = 0;
	gchar outbuf[8192];
	struct lua_logger_trace tr;

	struct thread_entry *thread = lua_thread_pool_get_for_config (rspamd_main->cfg);
	L = thread->lua_state;

	/* First try return + input */
	tb = g_string_sized_new (strlen (input) + sizeof ("return "));
	rspamd_printf_gstring (tb, "return %s", input);

	int r = luaL_loadstring (L, tb->str);
	if (r != 0) {
		/* Reset stack */
		lua_settop (L, 0);
		/* Try with no return */
		if (luaL_loadstring (L, input) != 0) {
			rspamd_fprintf (stderr, "cannot load string %s\n",
					input);
			g_string_free (tb, TRUE);
			lua_settop (L, 0);

			lua_thread_pool_return (rspamd_main->cfg->lua_thread_pool, thread);
			return;
		}
	}

	g_string_free (tb, TRUE);

	if (!per_line) {

		top = lua_gettop (L);

		if (lua_repl_thread_call (thread, 0, NULL, NULL) == 0) {
			/* Print output */
			for (i = top; i <= lua_gettop (L); i++) {
				if (lua_isfunction (L, i)) {
					lua_pushvalue (L, i);
					cbref = luaL_ref (L, LUA_REGISTRYINDEX);

					rspamd_printf ("local function: %d\n", cbref);
				} else {
					memset (&tr, 0, sizeof (tr));
					lua_logger_out_type (L, i, outbuf, sizeof (outbuf), &tr,
							LUA_ESCAPE_UNPRINTABLE);
					rspamd_printf ("%s\n", outbuf);
				}
			}
		}
	}
}

static void
wait_session_events (void)
{
	/* XXX: it's probably worth to add timeout here - not to wait forever */
	while (rspamd_session_events_pending (rspamadm_session) > 0) {
		event_base_loop (rspamd_main->ev_base, EVLOOP_ONCE);
	}
}

gint
lua_repl_thread_call (struct thread_entry *thread, gint narg, gpointer ud, lua_thread_error_t error_func)
{
	int ret;
	struct lua_call_data *cd = g_new0 (struct lua_call_data, 1);
	cd->top = lua_gettop (thread->lua_state);
	cd->ud = ud;

	thread->finish_callback = lua_thread_finish_cb;
	if (error_func) {
		thread->error_callback = error_func;
	}
	else {
		thread->error_callback = lua_thread_error_cb;
	}
	thread->cd = cd;

	lua_thread_call (thread, narg);

	wait_session_events ();

	ret = cd->ret;

	g_free (cd);

	return ret;
}

static void
rspamadm_lua_help_handler (lua_State *L, gint argc, gchar **argv)
{
	guint i;
	struct rspamadm_lua_dot_command *cmd;

	if (argv[1] == NULL) {
		/* Print all commands */
		for (i = 0; i < G_N_ELEMENTS (cmds); i ++) {
			rspamd_printf ("%s: %s\n", cmds[i].name, cmds[i].description);
		}

		rspamd_printf ("{{: start multiline input\n");
		rspamd_printf ("}}: end multiline input\n");
	}
	else {
		for (i = 1; argv[i] != NULL; i ++) {
			cmd = g_hash_table_lookup (cmds_hash, argv[i]);

			if (cmd) {
				rspamd_printf ("%s: %s\n", cmds->name, cmds->description);
			}
			else {
				rspamd_printf ("%s: no such command\n", argv[i]);
			}
		}
	}
}

static void
rspamadm_lua_load_handler (lua_State *L, gint argc, gchar **argv)
{
	guint i;
	gboolean ret;

	for (i = 1; argv[i] != NULL; i ++) {
		ret = rspamadm_lua_load_script (L, argv[i]);
		rspamd_printf ("%s: %sloaded\n", argv[i], ret ? "" : "NOT ");
	}
}

static void
rspamadm_lua_exec_handler (lua_State *L, gint argc, gchar **argv)
{
	gint i;

	struct thread_entry *thread = lua_thread_pool_get_for_config (rspamd_main->cfg);
	L = thread->lua_state;

	for (i = 1; argv[i] != NULL; i ++) {

		if (luaL_loadfile (L, argv[i]) != 0) {
			rspamd_fprintf (stderr, "cannot load script %s: %s\n",
					argv[i], lua_tostring (L, -1));
			lua_settop (L, 0);

			return;
		}

		lua_repl_thread_call (thread, 0, argv[i], lua_thread_str_error_cb);
	}
}

static void
rspamadm_lua_message_handler (lua_State *L, gint argc, gchar **argv)
{
	gulong cbref;
	gint old_top, func_idx, i, j;
	struct rspamd_task *task, **ptask;
	gpointer map;
	gsize len;
	gchar outbuf[8192];
	struct lua_logger_trace tr;

	if (argv[1] == NULL) {
		rspamd_printf ("no callback is specified\n");
		return;
	}

	for (i = 2; argv[i] != NULL; i ++) {
		struct thread_entry *thread = lua_thread_pool_get_for_config (rspamd_main->cfg);
		L = thread->lua_state;

		if (rspamd_strtoul (argv[1], strlen (argv[1]), &cbref)) {
			lua_rawgeti (L, LUA_REGISTRYINDEX, cbref);
		}
		else {
			lua_getglobal (L, argv[1]);
		}

		if (lua_type (L, -1) != LUA_TFUNCTION) {
			rspamd_printf ("bad callback type: %s\n", lua_typename (L, lua_type (L, -1)));
			lua_thread_pool_return (rspamd_main->cfg->lua_thread_pool, thread);
			return;
		}

		/* Save index to reuse */
		func_idx = lua_gettop (L);

		map = rspamd_file_xmap (argv[i], PROT_READ, &len, TRUE);

		if (map == NULL) {
			rspamd_printf ("cannot open %s: %s\n", argv[i], strerror (errno));
		}
		else {
			task = rspamd_task_new (NULL, rspamd_main->cfg, NULL, NULL, NULL);

			if (!rspamd_task_load_message (task, NULL, map, len)) {
				rspamd_printf ("cannot load %s\n", argv[i]);
				rspamd_task_free (task);
				munmap (map, len);
				continue;
			}

			if (!rspamd_message_parse (task)) {
				rspamd_printf ("cannot parse %s: %e\n", argv[i], task->err);
				rspamd_task_free (task);
				munmap (map, len);
				continue;
			}

			rspamd_message_process (task);
			old_top = lua_gettop (L);

			lua_pushvalue (L, func_idx);
			ptask = lua_newuserdata (L, sizeof (*ptask));
			*ptask = task;
			rspamd_lua_setclass (L, "rspamd{task}", -1);


			if (lua_repl_thread_call (thread, 1, argv[i], lua_thread_str_error_cb) == 0) {
				rspamd_printf ("lua callback for %s returned:\n", argv[i]);

				for (j = old_top + 1; j <= lua_gettop (L); j ++) {
					memset (&tr, 0, sizeof (tr));
					lua_logger_out_type (L, j, outbuf, sizeof (outbuf), &tr,
							LUA_ESCAPE_UNPRINTABLE);
					rspamd_printf ("%s\n", outbuf);
				}
			}

			rspamd_task_free (task);
			munmap (map, len);
			/* Pop all but the original function */
			lua_settop (L, func_idx);
		}
	}

	lua_settop (L, 0);
}


static gboolean
rspamadm_lua_try_dot_command (lua_State *L, const gchar *input)
{
	struct rspamadm_lua_dot_command *cmd;
	gchar **argv;

	argv = g_strsplit_set (input + 1, " ", -1);

	if (argv == NULL || argv[0] == NULL) {
		if (argv) {
			g_strfreev (argv);
		}

		return FALSE;
	}

	cmd = g_hash_table_lookup (cmds_hash, argv[0]);

	if (cmd) {
		cmd->handler (L, g_strv_length (argv), argv);
		g_strfreev (argv);

		return TRUE;
	}

	g_strfreev (argv);

	return FALSE;
}

static void
rspamadm_lua_run_repl (lua_State *L)
{
	gchar *input;
	gboolean is_multiline = FALSE;
	GString *tb;
	guint i;

	for (;;) {
		if (!is_multiline) {
			input = linenoise (MAIN_PROMPT);

			if (input == NULL) {
				return;
			}

			if (input[0] == '.') {
				if (rspamadm_lua_try_dot_command (L, input)) {
					linenoiseHistoryAdd (input);
					linenoiseFree (input);
					continue;
				}
			}

			if (strcmp (input, "{{") == 0) {
				is_multiline = TRUE;
				linenoiseFree (input);
				tb = g_string_sized_new (8192);
				continue;
			}

			rspamadm_exec_input (L, input);
			linenoiseHistoryAdd (input);
			linenoiseFree (input);
			lua_settop (L, 0);
		}
		else {
			input = linenoise (MULTILINE_PROMPT);

			if (input == NULL) {
				g_string_free (tb, TRUE);
				return;
			}

			if (strcmp (input, "}}") == 0) {
				is_multiline = FALSE;
				linenoiseFree (input);
				rspamadm_exec_input (L, tb->str);

				/* Replace \n with ' ' for sanity */
				for (i = 0; i < tb->len; i ++) {
					if (tb->str[i] == '\n') {
						tb->str[i] = ' ';
					}
				}

				linenoiseHistoryAdd (tb->str);
				g_string_free (tb, TRUE);
			}
			else {
				g_string_append (tb, input);
				g_string_append (tb, " \n");
				linenoiseFree (input);
			}
		}
	}
}

struct rspamadm_lua_repl_context {
	struct rspamd_http_connection_router *rt;
	lua_State *L;
};

struct rspamadm_lua_repl_session {
	struct rspamd_http_connection_router *rt;
	rspamd_inet_addr_t *addr;
	struct rspamadm_lua_repl_context *ctx;
	gint sock;
};

static void
rspamadm_lua_accept_cb (gint fd, short what, void *arg)
{
	struct rspamadm_lua_repl_context *ctx = arg;
	rspamd_inet_addr_t *addr;
	struct rspamadm_lua_repl_session *session;
	gint nfd;

	if ((nfd =
			rspamd_accept_from_socket (fd, &addr, NULL)) == -1) {
		rspamd_fprintf (stderr, "accept failed: %s", strerror (errno));
		return;
	}
	/* Check for EAGAIN */
	if (nfd == 0) {
		return;
	}

	session = g_malloc0 (sizeof (*session));
	session->rt = ctx->rt;
	session->ctx = ctx;
	session->addr = addr;
	session->sock = nfd;

	rspamd_http_router_handle_socket (ctx->rt, nfd, session);
}

static void
rspamadm_lua_error_handler (struct rspamd_http_connection_entry *conn_ent,
	GError *err)
{
	rspamd_fprintf (stderr, "http error occurred: %s\n", err->message);
}

static void
rspamadm_lua_finish_handler (struct rspamd_http_connection_entry *conn_ent)
{
	struct rspamadm_lua_repl_session *session = conn_ent->ud;

	g_free (session);
}

static void
lua_thread_http_error_cb (struct thread_entry *thread, int ret, const char *msg)
{
	struct lua_call_data *cd = thread->cd;
	struct rspamd_http_connection_entry *conn_ent = cd->ud;

	rspamd_controller_send_error (conn_ent, 500, "call failed: %s\n", msg);

	cd->ret = ret;
}


/*
 * Exec command handler:
 * request: /exec
 * body: lua script
 * reply: json {"status": "ok", "reply": {<lua json object>}}
 */
static int
rspamadm_lua_handle_exec (struct rspamd_http_connection_entry *conn_ent,
	struct rspamd_http_message *msg)
{
	GString *tb;
	gint err_idx, i;
	lua_State *L;
	struct rspamadm_lua_repl_context *ctx;
	struct rspamadm_lua_repl_session *session = conn_ent->ud;
	ucl_object_t *obj, *elt;
	const gchar *body;
	gsize body_len;

	ctx = session->ctx;

	struct thread_entry *thread = lua_thread_pool_get_for_config (rspamd_main->cfg);
	L = thread->lua_state;

	body = rspamd_http_message_get_body (msg, &body_len);

	if (body == NULL) {
		rspamd_controller_send_error (conn_ent, 400, "Empty lua script");

		return 0;
	}

	lua_pushcfunction (L, &rspamd_lua_traceback);
	err_idx = lua_gettop (L);

	/* First try return + input */
	tb = g_string_sized_new (body_len + sizeof ("return "));
	rspamd_printf_gstring (tb, "return %*s", (gint)body_len, body);

	if (luaL_loadstring (L, tb->str) != 0) {
		/* Reset stack */
		lua_settop (L, 0);
		lua_pushcfunction (L, &rspamd_lua_traceback);
		err_idx = lua_gettop (L);
		/* Try with no return */
		if (luaL_loadbuffer (L, body, body_len, "http input") != 0) {
			rspamd_controller_send_error (conn_ent, 400, "Invalid lua script");

			return 0;
		}
	}

	g_string_free (tb, TRUE);

	if (lua_repl_thread_call (thread, 0, conn_ent, lua_thread_http_error_cb) != 0) {
		return 0;
	}

	obj = ucl_object_typed_new (UCL_ARRAY);

	for (i = err_idx + 1; i <= lua_gettop (L); i ++) {
		if (lua_isfunction (L, i)) {
			/* XXX: think about API */
		}
		else {
			elt = ucl_object_lua_import (L, i);

			if (elt) {
				ucl_array_append (obj, elt);
			}
		}
	}

	rspamd_controller_send_ucl (conn_ent, obj);
	ucl_object_unref (obj);
	lua_settop (L, 0);

	return 0;
}

static void
rspamadm_lua (gint argc, gchar **argv, const struct rspamadm_command *cmd)
{
	GOptionContext *context;
	GError *error = NULL;
	gchar **elt;
	guint i;
	lua_State *L = rspamd_main->cfg->lua_state;

	context = g_option_context_new ("lua - run lua interpreter");
	g_option_context_set_summary (context,
			"Summary:\n  Rspamd administration utility version "
					RVERSION
					"\n  Release id: "
					RID);
	g_option_context_add_main_entries (context, entries, NULL);

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		fprintf (stderr, "option parsing failed: %s\n", error->message);
		g_error_free (error);
		exit (1);
	}

	if (batch == -1) {
		if (isatty (STDIN_FILENO)) {
			batch = 0;
		}
		else {
			batch = 1;
		}
	}

	if (paths) {
		for (elt = paths; *elt != NULL; elt ++) {
			rspamadm_lua_add_path (L, *elt);
		}
	}

	if (lua_args) {
		i = 1;

		lua_newtable (L);

		for (elt = lua_args; *elt != NULL; elt ++) {
			lua_pushinteger (L, i);
			lua_pushstring (L, *elt);
			lua_settable (L, -3);
			i++;
		}

		lua_setglobal (L, "arg");
	}

	if (scripts) {
		for (elt = scripts; *elt != NULL; elt ++) {
			if (!rspamadm_lua_load_script (L, *elt)) {
				exit (EXIT_FAILURE);
			}
		}
	}

	if (exec_line) {
		rspamadm_exec_input (L, exec_line);
	}

	if (serve) {
		/* HTTP Server mode */
		GPtrArray *addrs = NULL;
		gchar *name = NULL;
		struct event_base *ev_base;
		struct rspamd_http_connection_router *http;
		gint fd;
		struct rspamadm_lua_repl_context *ctx;

		if (!rspamd_parse_host_port_priority (serve, &addrs, NULL, &name,
				10000, NULL)) {
			fprintf (stderr, "cannot listen on %s", serve);
			exit (EXIT_FAILURE);
		}

		ev_base = rspamd_main->ev_base;
		ctx = g_malloc0  (sizeof (*ctx));
		http = rspamd_http_router_new (rspamadm_lua_error_handler,
						rspamadm_lua_finish_handler,
						NULL,
						NULL,
						rspamd_main->http_ctx);
		ctx->L = L;
		ctx->rt = http;
		rspamd_http_router_add_path (http,
				"/exec",
				rspamadm_lua_handle_exec);

		for (i = 0; i < addrs->len; i ++) {
			rspamd_inet_addr_t *addr = g_ptr_array_index (addrs, i);

			fd = rspamd_inet_address_listen (addr, SOCK_STREAM, TRUE);
			if (fd != -1) {
				struct event *ev;

				ev = g_malloc0 (sizeof (*ev));
				event_set (ev, fd, EV_READ|EV_PERSIST, rspamadm_lua_accept_cb,
						ctx);
				event_base_set (ev_base, ev);
				event_add (ev, NULL);
				rspamd_printf ("listen on %s\n",
						rspamd_inet_address_to_string_pretty (addr));
			}
		}

		event_base_loop (ev_base, 0);

		exit (EXIT_SUCCESS);
	}

	if (histfile == NULL) {
		const gchar *homedir;
		GString *hist_path;

		homedir = getenv ("HOME");

		if (homedir) {
			hist_path = g_string_sized_new (strlen (homedir) +
					strlen (default_history_file) + 1);
			rspamd_printf_gstring (hist_path, "%s/%s", homedir,
					default_history_file);
		}
		else {
			hist_path = g_string_sized_new (strlen (default_history_file) + 2);
			rspamd_printf_gstring (hist_path, "./%s", default_history_file);
		}

		histfile = hist_path->str;
		g_string_free (hist_path, FALSE);
	}

	if (argc > 1) {
		for (i = 1; i < argc; i ++) {
			if (!rspamadm_lua_load_script (L, argv[i])) {
				exit (EXIT_FAILURE);
			}
		}

		exit (EXIT_SUCCESS);
	}

	/* Init dot commands */
	cmds_hash = g_hash_table_new (rspamd_strcase_hash, rspamd_strcase_equal);

	for (i = 0; i < G_N_ELEMENTS (cmds); i ++) {
		g_hash_table_insert (cmds_hash, (gpointer)cmds[i].name, &cmds[i]);
	}

	if (per_line) {
		GIOChannel *in;
		GString *buf;
		gsize end_pos;
		GIOStatus ret;
		gint old_top;
		GError *err = NULL;

		in = g_io_channel_unix_new (STDIN_FILENO);
		buf = g_string_sized_new (BUFSIZ);

again:
		while ((ret = g_io_channel_read_line_string (in, buf, &end_pos, &err)) ==
				G_IO_STATUS_NORMAL) {
			old_top = lua_gettop (L);
			lua_pushvalue (L, -1);
			lua_pushlstring (L, buf->str, MIN (buf->len, end_pos));
			lua_setglobal (L, "input");

			struct thread_entry *thread = lua_thread_pool_get_for_config (rspamd_main->cfg);
			L = thread->lua_state;

			lua_repl_thread_call (thread, 0, NULL, NULL);

			lua_settop (L, old_top);
		}

		if (ret == G_IO_STATUS_AGAIN) {
			goto again;
		}

		g_string_free (buf, TRUE);
		g_io_channel_shutdown (in, FALSE, NULL);

		if (ret == G_IO_STATUS_EOF) {
			if (err) {
				g_error_free (err);
			}
		}
		else {
			rspamd_fprintf (stderr, "IO error: %e\n", err);

			if (err) {
				g_error_free (err);
			}

			exit (-errno);
		}
	}
	else {
		if (!batch) {
			linenoiseHistorySetMaxLen (max_history);
			linenoiseHistoryLoad (histfile);
			rspamadm_lua_run_repl (L);
			linenoiseHistorySave (histfile);
		} else {
			rspamadm_lua_run_repl (L);
		}
	}
}
