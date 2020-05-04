/*
 *
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2020	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 */


#include <string.h>
#include <arpa/inet.h>
#include "protocol/pool_pg_utils.h"
#include "protocol/pool_connection_pool.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/pool_stream.h"
#include "utils/pool_ssl.h"
#include "utils/elog.h"
#include "utils/pool_relcache.h"
#include "auth/pool_auth.h"
#include "context/pool_session_context.h"

#include "pool_config.h"
#include "pool_config_variables.h"

static int	choose_db_node_id(char *str);
static void free_persisten_db_connection_memory(POOL_CONNECTION_POOL_SLOT * cp);

/*
 * create a persistent connection
 */
POOL_CONNECTION_POOL_SLOT *
make_persistent_db_connection(
							  int db_node_id, char *hostname, int port, char *dbname, char *user, char *password, bool retry)
{
	POOL_CONNECTION_POOL_SLOT *cp;
	int			fd;

#define MAX_USER_AND_DATABASE	1024

	/* V3 startup packet */
	typedef struct
	{
		int			protoVersion;
		char		data[MAX_USER_AND_DATABASE];
	}			StartupPacket_v3;

	static StartupPacket_v3 * startup_packet;
	int			len,
				len1;

	cp = palloc0(sizeof(POOL_CONNECTION_POOL_SLOT));
	startup_packet = palloc0(sizeof(*startup_packet));
	startup_packet->protoVersion = htonl(0x00030000);	/* set V3 proto
														 * major/minor */

	/*
	 * create socket
	 */
	if (*hostname == '/')
	{
		fd = connect_unix_domain_socket_by_port(port, hostname, retry);
	}
	else
	{
		fd = connect_inet_domain_socket_by_port(hostname, port, retry);
	}

	if (fd < 0)
	{
		free_persisten_db_connection_memory(cp);
		pfree(startup_packet);
		ereport(ERROR,
				(errmsg("failed to make persistent db connection"),
				 errdetail("connection to host:\"%s:%d\" failed", hostname, port)));
	}

	cp->con = pool_open(fd, true);
	cp->closetime = 0;
	cp->con->isbackend = 1;
	pool_set_db_node_id(cp->con, db_node_id);

	pool_ssl_negotiate_clientserver(cp->con);

	/*
	 * build V3 startup packet
	 */
	len = snprintf(startup_packet->data, sizeof(startup_packet->data), "user") + 1;
	len1 = snprintf(&startup_packet->data[len], sizeof(startup_packet->data) - len, "%s", user) + 1;
	if (len1 >= (sizeof(startup_packet->data) - len))
	{
		pool_close(cp->con);
		free_persisten_db_connection_memory(cp);
		pfree(startup_packet);
		ereport(ERROR,
				(errmsg("failed to make persistent db connection"),
				 errdetail("user name is too long")));
	}

	len += len1;
	len1 = snprintf(&startup_packet->data[len], sizeof(startup_packet->data) - len, "database") + 1;
	if (len1 >= (sizeof(startup_packet->data) - len))
	{
		pool_close(cp->con);
		free_persisten_db_connection_memory(cp);
		pfree(startup_packet);
		ereport(ERROR,
				(errmsg("failed to make persistent db connection"),
				 errdetail("user name is too long")));
	}

	len += len1;
	len1 = snprintf(&startup_packet->data[len], sizeof(startup_packet->data) - len, "%s", dbname) + 1;
	if (len1 >= (sizeof(startup_packet->data) - len))
	{
		pool_close(cp->con);
		free_persisten_db_connection_memory(cp);
		pfree(startup_packet);
		ereport(ERROR,
				(errmsg("failed to make persistent db connection"),
				 errdetail("database name is too long")));
	}
	len += len1;
	startup_packet->data[len++] = '\0';

	cp->sp = palloc(sizeof(StartupPacket));

	cp->sp->startup_packet = (char *) startup_packet;
	cp->sp->len = len + 4;
	cp->sp->major = 3;
	cp->sp->minor = 0;
	cp->sp->database = pstrdup(dbname);
	cp->sp->user = pstrdup(user);

	/*
	 * send startup packet
	 */
	PG_TRY();
	{
		send_startup_packet(cp);
		connection_do_auth(cp, password);
	}
	PG_CATCH();
	{
		pool_close(cp->con);
		free_persisten_db_connection_memory(cp);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return cp;
}

/*
 * make_persistent_db_connection_noerror() is a wrapper over
 * make_persistent_db_connection() which does not ereports in case of an error
 */
POOL_CONNECTION_POOL_SLOT *
make_persistent_db_connection_noerror(
									  int db_node_id, char *hostname, int port, char *dbname, char *user, char *password, bool retry)
{
	POOL_CONNECTION_POOL_SLOT *slot = NULL;
	MemoryContext oldContext = CurrentMemoryContext;

	PG_TRY();
	{
		slot = make_persistent_db_connection(db_node_id,
											 hostname,
											 port,
											 dbname,
											 user,
											 password, retry);
	}
	PG_CATCH();
	{
		EmitErrorReport();
		MemoryContextSwitchTo(oldContext);
		FlushErrorState();
		slot = NULL;
	}
	PG_END_TRY();
	return slot;
}

/*
 * Free memory of POOL_CONNECTION_POOL_SLOT.  Should only be used in
 * make_persistent_db_connection and discard_persistent_db_connection.
 */
static void
free_persisten_db_connection_memory(POOL_CONNECTION_POOL_SLOT * cp)
{
	if (!cp)
		return;
	if (!cp->sp)
	{
		pfree(cp);
		return;
	}
	if (cp->sp->startup_packet)
		pfree(cp->sp->startup_packet);
	if (cp->sp->database)
		pfree(cp->sp->database);
	if (cp->sp->user)
		pfree(cp->sp->user);
	pfree(cp->sp);
	pfree(cp);
}

/*
 * Discard connection and memory allocated by
 * make_persistent_db_connection().
 */
void
discard_persistent_db_connection(POOL_CONNECTION_POOL_SLOT * cp)
{
	int			len;

	if (cp == NULL)
		return;

	pool_write(cp->con, "X", 1);
	len = htonl(4);
	pool_write(cp->con, &len, sizeof(len));

	/*
	 * XXX we cannot call pool_flush() here since backend may already close
	 * the socket and pool_flush() automatically invokes fail over handler.
	 * This could happen in copy command (remember the famous "lost
	 * synchronization with server, resetting connection" message)
	 */
	socket_set_nonblock(cp->con->fd);
	pool_flush_it(cp->con);
	socket_unset_nonblock(cp->con->fd);

	pool_close(cp->con);
	free_persisten_db_connection_memory(cp);
}

/*
 * send startup packet
 */
void
send_startup_packet(POOL_CONNECTION_POOL_SLOT * cp)
{
	int			len;

	len = htonl(cp->sp->len + sizeof(len));
	pool_write(cp->con, &len, sizeof(len));
	pool_write_and_flush(cp->con, cp->sp->startup_packet, cp->sp->len);
}

void
pool_free_startup_packet(StartupPacket *sp)
{
	if (sp)
	{
		if (sp->startup_packet)
			pfree(sp->startup_packet);
		if (sp->database)
			pfree(sp->database);
		if (sp->user)
			pfree(sp->user);
		pfree(sp);
	}
	sp = NULL;
}

/*
 * Select load balancing node. This function is called when:
 * 1) client connects
 * 2) the node previously selected for the load balance node is down
 */
int
select_load_balancing_node(void)
{
	int			selected_slot;
	double		total_weight,
				r;
	int			i;
	int			index_db = -1,
				index_app = -1;
	POOL_SESSION_CONTEXT *ses = pool_get_session_context(false);
	int			tmp;
	int			no_load_balance_node_id = -2;

	/*
	 * -2 indicates there's no database_redirect_preference_list. -1 indicates
	 * database_redirect_preference_list exists and any of standby nodes
	 * specified.
	 */
	int			suggested_node_id = -2;

#if defined(sun) || defined(__sun)
	r = (((double) rand()) / RAND_MAX);
#else
	r = (((double) random()) / RAND_MAX);
#endif

	/*
	 * Check database_redirect_preference_list
	 */
	if (SL_MODE && pool_config->redirect_dbnames)
	{
		char	   *database = MASTER_CONNECTION(ses->backend)->sp->database;

		/*
		 * Check to see if the database matches any of
		 * database_redirect_preference_list
		 */
		index_db = regex_array_match(pool_config->redirect_dbnames, database);
		if (index_db >= 0)
		{
			/* Matches */
			ereport(DEBUG1,
					(errmsg("selecting load balance node db matched"),
					 errdetail("dbname: %s index is %d dbnode is %s weight is %f", database, index_db,
							   pool_config->db_redirect_tokens->token[index_db].right_token,
							   pool_config->db_redirect_tokens->token[index_db].weight_token)));

			tmp = choose_db_node_id(pool_config->db_redirect_tokens->token[index_db].right_token);
			if (tmp == -1 || (tmp >= 0 && VALID_BACKEND(tmp)))
				suggested_node_id = tmp;
		}
	}

	/*
	 * Check app_name_redirect_preference_list
	 */
	if (SL_MODE && pool_config->redirect_app_names)
	{
		char	   *app_name = MASTER_CONNECTION(ses->backend)->sp->application_name;

		/*
		 * Check only if application name is set. Old applications may not
		 * have application name.
		 */
		if (app_name && strlen(app_name) > 0)
		{
			/*
			 * Check to see if the aplication name matches any of
			 * app_name_redirect_preference_list.
			 */
			index_app = regex_array_match(pool_config->redirect_app_names, app_name);
			if (index_app >= 0)
			{

				/*
				 * if the aplication name matches any of
				 * app_name_redirect_preference_list,
				 * database_redirect_preference_list will be ignored.
				 */
				index_db = -1;

				/* Matches */
				ereport(DEBUG1,
						(errmsg("selecting load balance node db matched"),
						 errdetail("app_name: %s index is %d dbnode is %s weight is %f", app_name, index_app,
								   pool_config->app_name_redirect_tokens->token[index_app].right_token,
								   pool_config->app_name_redirect_tokens->token[index_app].weight_token)));

				tmp = choose_db_node_id(pool_config->app_name_redirect_tokens->token[index_app].right_token);
				if (tmp == -1 || (tmp >= 0 && VALID_BACKEND(tmp)))
					suggested_node_id = tmp;
			}
		}
	}

	if (suggested_node_id >= 0)
	{
		/*
		 * If the weight is bigger than random rate then send to
		 * suggested_node_id. If the weight is less than random rate then
		 * choose load balance node from other nodes.
		 */
		if ((index_db >= 0 && r <= pool_config->db_redirect_tokens->token[index_db].weight_token) ||
			(index_app >= 0 && r <= pool_config->app_name_redirect_tokens->token[index_app].weight_token))
		{
			ereport(DEBUG1,
					(errmsg("selecting load balance node"),
					 errdetail("selected backend id is %d", suggested_node_id)));
			return suggested_node_id;
		}
		else
			no_load_balance_node_id = suggested_node_id;
	}

	/* In case of sending to standby */
	if (suggested_node_id == -1)
	{
		/* If the weight is less than random rate then send to primary. */
		if ((index_db >= 0 && r > pool_config->db_redirect_tokens->token[index_db].weight_token) ||
			(index_app >= 0 && r > pool_config->app_name_redirect_tokens->token[index_app].weight_token))
		{
			ereport(DEBUG1,
					(errmsg("selecting load balance node"),
					 errdetail("selected backend id is %d", PRIMARY_NODE_ID)));
			return PRIMARY_NODE_ID;
		}
	}

	/* Choose a backend in random manner with weight */
	selected_slot = MASTER_NODE_ID;
	total_weight = 0.0;

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (VALID_BACKEND_RAW(i))
		{
			if (i == no_load_balance_node_id)
				continue;
			if (suggested_node_id == -1)
			{
				if (i != PRIMARY_NODE_ID)
					total_weight += BACKEND_INFO(i).backend_weight;
			}
			else
				total_weight += BACKEND_INFO(i).backend_weight;
		}
	}

#if defined(sun) || defined(__sun)
	r = (((double) rand()) / RAND_MAX) * total_weight;
#else
	r = (((double) random()) / RAND_MAX) * total_weight;
#endif

	total_weight = 0.0;
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if ((suggested_node_id == -1 && i == PRIMARY_NODE_ID) || i == no_load_balance_node_id)
			continue;

		if (VALID_BACKEND_RAW(i) && BACKEND_INFO(i).backend_weight > 0.0)
		{
			if (r >= total_weight)
				selected_slot = i;
			else
				break;
			total_weight += BACKEND_INFO(i).backend_weight;
		}
	}
	ereport(DEBUG1,
			(errmsg("selecting load balance node"),
			 errdetail("selected backend id is %d", selected_slot)));
	return selected_slot;
}

/*
 * Returns PostgreSQL version.
 * The returned PgVersion struct is in static memory.
 * Caller must not modify it.
 *
 * Note:
 * Must be called while query context already exists.
 * If there's something goes wrong, this raises FATAL. So never returns to caller.
 *
 */
PGVersion *
Pgversion(POOL_CONNECTION_POOL * backend)
{
#define VERSION_BUF_SIZE	10
	static	PGVersion	pgversion;
	static	POOL_RELCACHE *relcache;
	char	*result;
	char	*p;
	char	buf[VERSION_BUF_SIZE];
	int		i;
	int		major;
	int		minor;

	/*
	 * First, check local cache. If cache is set, just return it.
	 */
	if (pgversion.major != 0)
	{
		ereport(DEBUG5,
				(errmsg("Pgversion: local cache returned")));

		return &pgversion;
	}

	if (!relcache)
	{
		/*
		 * Create relcache.
		 */
		relcache = pool_create_relcache(pool_config->relcache_size, "SELECT version()",
										string_register_func, string_unregister_func, false);
		if (relcache == NULL)
		{
			ereport(FATAL,
					(errmsg("Pgversion: unable to create relcache while getting PostgreSQL version.")));
			return NULL;
		}
	}

	/*
	 * Search relcache.
	 */
	result = (char *)pool_search_relcache(relcache, backend, "version");
	if (result == 0)
	{
		ereport(FATAL,
				(errmsg("Pgversion: unable to search relcache while getting PostgreSQL version.")));
		return NULL;
	}

	ereport(DEBUG5,
			(errmsg("Pgversion: version string: %s", result)));

	/*
	 * Extract major version number.  We create major version as "version" *
	 * 10.  For example, for V10, the major version number will be 100, for
	 * V9.6 it will be 96, and so on.  For alpha or beta version, the version
	 * string could be something like "12beta1". In this case we assume that
	 * atoi(3) is smart enough to stop at the first character which is not a
	 * valid digit (in our case 'b')). So "12beta1" should be converted to 12.
	 */
	p = strchr(result, ' ');
	if (p == NULL)
	{
		ereport(FATAL,
				(errmsg("Pgversion: unable to find the first space in the version string: %s", result)));
		return NULL;
	}

	p++;
	i = 0;
	while (i < VERSION_BUF_SIZE - 1 && p && *p != '.')
	{
		buf[i++] = *p++;
	}
	buf[i] = '\0';
	major = atoi(buf);
	ereport(DEBUG5,
			(errmsg("Pgversion: major version: %d", major)));

	/* Assuming PostgreSQL V100 is the final release:-) */
	if (major < 6 || major > 100)
	{
		ereport(FATAL,
				(errmsg("Pgversion: wrong major version: %d", major)));
		return NULL;
	}

	/*
	 * If major version is 10 or above, we are done to extract major.
	 * Otherwise extract below decimal point part.
	 */
	if (major >= 10)
	{
		major *= 10;
	}
	else
	{
		p++;
		i = 0;
		while (i < VERSION_BUF_SIZE -1 && p && *p != '.' && *p != ' ')
		{
			buf[i++] = *p++;
		}
		buf[i] = '\0';
		major = major * 10 + atoi(buf);
		ereport(DEBUG5,
				(errmsg("Pgversion: major version: %d", major)));
		pgversion.major = major;
	}

	/*
	 * Extract minor version.
	 */
	p++;
	i = 0;
	while (i < VERSION_BUF_SIZE -1 && p && *p != '.' && *p != ' ')
	{
		buf[i++] = *p++;
	}
	buf[i] = '\0';
	minor = atoi(buf);
	ereport(DEBUG5,
			(errmsg("Pgversion: minor version: %d", minor)));

	if (minor < 0 || minor > 100)
	{
		ereport(FATAL,
				(errmsg("Pgversion: wrong minor version: %d", minor)));
		return NULL;
	}


	/*
	 * Ok, everything looks good. Set the local cache.
	 */
	pgversion.major = major;
	pgversion.minor = minor;
	strncpy(pgversion.version_string, result, sizeof(pgversion.version_string) - 1);

	return &pgversion;
}

/*
 * Given db node specified in pgpool.conf, returns appropriate physical
 * DB node id.
 * Acceptable db node specifications are:
 *
 * primary: primary node
 * standby: any of standby node
 * numeric: physical node id
 *
 * If specified node does exist, returns MASTER_NODE_ID.  If "standby" is
 * specified, returns -1. Caller should choose one of standby nodes
 * appropriately.
 */
static int
choose_db_node_id(char *str)
{
	int			node_id = MASTER_NODE_ID;

	if (!strcmp("primary", str) && PRIMARY_NODE_ID >= 0)
	{
		node_id = PRIMARY_NODE_ID;
	}
	else if (!strcmp("standby", str))
	{
		node_id = -1;
	}
	else
	{
		int			tmp = atoi(str);

		if (tmp >= 0 && tmp < NUM_BACKENDS)
			node_id = tmp;
	}
	return node_id;
}
