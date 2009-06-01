/* Copyright (C) 2005-2009 Timo Sirainen */

#include "lib.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "array.h"
#include "env-util.h"
#include "home-expand.h"
#include "restrict-access.h"
#include "fd-close-on-exec.h"
#include "settings-parser.h"
#include "syslog-util.h"
#include "master-service-private.h"
#include "master-service-settings.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <syslog.h>

#define DEFAULT_CONFIG_FILE_PATH SYSCONFDIR"/dovecot.conf"

/* getenv(MASTER_CONFIG_FILE_ENV) provides path to configuration file/socket */
#define MASTER_CONFIG_FILE_ENV "CONFIG_FILE"

/* getenv(MASTER_DOVECOT_VERSION_ENV) provides master's version number */
#define MASTER_DOVECOT_VERSION_ENV "DOVECOT_VERSION"

struct master_service *master_service;

static void io_listeners_add(struct master_service *service);
static void io_listeners_remove(struct master_service *service);
static void master_status_update(struct master_service *service);

const char *master_service_getopt_string(void)
{
	return "c:ko:Os:L";
}

static void sig_die(const siginfo_t *si, void *context)
{
	struct master_service *service = context;

	/* warn about being killed because of some signal, except SIGINT (^C)
	   which is too common at least while testing :) */
	if (si->si_signo != SIGINT) {
		i_warning("Killed with signal %d (by pid=%s uid=%s code=%s)",
			  si->si_signo, dec2str(si->si_pid),
			  dec2str(si->si_uid),
			  lib_signal_code_to_str(si->si_signo, si->si_code));
	}
	io_loop_stop(service->ioloop);
}

static void master_service_verify_version(struct master_service *service)
{
	if (service->version_string != NULL &&
	    strcmp(service->version_string, PACKAGE_VERSION) != 0) {
		i_fatal("Dovecot version mismatch: "
			"Master is v%s, %s is v"PACKAGE_VERSION" "
			"(if you don't care, set version_ignore=yes)",
			service->name, service->version_string);
	}
}

struct master_service *
master_service_init(const char *name, enum master_service_flags flags,
		    int argc, char *argv[])
{
	struct master_service *service;
	const char *str;

	i_assert(name != NULL);

#ifdef DEBUG
	if (getenv("GDB") == NULL &&
	    (flags & MASTER_SERVICE_FLAG_STANDALONE) == 0) {
		int count;

		str = getenv("SOCKET_COUNT");
		count = str == NULL ? 0 : atoi(str);
		fd_debug_verify_leaks(MASTER_LISTEN_FD_FIRST + count, 1024);
	}
#endif

	/* NOTE: we start rooted, so keep the code minimal until
	   restrict_access_by_env() is called */
	lib_init();
	/* Set a logging prefix temporarily. This will be ignored once the log
	   is properly initialized */
	i_set_failure_prefix(t_strdup_printf("%s(init): ", name));

	if (getenv(MASTER_UID_ENV) == NULL)
		flags |= MASTER_SERVICE_FLAG_STANDALONE;

	service = i_new(struct master_service, 1);
	service->argc = argc;
	service->argv = argv;
	service->name = i_strdup(name);
	service->flags = flags;
	service->ioloop = io_loop_create();
	service->service_count_left = (unsigned int)-1;
	service->config_fd = -1;

	service->config_path = getenv(MASTER_CONFIG_FILE_ENV);
	if (service->config_path == NULL)
		service->config_path = DEFAULT_CONFIG_FILE_PATH;

	if ((flags & MASTER_SERVICE_FLAG_STANDALONE) == 0) {
		service->version_string = getenv(MASTER_DOVECOT_VERSION_ENV);
		service->socket_count = 1;
	} else {
		service->version_string = PACKAGE_VERSION;
	}
	str = getenv("SOCKET_COUNT");
	if (str != NULL)
		service->socket_count = atoi(str);
	str = getenv("SSL_SOCKET_COUNT");
	if (str != NULL)
		service->ssl_socket_count = atoi(str);

	/* set up some kind of logging until we know exactly how and where
	   we want to log */
	if (getenv("LOG_SERVICE") != NULL)
		i_set_failure_internal();
	if (getenv("USER") != NULL) {
		i_set_failure_prefix(t_strdup_printf("%s(%s): ",
						     name, getenv("USER")));
	} else {
		i_set_failure_prefix(t_strdup_printf("%s: ", name));
	}

	master_service_verify_version(service);
	return service;
}

void master_service_init_log(struct master_service *service, const char *prefix,
			     unsigned int max_lines_per_sec)
{
	const char *path;

	if ((service->flags & MASTER_SERVICE_FLAG_STANDALONE) != 0 &&
	    (service->flags & MASTER_SERVICE_FLAG_DONT_LOG_TO_STDERR) == 0) {
		i_set_failure_file("/dev/stderr", "");
		return;
	}

	if (getenv("LOG_SERVICE") != NULL && !service->log_directly) {
		/* logging via log service */
		i_set_failure_internal();
		i_set_failure_prefix(prefix);
		return;
	}

	if (service->set == NULL) {
		i_set_failure_file("/dev/stderr", prefix);
		return;
	}

	if (*service->set->log_path == '\0') {
		/* log to syslog */
		int facility;

		if (!syslog_facility_find(service->set->syslog_facility,
					  &facility))
			facility = LOG_MAIL;
		i_set_failure_syslog("dovecot", LOG_NDELAY, facility);
		i_set_failure_prefix(prefix);
	} else {
		/* log to file or stderr */
		path = home_expand(service->set->log_path);
		i_set_failure_file(path, prefix);
	}

	path = home_expand(service->set->info_log_path);
	if (*path != '\0')
		i_set_info_file(path);
	i_set_failure_timestamp_format(service->set->log_timestamp);
}

bool master_service_parse_option(struct master_service *service,
				 int opt, const char *arg)
{
	int i;

	switch (opt) {
	case 'c':
		service->config_path = arg;
		break;
	case 'k':
		service->keep_environment = TRUE;
		break;
	case 'o':
		if (!array_is_created(&service->config_overrides))
			i_array_init(&service->config_overrides, 16);
		array_append(&service->config_overrides, &arg, 1);
		break;
	case 'O':
		service->default_settings = TRUE;
		break;
	case 's':
		if ((i = atoi(arg)) < 0)
			i_fatal("Invalid socket count: %s", arg);
                service->socket_count = i;
		break;
	case 'L':
		service->log_directly = TRUE;
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

static void master_status_error(void *context)
{
	struct master_service *service = context;

	/* status fd is a write-only pipe, so if we're here it means the
	   master wants us to die (or died itself). don't die until all
	   service connections are finished. */
	io_remove(&service->io_status_error);

	/* the log fd may also be closed already, don't die when trying to
	   log later */
	i_set_failure_ignore_errors(TRUE);

	if (service->master_status.available_count ==
	    service->total_available_count)
		master_service_stop(service);
}

void master_service_init_finish(struct master_service *service)
{
	struct stat st;
	const char *value;
	unsigned int count;

	i_assert(service->total_available_count == 0);
	i_assert(service->service_count_left == (unsigned int)-1);

	/* set default signal handlers */
	lib_signals_init();
        lib_signals_ignore(SIGPIPE, TRUE);
        lib_signals_ignore(SIGALRM, FALSE);
        lib_signals_set_handler(SIGINT, TRUE, sig_die, service);
	lib_signals_set_handler(SIGTERM, TRUE, sig_die, service);

	if ((service->flags & MASTER_SERVICE_FLAG_STANDALONE) == 0) {
		if (fstat(MASTER_STATUS_FD, &st) < 0 || !S_ISFIFO(st.st_mode))
			i_fatal("Must be started by dovecot master process");

		/* initialize master_status structure */
		value = getenv(MASTER_UID_ENV);
		if (value == NULL)
			i_fatal(MASTER_UID_ENV" not set");
		service->master_status.pid = getpid();
		service->master_status.uid =
			(unsigned int)strtoul(value, NULL, 10);

		/* set the default limit */
		value = getenv(MASTER_CLIENT_LIMIT_ENV);
		count = value == NULL ? 0 :
			(unsigned int)strtoul(value, NULL, 10);
		if (count == 0)
			i_fatal(MASTER_CLIENT_LIMIT_ENV" not set");
		master_service_set_client_limit(service, count);

		/* start listening errors for status fd, it means master died */
		service->io_status_error = io_add(MASTER_STATUS_FD, IO_ERROR,
						  master_status_error, service);
	} else {
		master_service_set_client_limit(service, 1);
		master_service_set_service_count(service, 1);
	}

	io_listeners_add(service);

	if ((service->flags & MASTER_SERVICE_FLAG_STD_CLIENT) != 0) {
		/* we already have a connection to be served */
		service->master_status.available_count--;
	}
	master_status_update(service);
}

void master_service_env_clean(bool preserve_home)
{
	const char *user, *tz, *home;
#ifdef DEBUG
	bool gdb = getenv("GDB") != NULL;
#endif

	user = getenv("USER");
	if (user != NULL)
		user = t_strconcat("USER=", user, NULL);
	tz = getenv("TZ");
	if (tz != NULL)
		tz = t_strconcat("TZ=", tz, NULL);
	home = preserve_home ? getenv("HOME") : NULL;
	if (home != NULL)
		home = t_strconcat("HOME=", home, NULL);

	/* Note that if the original environment was set with env_put(), the
	   environment strings will be invalid after env_clean(). That's why
	   we t_strconcat() them above. */
	env_clean();

	if (user != NULL) env_put(user);
	if (tz != NULL) env_put(tz);
	if (home != NULL) env_put(home);
#ifdef DEBUG
	if (gdb) env_put("GDB=1");
#endif
}

void master_service_set_client_limit(struct master_service *service,
				     unsigned int client_limit)
{
	i_assert(service->master_status.available_count ==
		 service->total_available_count);

	service->total_available_count = client_limit;
	service->master_status.available_count = client_limit;
}

unsigned int master_service_get_client_limit(struct master_service *service)
{
	return service->total_available_count;
}

void master_service_set_service_count(struct master_service *service,
				      unsigned int count)
{
	unsigned int used;

	used = service->total_available_count -
		service->master_status.available_count;
	i_assert(count >= used);

	if (service->total_available_count > count) {
		service->total_available_count = count;
		service->master_status.available_count = count - used;
	}
	service->service_count_left = count;
}

unsigned int master_service_get_service_count(struct master_service *service)
{
	return service->service_count_left;
}

unsigned int master_service_get_socket_count(struct master_service *service)
{
	return service->socket_count;
}

const char *master_service_get_config_path(struct master_service *service)
{
	return service->config_path;
}

const char *master_service_get_version_string(struct master_service *service)
{
	return service->version_string;
}

const char *master_service_get_name(struct master_service *service)
{
	return service->name;
}

void master_service_run(struct master_service *service,
			master_service_connection_callback_t *callback)
{
	service->callback = callback;
	io_loop_run(service->ioloop);
	service->callback = NULL;
}

void master_service_stop(struct master_service *service)
{
        io_loop_stop(service->ioloop);
}

void master_service_anvil_send(struct master_service *service, const char *cmd)
{
	ssize_t ret;

	if ((service->flags & MASTER_SERVICE_FLAG_STANDALONE) != 0)
		return;

	ret = write(MASTER_ANVIL_FD, cmd, strlen(cmd));
	if (ret < 0)
		i_error("write(anvil) failed: %m");
	else if (ret == 0)
		i_error("write(anvil) failed: EOF");
	else {
		i_assert((size_t)ret == strlen(cmd));
	}
}

void master_service_client_connection_destroyed(struct master_service *service)
{
	if (service->listeners == NULL) {
		/* we can listen again */
		io_listeners_add(service);
	}

	i_assert(service->total_available_count > 0);

	if (service->service_count_left != service->total_available_count) {
		i_assert(service->service_count_left == (unsigned int)-1);
		service->master_status.available_count++;
	} else {
		/* we have only limited amount of service requests left */
		i_assert(service->service_count_left > 0);
                service->service_count_left--;
		service->total_available_count--;

		if (service->service_count_left == 0) {
			i_assert(service->master_status.available_count ==
				 service->total_available_count);
			master_service_stop(service);
		}
	}
	master_status_update(service);

	if (service->io_status_error == NULL &&
	    service->master_status.available_count ==
	    service->total_available_count) {
		/* master has closed the connection and we have nothing else
		   to do anymore. */
		master_service_stop(service);
	}
}

void master_service_deinit(struct master_service **_service)
{
	struct master_service *service = *_service;

	*_service = NULL;

	io_listeners_remove(service);

	if (service->io_status_error != NULL)
		io_remove(&service->io_status_error);
	if (service->io_status_write != NULL)
		io_remove(&service->io_status_write);
	if (array_is_created(&service->config_overrides))
		array_free(&service->config_overrides);

	if (service->set_parser != NULL) {
		settings_parser_deinit(&service->set_parser);
		pool_unref(&service->set_pool);
	}
	lib_signals_deinit();
	io_loop_destroy(&service->ioloop);

	i_free(service->name);
	i_free(service);

	lib_deinit();
}

static void master_service_listen(struct master_service_listener *l)
{
	struct master_service_connection conn;

	if (l->service->master_status.available_count == 0) {
		/* we are full. stop listening for now. */
		io_listeners_remove(l->service);
		return;
	}

	memset(&conn, 0, sizeof(conn));
	conn.listen_fd = l->fd;
	conn.fd = net_accept(l->fd, &conn.remote_ip, &conn.remote_port);
	if (conn.fd < 0) {
		if (conn.fd == -1)
			return;

		if (errno != ENOTSOCK) {
			i_error("net_accept() failed: %m");
			io_listeners_remove(l->service);
			return;
		}
		/* it's not a socket. probably a fifo. use the "listener"
		   as the connection fd */
		io_remove(&l->io);
		conn.fd = l->fd;
	}
	conn.ssl = l->ssl;
	net_set_nonblock(conn.fd, TRUE);

	l->service->master_status.available_count--;
        master_status_update(l->service);

        l->service->callback(&conn);
}

static void io_listeners_add(struct master_service *service)
{
	unsigned int i;

	if (service->socket_count == 0)
		return;

	service->listeners =
		i_new(struct master_service_listener, service->socket_count);

	for (i = 0; i < service->socket_count; i++) {
		struct master_service_listener *l = &service->listeners[i];

		l->service = service;
		l->fd = MASTER_LISTEN_FD_FIRST + i;
		l->io = io_add(MASTER_LISTEN_FD_FIRST + i, IO_READ,
			       master_service_listen, l);

		if (i >= service->socket_count - service->ssl_socket_count)
			l->ssl = TRUE;
	}
}

static void io_listeners_remove(struct master_service *service)
{
	unsigned int i;

	if (service->listeners != NULL) {
		for (i = 0; i < service->socket_count; i++) {
			if (service->listeners[i].io != NULL)
				io_remove(&service->listeners[i].io);
		}
		i_free_and_null(service->listeners);
	}
}

static bool master_status_update_is_important(struct master_service *service)
{
	if (service->master_status.available_count == 0)
		return TRUE;
	if (!service->initial_status_sent)
		return TRUE;
	return FALSE;
}

static void master_status_update(struct master_service *service)
{
	ssize_t ret;

	if (service->master_status.pid == 0)
		return; /* closed */

	ret = write(MASTER_STATUS_FD, &service->master_status,
		    sizeof(service->master_status));
	if (ret > 0) {
		/* success */
		if (service->io_status_write != NULL) {
			/* delayed important update sent successfully */
			io_remove(&service->io_status_write);
		}
		service->initial_status_sent = TRUE;
	} else if (ret == 0) {
		/* shouldn't happen? */
		i_error("write(master_status_fd) returned 0");
		service->master_status.pid = 0;
	} else if (errno != EAGAIN) {
		/* failure */
		if (errno != EPIPE)
			i_error("write(master_status_fd) failed: %m");
		service->master_status.pid = 0;
	} else if (master_status_update_is_important(service)) {
		/* reader is busy, but it's important to get this notification
		   through. send it when possible. */
		if (service->io_status_write == NULL) {
			service->io_status_write =
				io_add(MASTER_STATUS_FD, IO_WRITE,
				       master_status_update, service);
		}
	}
}
