
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#include "plugin.h"
#include "log.h"
#include "status_counter.h"

#include "mod_proxy_core_spawn.h"

#define PROXY_CORE "proxy-core"
#define CONFIG_PROXY_CORE_SPAWN            PROXY_CORE ".spawn"

#define MAX_CONNECT_FAILEDS 3

#if !USE_COUNTER
#undef COUNTER_SET
#define COUNTER_SET(x,y) do {} while(0)
#undef COUNTER_INC
#define COUNTER_INC(x) do {} while(0)
#endif

static proxy_spawn_binary* proxy_spawn_binary_init(buffer *bin_path) {
	char *start, *str;
	size_t len;
	proxy_spawn_binary *binary = calloc(1, sizeof(*binary));

	binary->arg_string = buffer_init();
	binary->base_dir = buffer_init();

	// split args
	buffer_copy_string_buffer(binary->arg_string, bin_path);

	str = start = binary->arg_string->ptr;
	len = binary->arg_string->used - 1;
	for (size_t i = 0; i < len; i++) {
		switch (str[i]) {
			case ' ':
			case '\t':
				str[i] = 0;
				ARRAY_STATIC_PREPARE_APPEND(binary);
				binary->ptr[binary->used++] = start;
				start = &str[i + 1];
				break;
			default:
				break;
		}
	}

	ARRAY_STATIC_PREPARE_APPEND(binary);
	binary->ptr[binary->used++] = start;

	ARRAY_STATIC_PREPARE_APPEND(binary);
	binary->ptr[binary->used++] = 0;

	// find basedir of binary
	if (NULL != (start = strrchr(str, '/'))) {
		buffer_copy_string_len(binary->base_dir, str, (size_t) (start - str));
	}

	return binary;
}

static void proxy_spawn_binary_free(proxy_spawn_binary* binary) {
	if (!binary) return;
	free(binary->ptr);
	buffer_free(binary->arg_string);
	buffer_free(binary->base_dir);
	free(binary);
}

// spawn_proc
static void proxy_spawn_proc_start(proxy_spawn_proc *proc);
static void proxy_spawn_proc_change_state(proxy_spawn_proc *proc, proxy_core_spawn_proc_state state, time_t timeout);

static proxy_spawn_proc* proxy_spawn_proc_init(proxy_spawn *spawn, proxy_address *base_address, size_t offset) {
	proxy_spawn_proc *proc;

	proc = calloc(1, sizeof(*proc));
	proc->address = proxy_address_offset_instance(base_address, offset);
	proc->spawn = spawn;

	INIT_LIST_HEAD(&proc->list);
	INIT_LIST_HEAD(&proc->list_idle);
	proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_INIT, 0);

#if USE_COUNTER
	buffer *b;
	b = buffer_init();
#define COUNTER_NAME(x) \
	buffer_copy_string_len(b, CONST_STR_LEN(CONFIG_PROXY_CORE_SPAWN ".")); \
	buffer_append_string_len(b, CONST_STR_LEN("\"")); \
	buffer_append_string_buffer(b, spawn->key); \
	buffer_append_string_len(b, CONST_STR_LEN("\".")); \
	buffer_append_long(b, offset); \
	buffer_append_string_len(b, CONST_STR_LEN("." x));

	/* request count stat. */
	COUNTER_NAME("load");
	proc->stat_load = status_counter_get_counter(CONST_BUF_LEN(b));

	COUNTER_NAME("requests");
	proc->stat_requests = status_counter_get_counter(CONST_BUF_LEN(b));

	COUNTER_NAME("requests_failed");
	proc->stat_requests_failed = status_counter_get_counter(CONST_BUF_LEN(b));

	COUNTER_NAME("state");
	proc->stat_state = status_counter_get_counter(CONST_BUF_LEN(b));

	COUNTER_NAME("acttime");
	proc->stat_act_time = status_counter_get_counter(CONST_BUF_LEN(b));

	COUNTER_NAME("curtime");
	proc->stat_cur_time = status_counter_get_counter(CONST_BUF_LEN(b));

	buffer_free(b);
#undef COUNTER_NAME
#endif

	return proc;
}

static void proxy_spawn_proc_kill(proxy_spawn_proc *proc) {
	if (!proc) return;

	switch (proc->state) {
	case PROXY_CORE_SPAWN_PROC_INIT:
	case PROXY_CORE_SPAWN_PROC_KILLED:
	case PROXY_CORE_SPAWN_PROC_REMOVED:
		break;
	case PROXY_CORE_SPAWN_PROC_ACTIVE:
	case PROXY_CORE_SPAWN_PROC_DISABLED:
		kill(proc->pid, SIGTERM);
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_KILLED, 0);
		break;
	}
}

static void proxy_spawn_proc_free(proxy_spawn_proc *proc) {
	if (!proc) return;

	proxy_spawn_proc_kill(proc);

	list_del_init(&proc->list);

	proxy_address_free(proc->address);
	free(proc);
}

// State machine for proc:

static void proxy_spawn_proc_update_idle(proxy_spawn_proc *proc) {
	// Restart idle timeout (or stop it)
	list_head *head = &proc->spawn->cfg->proc_idle, *i;
	proxy_spawn_proc *p;

	// Stop it
	list_del_init(&proc->list_idle);

	// New idle timeout
	if (proc->state == PROXY_CORE_SPAWN_PROC_ACTIVE && proc->load == 0 && proc->spawn->active_procs > proc->spawn->min_procs) {
		proc->action_time = time(0) + proc->spawn->idle_time;
		list_for_each_reverse(i, head) {
			p = list_entry(i, proxy_spawn_proc, list_idle);
			if (p->action_time >= proc->action_time) break;
		}
		list_add(&proc->list_idle, i);
	}
}

static inline void proc_list_add_timed(proxy_spawn_proc *proc, list_head *head) {
	list_head *i;
	proxy_spawn_proc *p;

	list_for_each_reverse(i, head) {
		p = list_entry(i, proxy_spawn_proc, list);
		if (p->action_time >= proc->action_time) break;
	}
	list_add(&proc->list, i);
}

static void proxy_spawn_proc_change_state(proxy_spawn_proc *proc, proxy_core_spawn_proc_state state, time_t timeout) {
	proxy_core_spawn_proc_state ostate = proc->state;

	/* Remove from previous list */
	list_del_init(&proc->list);
	proc->action_time = time(0) + timeout;
	COUNTER_SET(proc->stat_act_time, proc->action_time);

	proc->state = state;
	COUNTER_SET(proc->stat_state, state);

	proxy_spawn_proc_update_idle(proc);

	switch (proc->state) {
	case PROXY_CORE_SPAWN_PROC_INIT:
		list_add_tail(&proc->list, &proc->spawn->proc_init);
		if (proc->spawn->active_procs < proc->spawn->min_procs) {
			proxy_spawn_proc_start(proc);
			return;
		}
		break;
	case PROXY_CORE_SPAWN_PROC_ACTIVE:
		if (ostate == PROXY_CORE_SPAWN_PROC_INIT) {
			proc->spawn->active_procs++;
		}
		list_add(&proc->list, &proc->spawn->proc_active);
		break;
	case PROXY_CORE_SPAWN_PROC_DISABLED:
		proc_list_add_timed(proc, &proc->spawn->cfg->proc_disabled);
		break;
	case PROXY_CORE_SPAWN_PROC_KILLED:
		if (ostate != PROXY_CORE_SPAWN_PROC_KILLED && ostate != PROXY_CORE_SPAWN_PROC_REMOVED && ostate != PROXY_CORE_SPAWN_PROC_INIT) {
			proc->spawn->active_procs--;
		}
		list_add_tail(&proc->list, &proc->spawn->cfg->proc_killed);
		break;
	case PROXY_CORE_SPAWN_PROC_REMOVED:
		proc_list_add_timed(proc, &proc->spawn->proc_removed);
		break;
	}
}

static void proxy_spawn_proc_check_killed(proxy_spawn_proc *proc) {
	if (!proc) return;
	
	switch (proc->state) {
	case PROXY_CORE_SPAWN_PROC_INIT:
	case PROXY_CORE_SPAWN_PROC_REMOVED:
		break;
	case PROXY_CORE_SPAWN_PROC_ACTIVE:
	case PROXY_CORE_SPAWN_PROC_DISABLED:
	case PROXY_CORE_SPAWN_PROC_KILLED:
		if (waitpid(proc->pid, 0, WNOHANG) != 0) {
			proc->pid = -1;
			if (proc->failed_connect < MAX_CONNECT_FAILEDS) {
				proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_INIT, 0);
			} else {
				proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_REMOVED, 300); // 5 min
			}
		}
	}
}

static void proxy_spawn_proc_check_idle(proxy_spawn_proc *proc) {
	// Stop it
	list_del_init(&proc->list_idle);

	if (proc->spawn->active_procs > proc->spawn->min_procs) {
		proc->failed_connect = -1;
		proxy_spawn_proc_kill(proc);
	}
}

static void proxy_spawn_session_register(proxy_spawn_proc *proc, proxy_session *sess) {
	proxy_spawn_proc *p = proc;

	sess->is_closing = 1;
	sess->proxy_proc = proc;
	proc->load++;
	COUNTER_SET(proc->stat_load, proc->load);
	proxy_spawn_proc_update_idle(proc);

	list_for_each_entry_continue(p, &proc->spawn->proc_active, list) {
		if (p->load > proc->load)
			break;
	}
	list_move_tail(&proc->list, &p->list);
}

static void proxy_spawn_session_unregister(proxy_spawn_proc *proc) {
	proc->load--;
	COUNTER_SET(proc->stat_load, proc->load);

	if (proc->state == PROXY_CORE_SPAWN_PROC_ACTIVE) {
		proxy_spawn_proc *p = proc;

		list_for_each_entry_continue_reverse(p, &proc->spawn->proc_active, list) {
			if (p->load < proc->load)
				break;
		}
		list_move(&proc->list, &p->list);
	}
}

static void proxy_spawn_proc_failed(proxy_spawn_proc *proc) {
	if (!proc) return;
	proxy_spawn_session_unregister(proc);
	COUNTER_INC(proc->stat_requests_failed);

	switch (proc->state) {
	case PROXY_CORE_SPAWN_PROC_ACTIVE:
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_DISABLED, 5);
	case PROXY_CORE_SPAWN_PROC_DISABLED:
		if (++(proc->failed_connect) > MAX_CONNECT_FAILEDS) {
			proxy_spawn_proc_kill(proc);
		}
	case PROXY_CORE_SPAWN_PROC_KILLED:
		proxy_spawn_proc_check_killed(proc);
		break;
	case PROXY_CORE_SPAWN_PROC_INIT:
	case PROXY_CORE_SPAWN_PROC_REMOVED:
		break;
	}
}

static void proxy_spawn_proc_success(proxy_spawn_proc *proc) {
	if (!proc) return;
	proxy_spawn_session_unregister(proc);
	COUNTER_INC(proc->stat_requests);

	proc->failed_connect = 0;

	if (proc->state == PROXY_CORE_SPAWN_PROC_ACTIVE) {
		proxy_spawn_proc_update_idle(proc);
	}
}

static proxy_spawn_proc *proxy_spawn_need_new_proc(proxy_spawn *spawn) {
	proxy_spawn_proc *proc, *n;

	// Move removed to init after timeout
	time_t curtime = time(0);
	list_for_each_entry_safe(proc, n, &spawn->proc_removed, list) {
		COUNTER_SET(proc->stat_cur_time, curtime);
		if (proc->action_time <= curtime) {
			proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_INIT, 0);
		}
	}

	list_for_each_entry(proc, &spawn->proc_active, list) {
		if (proc->load < spawn->max_load_per_proc) {
			return proc;
		}
	}

	list_for_each_entry_safe(proc, n, &spawn->proc_init, list) {
		proxy_spawn_proc_start(proc);
		if (proc->state == PROXY_CORE_SPAWN_PROC_ACTIVE) {
			return proc;
		}
	}

	return NULL;
}

typedef struct {
	char **ptr;

	size_t size;
	size_t used;
} char_array;

static int env_add(char_array *env, const char *var) {
	if (env->size == 0) {
		env->size = 16;
		env->ptr = malloc(env->size * sizeof(*env->ptr));
	} else if (env->size == env->used + 1) {
		env->size += 16;
		env->ptr = realloc(env->ptr, env->size * sizeof(*env->ptr));
	}

	env->ptr[env->used++] = (char*) var;
	env->ptr[env->used] = 0;

	return 0;
}

static int env_add_key_val(char_array *env, const char *key, size_t key_len, const char *val, size_t val_len) {
	char *dst;

	if (!key || !val) return -1;

	dst = malloc(key_len + val_len + 3);
	memcpy(dst, key, key_len);
	dst[key_len] = '=';
	/* add the \0 from the value */
	memcpy(dst + key_len + 1, val, val_len + 1);

	env_add(env, dst);

	return 0;
}

static void env_init(char_array *env) {
	env->size = 16; env->used = 0;
	env->ptr = malloc(env->size * sizeof(*env->ptr));
	env->ptr[env->used] = 0;
}

static void proxy_spawn_proc_make_env(char_array *env, proxy_spawn_proc *proc) {
	size_t i;
	proxy_spawn *spawn = proc->spawn;

	env_init(env);

	/* build clean environment */
	if (spawn->bin_env_copy->used) {
		for (i = 0; i < spawn->bin_env_copy->used; i++) {
			data_string *ds = (data_string *)spawn->bin_env_copy->data[i];
			char *ge;

			if (NULL != (ge = getenv(ds->value->ptr))) {
				env_add_key_val(env, CONST_BUF_LEN(ds->value), ge, strlen(ge));
			}
		}
	} else {
		for (i = 0; environ[i]; i++) {
			env_add(env, environ[i]);
		}
	}

	/* create environment */
	for (i = 0; i < spawn->bin_env->used; i++) {
		data_string *ds = (data_string *)spawn->bin_env->data[i];

		env_add_key_val(env, CONST_BUF_LEN(ds->key), CONST_BUF_LEN(ds->value));
	}
}

static void proxy_spawn_proc_start(proxy_spawn_proc *proc) {
	int fcgi_fd, val;
	pid_t child;
	proxy_spawn *spawn = proc->spawn;

	if (proc->state == PROXY_CORE_SPAWN_PROC_ACTIVE) return;

	/* Unlink unix socket */
	if (proc->address->addr.un.sun_family == AF_UNIX) {
		unlink(proc->address->addr.un.sun_path);
	}

	if (-1 == (fcgi_fd = socket(proc->address->addr.plain.sa_family, SOCK_STREAM, 0))) {
		ERROR("socket failed: %s (%d)", strerror(errno), errno);
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_REMOVED, 60);
		return;
	}

	val = 1;
	if (setsockopt(fcgi_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		ERROR("socketsockopt failed: %s (%d)", strerror(errno), errno);
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_REMOVED, 60);
		return;
	}

	if (-1 == bind(fcgi_fd, &(proc->address->addr.plain), proc->address->addrlen)) {
		ERROR("bind(%s) failed: %s (%d)",
			BUF_STR(proc->address->name),
			strerror(errno), errno);
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_REMOVED, 60);
		return;
	}

	if (-1 == listen(fcgi_fd, 32)) {
		ERROR("listen(%s) failed: %s (%d)",
			BUF_STR(proc->address->name),
			strerror(errno), errno);
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_REMOVED, 60);
		return;
	}

	child = fork();

	if (child == -1) {
		ERROR("Could not fork: %s (%d)", strerror(errno), errno);
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_REMOVED, 60);
	} else if (child) {
		close(fcgi_fd);
		proc->pid = child;
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_ACTIVE, 0);
	} else { /* child == 0 */
		char_array env;
		/* close some fds, so there should be enough free
		 * for (int i = 3; i < 20; i++) close(i);
		 */

#define FCGI_LISTENSOCK_FILENO 0
		if (fcgi_fd != FCGI_LISTENSOCK_FILENO) {
			close(FCGI_LISTENSOCK_FILENO);
			dup2(fcgi_fd, FCGI_LISTENSOCK_FILENO);
			close(fcgi_fd);
			fcgi_fd = FCGI_LISTENSOCK_FILENO;
		}

		for (int i = 3; i < 1024; i++) close(i);

		if (spawn->binary->base_dir->used > 0) {
			if (-1 == chdir(BUF_STR(spawn->binary->base_dir))) {
				ERROR("chdir(%s) failed: %s (%d)",
					BUF_STR(spawn->binary->base_dir),
					strerror(errno), errno);
				exit(-1);
			}
		}

		proxy_spawn_proc_make_env(&env, proc);
		if (spawn->debug) {
			TRACE("exec %s", spawn->binary->ptr[0]);
		}
		/* close(1); close(2); */
		execve(spawn->binary->ptr[0], spawn->binary->ptr, env.ptr);

		ERROR("execvp(%s) failed: %s (%d)",
			BUF_STR(spawn->bin_path),
			strerror(errno), errno);
		exit(-2);
	}
}

proxy_address *proxy_spawn_address_balancer(server *srv, connection *con, struct proxy_session *sess, proxy_spawn* spawn) {
	proxy_spawn_proc *proc;

	UNUSED(srv);
	UNUSED(con);

	list_for_each_entry(proc, &spawn->proc_active, list) {
		if (proc->load < spawn->max_load_per_proc) {
			proxy_spawn_session_register(proc, sess);
			return proc->address;
		}
	}

	if (NULL != (proc = proxy_spawn_need_new_proc(spawn))) {
		if (proc->load < spawn->max_load_per_proc) {
			proxy_spawn_session_register(proc, sess);
			return proc->address;
		}
	}

	TRACE("%s", "No active proxy spawn proc found");
	return NULL;
}

void proxy_spawn_session_close(server *srv, proxy_session *session) {
	proxy_spawn_proc *proc = session->proxy_proc;
	UNUSED(srv);

	if (!proc) return;

	proxy_spawn_proc_success(proc);
	session->proxy_proc = 0;
}

void proxy_spawn_disable_address(proxy_session *session, time_t until) {
	proxy_spawn_proc *proc = session->proxy_proc;
	UNUSED(until);

	if (!proc) return;

	proxy_spawn_proc_failed(proc);
	session->proxy_proc = 0;
}

void proxy_spawn_trigger(proxy_spawn_config* cfg) {
	proxy_spawn_proc *proc, *p;
	time_t cur = time(0);

	list_for_each_entry_safe(proc, p, &cfg->proc_killed, list) {
		if (proc->action_time > cur) break;
		proxy_spawn_proc_check_killed(proc);
	}

	list_for_each_entry_safe(proc, p, &cfg->proc_disabled, list) {
		if (proc->action_time > cur) break;
		proxy_spawn_proc_change_state(proc, PROXY_CORE_SPAWN_PROC_ACTIVE, 0);
	}

	list_for_each_entry_safe(proc, p, &cfg->proc_idle, list_idle) {
		if (proc->action_time > cur) break;
		proxy_spawn_proc_check_idle(proc);
	}
}

// proxy_spawn + config

// forward for data init proc
static proxy_spawn* spawn_init(buffer* id);

static proxy_spawn *spawn_find_id(proxy_spawn_config* cfg, buffer *id) {
	return (proxy_spawn *) array_get_element(cfg->spawns, CONST_BUF_LEN(id));
}

static proxy_spawn *spawn_find_address(proxy_spawn_config* cfg, buffer *address) {
	return (proxy_spawn *) array_get_element(cfg->spawns,
		address->ptr + sizeof(PROXY_CORE_SPAWN_URL) - 1,
		address->used - sizeof(PROXY_CORE_SPAWN_URL));
}

proxy_spawn_config* proxy_spawn_config_init() {
	proxy_spawn_config* cfg = calloc(1, sizeof(*cfg));

	cfg->spawns = array_init();

	INIT_LIST_HEAD(&cfg->proc_disabled);
	INIT_LIST_HEAD(&cfg->proc_killed);
	INIT_LIST_HEAD(&cfg->proc_idle);

	return cfg;
}

void proxy_spawn_config_free(proxy_spawn_config* cfg) {
	if (!cfg) return;

	array_free(cfg->spawns);
	free(cfg);
}

static proxy_spawn* spawn_create_from_config(server *srv, mod_proxy_core_plugin_data *p, proxy_spawn_config *cfg, array *ca, buffer *id) {
	proxy_spawn *spawn;
	buffer *address;

	config_values_t cv[] = {
		/* "id" already in id */
		{ "bin-path",              NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ "min-procs",             NULL, T_CONFIG_SHORT , T_CONFIG_SCOPE_CONNECTION },       /* 1 */
		{ "max-procs",             NULL, T_CONFIG_SHORT , T_CONFIG_SCOPE_CONNECTION },       /* 2 */
		{ "idle-time",             NULL, T_CONFIG_SHORT , T_CONFIG_SCOPE_CONNECTION },       /* 3 */
		{ "max-load-per-proc",     NULL, T_CONFIG_SHORT , T_CONFIG_SCOPE_CONNECTION },       /* 4 */
		{ "address",               NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 5 */
		{ "bin-environment",       NULL, T_CONFIG_ARRAY , T_CONFIG_SCOPE_CONNECTION },        /* 6 */
		{ "bin-copy-environment",  NULL, T_CONFIG_ARRAY , T_CONFIG_SCOPE_CONNECTION },        /* 7 */
		{ "debug",                 NULL, T_CONFIG_SHORT , T_CONFIG_SCOPE_CONNECTION },        /* 8 */
		{ "logfile",               NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },        /* 9 */
		{ NULL,                    NULL, T_CONFIG_UNSET , T_CONFIG_SCOPE_UNSET }
	};

	UNUSED(p);

	spawn = spawn_init(id);
	spawn->cfg = cfg;

	address = buffer_init();

	cv[0].destination = spawn->bin_path;
	cv[1].destination = &(spawn->min_procs);
	cv[2].destination = &(spawn->max_procs);
	cv[3].destination = &(spawn->idle_time);
	cv[4].destination = &(spawn->max_load_per_proc);
	cv[5].destination = address;
	cv[6].destination = spawn->bin_env;
	cv[7].destination = spawn->bin_env_copy;
	cv[8].destination = &spawn->debug;
	cv[9].destination = spawn->logfile;

	if (0 != config_insert_values_internal(srv, ca, cv)) {
		return 0;
	}

	if (0 == spawn->bin_path->used) {
		ERROR("%s", "Spawn option 'bin-path' empty");
		return 0;
	}

	if (0 == address->used) {
		ERROR("%s", "Spawn option 'address' empty");
		return 0;
	} else {
		proxy_address_pool* apool;

		apool = proxy_address_pool_init();
		if (0 != proxy_address_pool_add_string(apool, address)) {
			return 0;
		}
		// Take first address from pool
		spawn->base_address = apool->ptr[0];
		if (apool->used-- > 1) {
			apool->ptr[0] = apool->ptr[apool->used];
		}
		proxy_address_pool_free(apool);
	}

	spawn->binary = proxy_spawn_binary_init(spawn->bin_path);

	if (spawn->min_procs > spawn->max_procs) {
		spawn->min_procs = spawn->max_procs;
	}

	{
		proxy_spawn_procs *procs = spawn->procs = calloc(1, sizeof(*spawn->procs));
		size_t maxs = spawn->max_procs;
		procs->size = maxs;
		procs->used = maxs;
		procs->ptr = malloc(maxs * sizeof(*(procs->ptr)));
		for (size_t i = 0; i < maxs; i++) {
			procs->ptr[i] = proxy_spawn_proc_init(spawn, spawn->base_address, i);
		}
	}

	buffer_free(address);

	return spawn;
}


handler_t proxy_spawn_config_parse(server *srv, mod_proxy_core_plugin_data *p) {
	size_t i, j;
	proxy_spawn_config *cfg = p->spawn_config;

	config_values_t cv[] = {
		{ CONFIG_PROXY_CORE_SPAWN,     NULL, T_CONFIG_LOCAL, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ NULL,                        NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};
	cv[0].destination = NULL; // T_CONFIG_LOCAL is not set

	if (!cfg) return HANDLER_ERROR;

	for (i = 0; i < srv->config_context->used; i++) {
		array *ca, *spawns;
		data_array *da;

		ca = ((data_config *)srv->config_context->data[i])->value;

		if (0 != config_insert_values_global(srv, ca, cv)) {
			return HANDLER_ERROR;
		}

		da = (data_array*) array_get_element(ca, CONST_STR_LEN(CONFIG_PROXY_CORE_SPAWN));
		if (!da) continue;

		if (da->type != TYPE_ARRAY) {
			ERROR("'%s' must be an array of spawns", CONFIG_PROXY_CORE_SPAWN);
			return HANDLER_ERROR;
		}

		spawns = da->value;

		for (j = 0; j < spawns->used; j++) {
			proxy_spawn *spawn;
			array *spawna;
			data_string *dspawnid;
			data_array *dspawn = (data_array*) spawns->data[j];
			if (dspawn->type != TYPE_ARRAY) {
				ERROR("'%s' entry must be an array of spawn options", CONFIG_PROXY_CORE_SPAWN);
				return HANDLER_ERROR;
			}
			spawna = dspawn->value;

			dspawnid = (data_string*) array_get_element(spawna, CONST_STR_LEN("id"));
			if (!dspawnid) {
				ERROR("%s", "Missing id of spawn");
				return HANDLER_ERROR;
			}
			if (dspawnid->type != TYPE_STRING) {
				ERROR("%s", "Unexpected type of spawn id; expected string.");
				return HANDLER_ERROR;
			}

			spawn = spawn_find_id(cfg, dspawnid->value);
			if (spawn) {
				if (spawn->spawna != spawna) {
					ERROR("spawn id not unique: %s", BUF_STR(dspawnid->value));
					return HANDLER_ERROR;
				}
				continue;
			}

			spawn = spawn_create_from_config(srv, p, cfg, spawna, dspawnid->value);

			if (spawn) {
				array_insert_unique(cfg->spawns, (data_unset*) spawn);
			} else {
				return HANDLER_ERROR;
			}
		}
	}
	return HANDLER_GO_ON;
}

proxy_backend* proxy_spawn_config_backend(proxy_spawn_config* cfg, buffer *address) {
	proxy_backend *backend;
	proxy_spawn *spawn = spawn_find_address(cfg, address);
	if (!spawn) {
		ERROR("Couldn't find proxy-core.spawn with id '%s'", BUF_STR(address));
		return NULL;
	}

	backend = proxy_backend_init();

	backend->spawn = spawn;
	backend->pool->max_size = spawn->max_procs * spawn->max_load_per_proc;

	return backend;
}

// proxy spawn data handling (for array)

static data_unset* data_spawn_copy(const data_unset *s) {
	UNUSED(s);
	ERROR("%s", "Cannot copy proxy_spawn");
	assert(0);
}

static void data_spawn_free(data_unset *d) {
	proxy_spawn *ds = (proxy_spawn*) d;
	if (!ds) return;

	buffer_free(ds->key);

	buffer_free(ds->bin_path);
	ds->bin_path = 0;
	proxy_spawn_binary_free(ds->binary);
	ds->binary = 0;

	buffer_free(ds->logfile);
	ds->logfile = 0;

	proxy_address_free(ds->base_address);
	ds->base_address = 0;

	array_free(ds->bin_env);
	array_free(ds->bin_env_copy);

	ARRAY_STATIC_FREE(ds->procs, proxy_spawn_proc, element, proxy_spawn_proc_free(element));
	free(ds->procs);
	ds->procs = 0;

	free(d);
}

static void data_spawn_reset(data_unset *d) {
	proxy_spawn *ds = (proxy_spawn*) d;
	if (!ds) return;

	buffer_reset(ds->key);
}

static proxy_spawn* spawn_init(buffer* id) {
	proxy_spawn *ds;

	ds = calloc(1, sizeof(*ds));
	assert(ds);

	ds->key = buffer_init();
	buffer_copy_string_buffer(ds->key, id);

	ds->bin_path = buffer_init();
	ds->min_procs = 4;
	ds->max_procs = 4;
	ds->idle_time = 30;
	ds->max_load_per_proc = 4;
	ds->bin_env = array_init();
	ds->bin_env_copy = array_init();
	ds->debug = 0;
	ds->logfile = buffer_init();

	INIT_LIST_HEAD(&ds->proc_init);
	INIT_LIST_HEAD(&ds->proc_active);
	INIT_LIST_HEAD(&ds->proc_removed);

	ds->copy = data_spawn_copy;
	ds->free = data_spawn_free;
	ds->reset = data_spawn_reset;
	ds->type = TYPE_PROXY_SPAWN;

	return ds;
}
