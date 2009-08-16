#ifndef _MOD_PROXY_CORE_SPAWN_H_
#define _MOD_PROXY_CORE_SPAWN_H_

#ifndef USE_COUNTER
#define USE_COUNTER 0
#endif

#include "array-static.h"
#include "array.h"
#include "list.h"
#include "buffer.h"
#include "server.h"
#include "mod_proxy_core.h"
#include "mod_proxy_core_address.h"
#include "mod_proxy_core_backend.h"
#include "sys-socket.h"

typedef enum proxy_core_spawn_proc_state {
	// reset failed_connect
	// on spawn move to idle
	PROXY_CORE_SPAWN_PROC_INIT,

	// proc_count > min_proc && load == 0 => set kill timeout
	// on addr disable to disabled
	PROXY_CORE_SPAWN_PROC_ACTIVE,

	// incr failed_connect, if failed_connect >= 3 kill
	// if proc is down, remove
	// wait disable time
	// if proc is down, remove
	// move to active
	PROXY_CORE_SPAWN_PROC_DISABLED,

	// wait for proc going down
	// iv failed_connect >= 3 => remove
	// move to init
	PROXY_CORE_SPAWN_PROC_KILLED,

	// wait for timeout
	// move to init
	PROXY_CORE_SPAWN_PROC_REMOVED,
} proxy_core_spawn_proc_state;


ARRAY_STATIC_DEF(proxy_spawn_binary, char, buffer *arg_string; buffer *base_dir;);

struct proxy_spawn_proc;
typedef struct proxy_spawn_proc proxy_spawn_proc;

ARRAY_STATIC_DEF(proxy_spawn_procs, proxy_spawn_proc, );

struct proxy_spawn;
typedef struct proxy_spawn proxy_spawn;

struct proxy_spawn_config;
typedef struct proxy_spawn_config proxy_spawn_config;

struct proxy_spawn_proc {
	list_head list, list_idle;

	pid_t pid;
	proxy_address *address;

	time_t spawn_time;      // time proc was spawned
	time_t action_time;     // meaning depends on state

	unsigned short load;
	short failed_connect;   // -1: kill-reason idle-timeout

	proxy_core_spawn_proc_state state;
	proxy_spawn *spawn;

#if USE_COUNTER
	data_integer *stat_load, *stat_requests, *stat_requests_failed, *stat_state, *stat_act_time, *stat_cur_time;
#endif
};

struct proxy_spawn {
	DATA_UNSET;

	buffer *bin_path;
	proxy_spawn_binary *binary;

	buffer *logfile;

	array *bin_env;
	array *bin_env_copy;

	unsigned short min_procs, max_procs, active_procs;
	unsigned short idle_time;
	unsigned short max_load_per_proc;
	unsigned short debug;

	proxy_address *base_address;

	proxy_spawn_procs *procs;

	// linked lists
	list_head proc_init, proc_active, proc_removed;

	proxy_spawn_config *cfg;
	// config only
	array *spawna;     // check for unique spawn id
};

struct proxy_spawn_config {
	array *spawns;

	// linked lists
	list_head proc_disabled, proc_killed, proc_idle;
};

// Config
proxy_spawn_config* proxy_spawn_config_init();
void proxy_spawn_config_free(proxy_spawn_config* cfg);
handler_t proxy_spawn_config_parse(server *srv, mod_proxy_core_plugin_data *p);
proxy_backend* proxy_spawn_config_backend(proxy_spawn_config* cfg, buffer *address);

proxy_address *proxy_spawn_address_balancer(server *srv, connection *con, struct proxy_session *sess, proxy_spawn* spawn);

void proxy_spawn_session_close(server *srv, proxy_session *session);
void proxy_spawn_disable_address(proxy_session *session, time_t until);

void proxy_spawn_trigger(proxy_spawn_config* cfg);

#define PROXY_CORE_SPAWN_URL "spawn:"

#endif
