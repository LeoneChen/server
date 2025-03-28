/*
   Copyright (c) 2000, 2014, Oracle and/or its affiliates.
   Copyright (c) 2010, 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* maintenance of mysql databases */

#include "client_priv.h"
#include <signal.h>
#include <my_pthread.h>				/* because of signal()	*/
#include <sys/stat.h>
#include <mysql.h>
#include <mysql_version.h>
#include <welcome_copyright_notice.h>
#include <my_rnd.h>
#include <password.h>
#include <my_sys.h>

#define ADMIN_VERSION "10.0"
#define MAX_MYSQL_VAR 512
#define SHUTDOWN_DEF_TIMEOUT 3600		/* Wait for shutdown */
#define MAX_TRUNC_LENGTH 3

char *host= NULL, *user= 0, *opt_password= 0,
     *default_charset= (char*) MYSQL_AUTODETECT_CHARSET_NAME;
char truncated_var_names[MAX_MYSQL_VAR+100][MAX_TRUNC_LENGTH];
char ex_var_names[MAX_MYSQL_VAR+100][FN_REFLEN];
ulonglong last_values[MAX_MYSQL_VAR+100];
static int interval=0;
static my_bool option_force=0,interrupted=0,new_line=0,
               opt_compress= 0, opt_local= 0, opt_relative= 0,
               opt_vertical= 0, tty_password= 0, opt_nobeep,
               opt_shutdown_wait_for_slaves= 0, opt_not_used;
static my_bool debug_info_flag= 0, debug_check_flag= 0;
static uint tcp_port = 0, option_wait = 0, option_silent=0, nr_iterations;
static uint opt_count_iterations= 0, my_end_arg, opt_verbose= 0;
static ulong opt_connect_timeout, opt_shutdown_timeout;
static char * unix_port=0;
static char *opt_plugin_dir= 0, *opt_default_auth= 0;
static bool sql_log_bin_off= false;

static uint opt_protocol=0;
static myf error_flags; /* flags to pass to my_printf_error, like ME_BELL */

/*
  When using extended-status relatively, ex_val_max_len is the estimated
  maximum length for any relative value printed by extended-status. The
  idea is to try to keep the length of output as short as possible.
*/

static uint ex_val_max_len[MAX_MYSQL_VAR];
static my_bool ex_status_printed = 0; /* First output is not relative. */
static uint ex_var_count, max_var_length, max_val_length;

#include <sslopt-vars.h>

static void print_version(void);
static void usage(void);
extern "C" my_bool get_one_option(int optid, const struct my_option *opt,
                                  const char *argument);
static my_bool sql_connect(MYSQL *mysql, uint wait);
static int execute_commands(MYSQL *mysql,int argc, char **argv);
static char **mask_password(int argc, char ***argv);
static int drop_db(MYSQL *mysql,const char *db);
extern "C" sig_handler endprog(int signal_number);
static void nice_time(ulong sec,char *buff);
static void print_header(MYSQL_RES *result);
static void print_top(MYSQL_RES *result);
static void print_row(MYSQL_RES *result,MYSQL_ROW cur, uint row);
static void print_relative_row(MYSQL_RES *result, MYSQL_ROW cur, uint row);
static void print_relative_row_vert(MYSQL_RES *result, MYSQL_ROW cur, uint row);
static void print_relative_header();
static void print_relative_line();
static void truncate_names();
static my_bool get_pidfile(MYSQL *mysql, char *pidfile);
static my_bool wait_pidfile(char *pidfile, time_t last_modified,
			    struct stat *pidfile_status);
static void store_values(MYSQL_RES *result);

/*
  The order of commands must be the same as command_names,
  except ADMIN_ERROR
*/
enum commands {
  ADMIN_ERROR,
  ADMIN_CREATE,           ADMIN_DROP,            ADMIN_SHUTDOWN,
  ADMIN_RELOAD,           ADMIN_REFRESH,         ADMIN_VER,
  ADMIN_PROCESSLIST,      ADMIN_STATUS,          ADMIN_KILL,
  ADMIN_DEBUG,            ADMIN_VARIABLES,       ADMIN_FLUSH_LOGS,
  ADMIN_FLUSH_HOSTS,      ADMIN_FLUSH_TABLES,    ADMIN_PASSWORD,
  ADMIN_PING,             ADMIN_EXTENDED_STATUS, ADMIN_FLUSH_STATUS,
  ADMIN_FLUSH_PRIVILEGES, ADMIN_START_SLAVE,     ADMIN_STOP_SLAVE,
  ADMIN_START_ALL_SLAVES, ADMIN_STOP_ALL_SLAVES,
  ADMIN_FLUSH_THREADS,    ADMIN_OLD_PASSWORD,    ADMIN_FLUSH_BINARY_LOG,
  ADMIN_FLUSH_ENGINE_LOG, ADMIN_FLUSH_ERROR_LOG, ADMIN_FLUSH_GENERAL_LOG,
  ADMIN_FLUSH_RELAY_LOG,  ADMIN_FLUSH_SLOW_LOG,
  ADMIN_FLUSH_TABLE_STATISTICS, ADMIN_FLUSH_INDEX_STATISTICS,
  ADMIN_FLUSH_USER_STATISTICS, ADMIN_FLUSH_CLIENT_STATISTICS,
  ADMIN_FLUSH_USER_RESOURCES,
  ADMIN_FLUSH_ALL_STATUS, ADMIN_FLUSH_ALL_STATISTICS, ADMIN_FLUSH_SSL
};
static const char *command_names[]= {
  "create",               "drop",                "shutdown",
  "reload",               "refresh",             "version",
  "processlist",          "status",              "kill",
  "debug",                "variables",           "flush-logs",
  "flush-hosts",          "flush-tables",        "password",
  "ping",                 "extended-status",     "flush-status",
  "flush-privileges",     "start-slave",         "stop-slave",
  "start-all-slaves", "stop-all-slaves",
  "flush-threads", "old-password", "flush-binary-log", "flush-engine-log",
  "flush-error-log", "flush-general-log", "flush-relay-log", "flush-slow-log",
  "flush-table-statistics", "flush-index-statistics",
  "flush-user-statistics", "flush-client-statistics", "flush-user-resources",
  "flush-all-status", "flush-all-statistics", "flush-ssl",
  NullS
};

static TYPELIB command_typelib=
{ array_elements(command_names)-1,"commands", command_names, NULL};

static struct my_option my_long_options[] =
{
  {"count", 'c',
   "Number of iterations to make. This works with -i (--sleep) only.",
   &nr_iterations, &nr_iterations, 0, GET_UINT,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DBUG_OFF
  {"debug", '#', "Output debug log. Often this is 'd:t:o,filename'.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"debug-check", 0, "Check memory and open file usage at exit.",
   &debug_check_flag, &debug_check_flag, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"debug-info", 0, "Print some debug info at exit.",
   &debug_info_flag, &debug_info_flag,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"force", 'f',
   "Don't ask for confirmation on drop database; with multiple commands, "
   "continue even if an error occurs.",
   &option_force, &option_force, 0, GET_BOOL, NO_ARG, 0, 0,
   0, 0, 0, 0},
  {"compress", 'C', "Use compression in server/client protocol.",
   &opt_compress, &opt_compress, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"character-sets-dir", OPT_CHARSETS_DIR,
   "Directory for character set files.", &charsets_dir,
   &charsets_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default-character-set", 0,
   "Set the default character set.", &default_charset,
   &default_charset, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"help", '?', "Display this help and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"host", 'h', "Connect to host.", &host, &host, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"local", 'l', "Local command, don't write to binlog.",
   &opt_local, &opt_local, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"no-beep", 'b', "Turn off beep on error.", &opt_nobeep,
   &opt_nobeep, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0}, 
  {"password", 'p',
   "Password to use when connecting to server. If password is not given it's asked from the tty.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#ifdef _WIN32
  {"pipe", 'W', "Use named pipes to connect to server.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"port", 'P', "Port number to use for connection or 0 for default to, in "
   "order of preference, my.cnf, $MYSQL_TCP_PORT, "
#if MYSQL_PORT_DEFAULT == 0
   "/etc/services, "
#endif
   "built-in default (" STRINGIFY_ARG(MYSQL_PORT) ").",
   &tcp_port, &tcp_port, 0, GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"protocol", OPT_MYSQL_PROTOCOL, "The protocol to use for connection (tcp, socket, pipe).",
    0, 0, 0, GET_STR,  REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"relative", 'r',
   "Show difference between current and previous values when used with -i. "
   "Currently only works with extended-status.",
   &opt_relative, &opt_relative, 0, GET_BOOL, NO_ARG, 0, 0, 0,
  0, 0, 0},
  {"silent", 's', "Silently exit if one can't connect to server.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"socket", 'S', "The socket file to use for connection.",
   &unix_port, &unix_port, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
   0, 0, 0},
  {"sleep", 'i', "Execute commands repeatedly with a sleep between.",
   &interval, &interval, 0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},
#include <sslopt-longopts.h>
#ifndef DONT_ALLOW_USER_CHANGE
  {"user", 'u', "User for login if not current user.", &user,
   &user, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"verbose", 'v', "Write more information."
  "Using it will print more information for 'processlist."
  "Using it 2 times will print even more information for 'processlist'.",
   &opt_not_used, &opt_not_used, 0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"vertical", 'E',
   "Print output vertically. Is similar to --relative, but prints output vertically.",
   &opt_vertical, &opt_vertical, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"wait", 'w', "Wait and retry if connection is down.", 0, 0, 0, GET_UINT,
   OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"connect_timeout", 0, "", &opt_connect_timeout,
   &opt_connect_timeout, 0, GET_ULONG, REQUIRED_ARG, 3600*12, 0,
   3600*12, 0, 1, 0},
  {"shutdown_timeout", 0, "", &opt_shutdown_timeout,
   &opt_shutdown_timeout, 0, GET_ULONG, REQUIRED_ARG,
   SHUTDOWN_DEF_TIMEOUT, 0, 3600*12, 0, 1, 0},
  {"wait_for_all_slaves", 0,
   "Defers shutdown until after all binlogged events have been sent to "
   "all connected slaves", &opt_shutdown_wait_for_slaves,
   &opt_shutdown_wait_for_slaves, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"plugin_dir", 0, "Directory for client-side plugins.",
    &opt_plugin_dir, &opt_plugin_dir, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default_auth", 0,
   "Default authentication client-side plugin to use.",
   &opt_default_auth, &opt_default_auth, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static const char *load_default_groups[]=
{ "mysqladmin", "mariadb-admin", "client", "client-server", "client-mariadb",
  0 };

my_bool
get_one_option(const struct my_option *opt, const char *argument,
               const char *filename)
{
  switch(opt->id) {
  case 'c':
    opt_count_iterations= 1;
    break;
  case 'p':
    if (argument == disabled_my_option)
      argument= (char*) "";			// Don't require password
    if (argument)
    {
      /*
        One should not really change the argument, but we make an
        exception for passwords
      */
      char *start= (char*) argument;
      my_free(opt_password);
      opt_password=my_strdup(PSI_NOT_INSTRUMENTED, argument,MYF(MY_FAE));
      while (*argument)
        *(char*) argument++= 'x';		/* Destroy argument */
      if (*start)
	start[1]=0;				/* Cut length of argument */
      tty_password= 0;
    }
    else
      tty_password=1;
    break;
  case 's':
    option_silent++;
    break;
  case 'W':
#ifdef _WIN32
    opt_protocol = MYSQL_PROTOCOL_PIPE;
#endif
    break;
  case '#':
    DBUG_PUSH(argument ? argument : "d:t:o,/tmp/mysqladmin.trace");
    break;
#include <sslopt-case.h>
  case 'V':
    print_version();
    exit(0);
    break;
  case 'w':
    if (argument)
    {
      if ((option_wait=atoi(argument)) <= 0)
	option_wait=1;
    }
    else
      option_wait= ~(uint)0;
    break;
  case '?':
  case 'I':					/* Info */
    usage();
    exit(0);
  case 'v':                                     /* --verbose   */
    opt_verbose++;
    if (argument == disabled_my_option)
      opt_verbose= 0;
    break;
  case OPT_CHARSETS_DIR:
#if MYSQL_VERSION_ID > 32300
    charsets_dir = argument;
#endif
    break;
  case OPT_MYSQL_PROTOCOL:
    if ((opt_protocol= find_type_with_warning(argument, &sql_protocol_typelib,
                                              opt->name)) <= 0)
    {
      sf_leaking_memory= 1; /* no memory leak reports here */
      exit(1);
    }
    break;
  case 'P':
    if (filename[0] == '\0')
    {
      /* Port given on command line, switch protocol to use TCP */
      opt_protocol= MYSQL_PROTOCOL_TCP;
    }
    break;
  case 'S':
    if (filename[0] == '\0')
    {
      /*
        Socket given on command line, switch protocol to use SOCKETSt
        Except on Windows if 'protocol= pipe' has been provided in
        the config file or command line.
      */
      if (opt_protocol != MYSQL_PROTOCOL_PIPE)
      {
        opt_protocol= MYSQL_PROTOCOL_SOCKET;
      }
    }
    break;
  }
  return 0;
}


int main(int argc,char *argv[])
{
  int error= 0, temp_argc;
  MYSQL mysql;
  char **commands, **save_argv, **temp_argv;

  MY_INIT(argv[0]);
  sf_leaking_memory=1; /* don't report memory leaks on early exits */

  /* We need to know if protocol-related options originate from CLI args */
  my_defaults_mark_files = TRUE;

  load_defaults_or_exit("my", load_default_groups, &argc, &argv);
  save_argv = argv;				/* Save for free_defaults */

  if ((error=handle_options(&argc, &argv, my_long_options, get_one_option)))
    goto err2;
  temp_argv= mask_password(argc, &argv);
  temp_argc= argc;

  if (debug_info_flag)
    my_end_arg= MY_CHECK_ERROR | MY_GIVE_INFO;
  if (debug_check_flag)
    my_end_arg= MY_CHECK_ERROR;

  if (argc == 0)
  {
    usage();
    exit(1);
  }
  commands = temp_argv;
  if (tty_password)
    opt_password = my_get_tty_password(NullS);

  (void) signal(SIGINT,endprog);			/* Here if abort */
  (void) signal(SIGTERM,endprog);		/* Here if abort */

  sf_leaking_memory=0; /* from now on we cleanup properly */

  mysql_init(&mysql);
  if (opt_compress)
    mysql_options(&mysql,MYSQL_OPT_COMPRESS,NullS);
  if (opt_connect_timeout)
  {
    uint tmp=opt_connect_timeout;
    mysql_options(&mysql,MYSQL_OPT_CONNECT_TIMEOUT, (char*) &tmp);
  }
#ifdef HAVE_OPENSSL
  if (opt_use_ssl)
  {
    mysql_ssl_set(&mysql, opt_ssl_key, opt_ssl_cert, opt_ssl_ca,
		  opt_ssl_capath, opt_ssl_cipher);
    mysql_options(&mysql, MYSQL_OPT_SSL_CRL, opt_ssl_crl);
    mysql_options(&mysql, MYSQL_OPT_SSL_CRLPATH, opt_ssl_crlpath);
    mysql_options(&mysql, MARIADB_OPT_TLS_VERSION, opt_tls_version);
  }
  mysql_options(&mysql,MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                (char*)&opt_ssl_verify_server_cert);
#endif
  if (opt_protocol)
    mysql_options(&mysql,MYSQL_OPT_PROTOCOL,(char*)&opt_protocol);
  if (!strcmp(default_charset,MYSQL_AUTODETECT_CHARSET_NAME))
    default_charset= (char *)my_default_csname();
  mysql_options(&mysql, MYSQL_SET_CHARSET_NAME, default_charset);
  error_flags= (myf)(opt_nobeep ? 0 : ME_BELL);

  if (opt_plugin_dir && *opt_plugin_dir)
    mysql_options(&mysql, MYSQL_PLUGIN_DIR, opt_plugin_dir);

  if (opt_default_auth && *opt_default_auth)
    mysql_options(&mysql, MYSQL_DEFAULT_AUTH, opt_default_auth);

  mysql_options(&mysql, MYSQL_OPT_CONNECT_ATTR_RESET, 0);
  mysql_options4(&mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                 "program_name", "mysqladmin");
  if (sql_connect(&mysql, option_wait))
  {
    /*
      We couldn't get an initial connection and will definitely exit.
      The following just determines the exit-code we'll give.
    */

    unsigned int err= mysql_errno(&mysql);
    if (err >= CR_MIN_ERROR && err <= CR_MAX_ERROR)
      error= 1;
    else
    {
      /* Return 0 if all commands are PING */
      for (; argc > 0; argv++, argc--)
      {
        if (find_type(argv[0], &command_typelib, FIND_TYPE_BASIC) !=
            ADMIN_PING)
        {
          error= 1;
          break;
        }
      }
    }
  }
  else
  {
    /*
      --count=0 aborts right here. Otherwise iff --sleep=t ("interval")
      is given a t!=0, we get an endless loop, or n iterations if --count=n
      was given an n!=0. If --sleep wasn't given, we get one iteration.

      To wait, --wait loops the connection-attempts, while --sleep loops
      the command execution (endlessly if no --count is given).
    */

    while (!interrupted && (!opt_count_iterations || nr_iterations))
    {
      new_line = 0;

      if ((error= execute_commands(&mysql,argc,commands)))
      {
        /*
          Unknown/malformed command always aborts and can't be --forced.
          If the user got confused about the syntax, proceeding would be
          dangerous ...
        */
	if (error > 0)
	  break;

        error= -error; /* don't exit with negative error codes */
        /*
          Command was well-formed, but failed on the server. Might succeed
          on retry (if conditions on server change etc.), but needs --force
          to retry.
        */
        if (!option_force)
          break;
      }                                         /* if((error= ... */

      if (interval)                             /* --sleep=interval given */
      {
        if (opt_count_iterations && --nr_iterations == 0)
          break;

        /*
          If connection was dropped (unintentionally, or due to SHUTDOWN),
          re-establish it if --wait ("retry-connect") was given and user
          didn't signal for us to die. Otherwise, signal failure.
        */

	if (mysql.net.pvio == 0)
	{
	  if (option_wait && !interrupted)
	  {
	    sleep(1);
	    sql_connect(&mysql, option_wait);
	    /*
	      continue normally and decrease counters so that
	      "mysqladmin --count=1 --wait=1 shutdown"
	      cannot loop endlessly.
	    */
	  }
	  else
	  {
	    /*
	      connexion broke, and we have no order to re-establish it. fail.
	    */
	    if (!option_force)
	      error= 1;
	    break;
	  }
	}                                       /* lost connection */

	sleep(interval);
	if (new_line)
	  puts("");
      }
      else
        break;                                  /* no --sleep, done looping */
    }                                           /* command-loop */
  }                                             /* got connection */

  mysql_close(&mysql);
  temp_argc--;
  while(temp_argc >= 0)
  {
    my_free(temp_argv[temp_argc]);
    temp_argc--;
  }
  my_free(temp_argv);
err2:
  mysql_library_end();
  my_free(opt_password);
  my_free(user);
  free_defaults(save_argv);
  my_end(my_end_arg);
  return error;
}


sig_handler endprog(int signal_number __attribute__((unused)))
{
  interrupted=1;
}

/**
   @brief connect to server, optionally waiting for same to come up

   @param  mysql     connection struct
   @param  wait      wait for server to come up?
                     (0: no, ~0: forever, n: cycles)

   @return Operation result
   @retval 0         success
   @retval 1         failure
*/

static my_bool sql_connect(MYSQL *mysql, uint wait)
{
  my_bool info=0;

  for (;;)
  {
    if (mysql_real_connect(mysql,host,user,opt_password,NullS,tcp_port,
			   unix_port, CLIENT_REMEMBER_OPTIONS))
    {
      my_bool reconnect= 1;
      mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
      if (info)
      {
	fputs("\n",stderr);
	(void) fflush(stderr);
      }
      return 0;
    }

    if (!wait)                                  // was or reached 0, fail
    {
      if (!option_silent)                       // print diagnostics
      {
	if (!host)
	  host= (char*) LOCAL_HOST;
	my_printf_error(0,"connect to server at '%s' failed\nerror: '%s'",
			error_flags, host, mysql_error(mysql));
	if (mysql_errno(mysql) == CR_CONNECTION_ERROR)
	{
	  fprintf(stderr,
		  "Check that mariadbd is running and that the socket: '%s' exists!\n",
		  unix_port ? unix_port : mysql_unix_port);
	}
	else if (mysql_errno(mysql) == CR_CONN_HOST_ERROR ||
		 mysql_errno(mysql) == CR_UNKNOWN_HOST)
	{
	  fprintf(stderr,"Check that mariadbd is running on %s",host);
	  fprintf(stderr," and that the port is %d.\n",
		  tcp_port ? tcp_port: mysql_port);
	  fprintf(stderr,"You can check this by doing 'telnet %s %d'\n",
		  host, tcp_port ? tcp_port: mysql_port);
	}
      }
      return 1;
    }

    if (wait != (uint) ~0)
      wait--;				/* count down, one less retry */

    if ((mysql_errno(mysql) != CR_CONN_HOST_ERROR) &&
	(mysql_errno(mysql) != CR_CONNECTION_ERROR))
    {
      /*
        Error is worse than "server doesn't answer (yet?)";
        fail even if we still have "wait-coins" unless --force
        was also given.
      */
      fprintf(stderr,"Got error: %s\n", mysql_error(mysql));
      if (!option_force)
	return 1;
    }
    else if (!option_silent)
    {
      if (!info)
      {
	info=1;
	fputs("Waiting for MariaDB server to answer",stderr);
	(void) fflush(stderr);
      }
      else
      {
	putc('.',stderr);
	(void) fflush(stderr);
      }
    }
    sleep(5);
  }
}


static int maybe_disable_binlog(MYSQL *mysql)
{
  if (opt_local && !sql_log_bin_off)
  {
    if (mysql_query(mysql,  "set local sql_log_bin=0"))
    {
      my_printf_error(0, "SET LOCAL SQL_LOG_BIN=0 failed; error: '%-.200s'",
                      error_flags, mysql_error(mysql));
      return -1;
    }
  }
  sql_log_bin_off= true;
  return 0;
}


int flush(MYSQL *mysql, const char *what)
{
  char buf[FN_REFLEN];
  my_snprintf(buf, sizeof(buf), "flush %s%s",
              (opt_local && !sql_log_bin_off ? "local " : ""), what);

  if (mysql_query(mysql, buf))
  {
    my_printf_error(0, "flush %s failed; error: '%s'", error_flags, what,
                    mysql_error(mysql));
    return -1;
  }
  return 0;
}


/**
   @brief Execute all commands

   @details We try to execute all commands we were given, in the order
            given, but return with non-zero as soon as we encounter trouble.
            By that token, individual commands can be considered a conjunction
            with boolean short-cut.

   @return success?
   @retval 0       Yes!  ALL commands worked!
   @retval 1       No, one failed and will never work (malformed): fatal error!
   @retval -1      No, one failed on the server, may work next time!
*/

static int execute_commands(MYSQL *mysql,int argc, char **argv)
{
  int ret = 0;
  const char *status;
  /*
    MySQL documentation relies on the fact that mysqladmin will
    execute commands in the order specified, e.g.
    mysqladmin -u root flush-privileges password "newpassword"
    to reset a lost root password.
    If this behaviour is ever changed, Docs should be notified.
  */

  struct my_rnd_struct rand_st;
  char buff[FN_REFLEN + 20];

  for (; argc > 0 ; argv++,argc--)
  {
    int command;
    switch ((command= find_type(argv[0],&command_typelib,FIND_TYPE_BASIC))) {
    case ADMIN_CREATE:
    {
      if (argc < 2)
      {
	my_printf_error(0, "Too few arguments to create", error_flags);
	return 1;
      }
      if (maybe_disable_binlog(mysql))
        return -1;
      sprintf(buff,"create database `%.*s`",FN_REFLEN,argv[1]);
      if (mysql_query(mysql,buff))
      {
	my_printf_error(0,"CREATE DATABASE failed; error: '%-.200s'",
			error_flags, mysql_error(mysql));
	return -1;
      }
      argc--; argv++;
      break;
    }
    case ADMIN_DROP:
    {
      if (argc < 2)
      {
	my_printf_error(0, "Too few arguments to drop", error_flags);
	return 1;
      }
      if (maybe_disable_binlog(mysql))
        return -1;
      if (drop_db(mysql,argv[1]))
	return -1;
      argc--; argv++;
      break;
    }
    case ADMIN_SHUTDOWN:
    {
      char pidfile[FN_REFLEN];
      my_bool got_pidfile= 0;
      time_t last_modified= 0;
      struct stat pidfile_status;

      /*
	Only wait for pidfile on local connections
	If pidfile doesn't exist, continue without pid file checking
      */
      if (mysql->unix_socket && (got_pidfile= !get_pidfile(mysql, pidfile)) &&
	  !stat(pidfile, &pidfile_status))
	last_modified= pidfile_status.st_mtime;

      if (opt_shutdown_wait_for_slaves)
      {
        sprintf(buff, "SHUTDOWN WAIT FOR ALL SLAVES");
        if (mysql_query(mysql, buff))
        {
          my_printf_error(0, "%s failed; error: '%-.200s'",
                          error_flags, buff, mysql_error(mysql));
          return -1;
        }
      }
      else if (mysql_shutdown(mysql, SHUTDOWN_DEFAULT))
      {
	my_printf_error(0, "shutdown failed; error: '%s'", error_flags,
			mysql_error(mysql));
	return -1;
      }
      argc=1;                   /* force SHUTDOWN to be the last command    */
      if (got_pidfile)
      {
	if (opt_verbose)
	  printf("Shutdown signal sent to server;  Waiting for pid file to disappear\n");

	/* Wait until pid file is gone */
	if (wait_pidfile(pidfile, last_modified, &pidfile_status))
	  return -1;
      }
      break;
    }
    case ADMIN_FLUSH_PRIVILEGES:
    case ADMIN_RELOAD:
      if (flush(mysql, "privileges"))
	return -1;
      break;
    case ADMIN_REFRESH:
      if (mysql_refresh(mysql,
			(uint) ~(REFRESH_GRANT | REFRESH_STATUS |
				 REFRESH_READ_LOCK | REFRESH_SLAVE |
				 REFRESH_MASTER)))
      {
	my_printf_error(0, "refresh failed; error: '%s'", error_flags,
			mysql_error(mysql));
	return -1;
      }
      break;
    case ADMIN_FLUSH_THREADS:
      if (mysql_refresh(mysql,(uint) REFRESH_THREADS))
      {
	my_printf_error(0, "refresh failed; error: '%s'", error_flags,
			mysql_error(mysql));
	return -1;
      }
      break;
    case ADMIN_VER:
      new_line=1;
      print_version();
      puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
      printf("Server version\t\t%s\n", mysql_get_server_info(mysql));
      printf("Protocol version\t%d\n", mysql_get_proto_info(mysql));
      printf("Connection\t\t%s\n",mysql_get_host_info(mysql));
      if (mysql->unix_socket)
	printf("UNIX socket\t\t%s\n", mysql->unix_socket);
      else
	printf("TCP port\t\t%d\n", mysql->port);
      status=mysql_stat(mysql);
      {
	char *pos,buff[40];
	ulong sec;
	pos= (char*) strchr(status,' ');
	*pos++=0;
	printf("%s\t\t\t",status);			/* print label */
	if ((status=str2int(pos,10,0,LONG_MAX,(long*) &sec)))
	{
	  nice_time(sec,buff);
	  puts(buff);				/* print nice time */
	  while (*status == ' ') status++;	/* to next info */
	}
      }
      putc('\n',stdout);
      if (status)
	puts(status);
      break;
    case ADMIN_PROCESSLIST:
    {
      MYSQL_RES *result;
      MYSQL_ROW row;
      const char *query;

      if (!opt_verbose)
        query= "show processlist";
      else if (opt_verbose == 1)
        query= "show full processlist";
      else
        query= "select * from information_schema.processlist where id != connection_id()";

      if (mysql_query(mysql, query) ||
          !(result = mysql_store_result(mysql)))
      {
	my_printf_error(0, "process list failed; error: '%s'", error_flags,
			mysql_error(mysql));
	return -1;
      }
      print_header(result);
      while ((row=mysql_fetch_row(result)))
	print_row(result,row,0);
      print_top(result);
      mysql_free_result(result);
      new_line=1;
      break;
    }
    case ADMIN_STATUS:
      status=mysql_stat(mysql);
      if (status)
	puts(status);
      break;
    case ADMIN_KILL:
      {
	uint error=0;
	char *pos;
	if (argc < 2)
	{
	  my_printf_error(0, "Too few arguments to 'kill'", error_flags);
	  return 1;
	}
	pos=argv[1];
	for (;;)
	{
          /* We don't use mysql_kill(), since it only handles 32-bit IDs. */
          char buff[26], *out; /* "KILL " + max 20 digs + NUL */
          out= strxmov(buff, "KILL ", NullS);
          ullstr(strtoull(pos, NULL, 0), out);

          if (mysql_query(mysql, buff))
	  {
            /* out still points to just the number */
	    my_printf_error(0, "kill failed on %s; error: '%s'", error_flags,
			    out, mysql_error(mysql));
	    error=1;
	  }
	  if (!(pos=strchr(pos,',')))
	    break;
	  pos++;
	}
	argc--; argv++;
	if (error)
	  return -1;
	break;
      }
    case ADMIN_DEBUG:
      if (mysql_dump_debug_info(mysql))
      {
	my_printf_error(0, "debug failed; error: '%s'", error_flags,
			mysql_error(mysql));
	return -1;
      }
      break;
    case ADMIN_VARIABLES:
    {
      MYSQL_RES *res;
      MYSQL_ROW row;

      new_line=1;
      if (mysql_query(mysql,"show /*!40003 GLOBAL */ variables") ||
	  !(res=mysql_store_result(mysql)))
      {
	my_printf_error(0, "unable to show variables; error: '%s'", error_flags,
			mysql_error(mysql));
	return -1;
      }
      print_header(res);
      while ((row=mysql_fetch_row(res)))
	print_row(res,row,0);
      print_top(res);
      mysql_free_result(res);
      break;
    }
    case ADMIN_EXTENDED_STATUS:
    {
      MYSQL_RES *res;
      MYSQL_ROW row;
      uint rownr = 0;
      void (*func) (MYSQL_RES*, MYSQL_ROW, uint);

      new_line = 1;
      if (mysql_query(mysql, "show /*!50002 GLOBAL */ status") ||
	  !(res = mysql_store_result(mysql)))
      {
	my_printf_error(0, "unable to show status; error: '%s'", error_flags,
			mysql_error(mysql));
	return -1;
      }

      DBUG_ASSERT(mysql_num_rows(res) < MAX_MYSQL_VAR+100);

      if (!opt_vertical)
	print_header(res);
      else
      {
	if (!ex_status_printed)
	{
	  store_values(res);
	  truncate_names();   /* Does some printing also */
	}
	else
	{
	  print_relative_line();
	  print_relative_header();
	  print_relative_line();
	}
      }

      /*      void (*func) (MYSQL_RES*, MYSQL_ROW, uint); */
      if (opt_relative && !opt_vertical)
	func = print_relative_row;
      else if (opt_vertical)
	func = print_relative_row_vert;
      else
	func = print_row;

      while ((row = mysql_fetch_row(res)))
	(*func)(res, row, rownr++);
      if (opt_vertical)
      {
	if (ex_status_printed)
	{
	  putchar('\n');
	  print_relative_line();
	}
      }
      else
	print_top(res);

      ex_status_printed = 1; /* From now on the output will be relative */
      mysql_free_result(res);
      break;
    }
    case ADMIN_FLUSH_LOGS:
    {
      if (flush(mysql, "logs"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_BINARY_LOG:
    {
      if (flush(mysql, "binary logs"))
        return -1;
      break;
    }
    case ADMIN_FLUSH_ENGINE_LOG:
    {
      if (flush(mysql, "engine logs"))
        return -1;
      break;
    }
    case ADMIN_FLUSH_ERROR_LOG:
    {
      if (flush(mysql, "error logs"))
        return -1;
      break;
    }
    case ADMIN_FLUSH_GENERAL_LOG:
    {
      if (flush(mysql, "general logs"))
        return -1;
      break;
    }
    case ADMIN_FLUSH_RELAY_LOG:
    {
      if (flush(mysql, "relay logs"))
        return -1;
      break;
    }
    case ADMIN_FLUSH_SLOW_LOG:
    {
      if (flush(mysql, "slow logs"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_HOSTS:
    {
      if (flush(mysql, "hosts"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_TABLES:
    {
      if (flush(mysql, "tables"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_STATUS:
    {
      if (flush(mysql, "status"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_TABLE_STATISTICS:
    {
      if (flush(mysql, "table_statistics"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_INDEX_STATISTICS:
    {
      if (flush(mysql, "index_statistics"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_SSL:
    {
      if (flush(mysql, "ssl"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_USER_STATISTICS:
    {
      if (flush(mysql, "user_statistics"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_USER_RESOURCES:
    {
      if (flush(mysql, "user_resources"))
        return -1;
      break;
    }
    case ADMIN_FLUSH_CLIENT_STATISTICS:
    {
      if (flush(mysql, "client_statistics"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_ALL_STATISTICS:
    {
      if (flush(mysql, "table_statistics,index_statistics,"
                       "user_statistics,client_statistics"))
	return -1;
      break;
    }
    case ADMIN_FLUSH_ALL_STATUS:
    {
      if (flush(mysql, "status,table_statistics,index_statistics,"
                       "user_statistics,client_statistics"))
	return -1;
      break;
    }
    case ADMIN_OLD_PASSWORD:
    case ADMIN_PASSWORD:
    {
      char buff[128],crypted_pw[64];
      time_t start_time;
      char *typed_password= NULL, *verified= NULL;
      /* Do initialization the same way as we do in mysqld */
      start_time=time((time_t*) 0);
      my_rnd_init(&rand_st,(ulong) start_time,(ulong) start_time/2);

      if (maybe_disable_binlog(mysql))
        return -1;
      if (argc < 1)
      {
	my_printf_error(0, "Too few arguments to change password", error_flags);
	return 1;
      }
      else if (argc == 1)
      {
        /* prompt for password */
        typed_password= my_get_tty_password("New password: ");
        verified= my_get_tty_password("Confirm new password: ");
        if (strcmp(typed_password, verified) != 0)
        {
          my_printf_error(0,"Passwords don't match",MYF(ME_BELL));
          ret = -1;
          goto password_done;
        }
      }
      else
        typed_password= argv[1];

      if (typed_password[0])
      {
        bool old= (find_type(argv[0], &command_typelib, FIND_TYPE_BASIC) ==
                   ADMIN_OLD_PASSWORD);
#ifdef _WIN32
        size_t pw_len= strlen(typed_password);
        if (pw_len > 1 && typed_password[0] == '\'' &&
            typed_password[pw_len-1] == '\'')
          printf("Warning: single quotes were not trimmed from the password by"
                 " your command\nline client, as you might have expected.\n");
#endif
        /*
           If we don't already know to use an old-style password, see what
           the server is using
        */
        if (!old)
        {
          if (mysql_query(mysql, "SHOW VARIABLES LIKE 'old_passwords'"))
          {
            my_printf_error(0, "Could not determine old_passwords setting from server; error: '%s'",
                	    error_flags, mysql_error(mysql));
            ret = -1;
            goto password_done;
          }
          else
          {
            MYSQL_RES *res= mysql_store_result(mysql);
            if (!res)
            {
              my_printf_error(0,
                              "Could not get old_passwords setting from "
                              "server; error: '%s'",
        		      error_flags, mysql_error(mysql));
              ret = -1;
              goto password_done;
            }
            if (!mysql_num_rows(res))
              old= 1;
            else
            {
              MYSQL_ROW row= mysql_fetch_row(res);
              old= !strncmp(row[1], "ON", 2);
            }
            mysql_free_result(res);
          }
        }
        if (old)
          my_make_scrambled_password_323(crypted_pw, typed_password, strlen(typed_password));
        else
          my_make_scrambled_password(crypted_pw, typed_password, strlen(typed_password));
      }
      else
	crypted_pw[0]=0;			/* No password */
      sprintf(buff,"set password='%s',sql_log_off=0",crypted_pw);

      if (mysql_query(mysql,"set sql_log_off=1"))
      {
	my_printf_error(0, "Can't turn off logging; error: '%s'",
			error_flags, mysql_error(mysql));
        ret = -1;
      }
      else
      if (mysql_query(mysql,buff))
      {
        my_printf_error(0,"unable to change password; error: '%s'",
                        error_flags, mysql_error(mysql));
        ret = -1;
      }
password_done:
      /* free up memory from prompted password */
      if (typed_password != argv[1]) 
      {
        my_free(typed_password);
        my_free(verified);
      }
      argc--; argv++;
      break;
    }

    case ADMIN_START_SLAVE:
    case ADMIN_START_ALL_SLAVES:
    {
      my_bool many_slaves= 0;
      const char *query= "START SLAVE";
      if (command == ADMIN_START_ALL_SLAVES && mariadb_connection(mysql) &&
          mysql_get_server_version(mysql) >= 100000)
      {
        query="START ALL SLAVES";
        many_slaves= 1;
      }

      if (mysql_query(mysql, query))
      {
	my_printf_error(0, "Error starting slave: %s", error_flags,
			mysql_error(mysql));
	return -1;
      }
      else if (!many_slaves || mysql_warning_count(mysql) > 0)
      {
        if (!option_silent)
          puts("Slave('s) started");
      }
      else
      {
        if (!option_silent)
          puts("No slaves to start");
      }
      break;
    }
    case ADMIN_STOP_SLAVE:
    case ADMIN_STOP_ALL_SLAVES:
    {
      const char *query= "STOP SLAVE";
      my_bool many_slaves= 0;

      if (command == ADMIN_STOP_ALL_SLAVES && mariadb_connection(mysql) &&
          mysql_get_server_version(mysql) >= 100000)
      {
        query="STOP ALL SLAVES";
        many_slaves= 1;
      }

      if (mysql_query(mysql, query))
      {
	  my_printf_error(0, "Error stopping slave: %s", error_flags,
			  mysql_error(mysql));
	  return -1;
      }
      else if (!many_slaves || mysql_warning_count(mysql) > 0)
      {
        /* We can't detect if there was any slaves to stop with STOP SLAVE */
        if (many_slaves && !option_silent)
          puts("Slave('s) stopped");
      }
      else
      {
        if (!option_silent)
          puts("All slaves was already stopped");
      }
      break;
    }
    case ADMIN_PING:
    {
      my_bool reconnect= 0;
      mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
      if (!mysql_ping(mysql))
      {
	if (option_silent < 2)
	  puts("mysqld is alive");
      }
      else
      {
	if (mysql_errno(mysql) == CR_SERVER_GONE_ERROR)
	{
          reconnect= 1;
          mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
	  if (!mysql_ping(mysql))
	    puts("connection was down, but mysqld is now alive");
	}
	else
	{
	  my_printf_error(0,"mariadbd doesn't answer to ping, error: '%s'",
			  error_flags, mysql_error(mysql));
	  return -1;
	}
      }
      reconnect=1;	/* Automatic reconnect is default */
      mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
      break;
    }
    default:
      my_printf_error(0, "Unknown command: '%-.60s'", error_flags, argv[0]);
      return 1;
    }
  }
  return ret;
}

/**
   @brief Masking the password if it is passed as command line argument.

   @details It works in Linux and changes cmdline in ps and /proc/pid/cmdline,
            but it won't work for history file of shell.
            The command line arguments are copied to another array and the
            password in the argv is masked. This function is called just after
            "handle_options" because in "handle_options", the agrv pointers
            are altered which makes freeing of dynamically allocated memory
            difficult. The password masking is done before all other operations
            in order to minimise the time frame of password visibility via cmdline.

   @param argc            command line options (count)
   @param argv            command line options (values)

   @return temp_argv      copy of argv
*/

static char **mask_password(int argc, char ***argv)
{
  char **temp_argv;
  if (!argc)
    return NULL;

  temp_argv= (char **)(my_malloc(PSI_NOT_INSTRUMENTED, sizeof(char *) * argc, MYF(MY_WME)));
  argc--;
  while (argc > 0)
  {
    temp_argv[argc]= my_strdup(PSI_NOT_INSTRUMENTED, (*argv)[argc], MYF(MY_FAE));
    if (find_type((*argv)[argc - 1],&command_typelib, FIND_TYPE_BASIC) == ADMIN_PASSWORD ||
        find_type((*argv)[argc - 1],&command_typelib, FIND_TYPE_BASIC) == ADMIN_OLD_PASSWORD)
    {
      char *start= (*argv)[argc];
      while (*start)
        *start++= 'x';
      start= (*argv)[argc];
      if (*start)
        start[1]= 0;                         /* Cut length of argument */
     }
    argc--;
  }
  temp_argv[argc]= my_strdup(PSI_NOT_INSTRUMENTED, (*argv)[argc], MYF(MY_FAE));
  return(temp_argv);
}

static void print_version(void)
{
  printf("%s  Ver %s Distrib %s, for %s on %s\n",my_progname,ADMIN_VERSION,
	 MYSQL_SERVER_VERSION,SYSTEM_TYPE,MACHINE_TYPE);
}


static void usage(void)
{
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  puts("Administration program for the mysqld daemon.");
  printf("Usage: %s [OPTIONS] command command....\n", my_progname);
  print_defaults("my",load_default_groups);
  puts("");
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
  puts("\nWhere command is a one or more of: (Commands may be shortened)\n\
  create databasename	  Create a new database\n\
  debug			  Instruct server to write debug information to log\n\
  drop databasename	  Delete a database and all its tables\n\
  extended-status         Gives an extended status message from the server\n\
  flush-all-statistics    Flush all statistics tables\n\
  flush-all-status        Flush status and statistics\n\
  flush-client-statistics Flush client statistics\n\
  flush-hosts             Flush all cached hosts\n\
  flush-index-statistics  Flush index statistics\n\
  flush-logs              Flush all logs\n\
  flush-privileges        Reload grant tables (same as reload)\n\
  flush-binary-log        Flush binary log\n\
  flush-engine-log        Flush engine log(s)\n\
  flush-error-log         Flush error log\n\
  flush-general-log       Flush general log\n\
  flush-relay-log         Flush relay log\n\
  flush-slow-log          Flush slow query log\n\
  flush-ssl               Flush SSL certificates\n\
  flush-status            Clear status variables\n\
  flush-table-statistics  Clear table statistics\n\
  flush-tables            Flush all tables\n\
  flush-threads           Flush the thread cache\n\
  flush-user-statistics   Flush user statistics\n\
  flush-user-resources    Flush user resources\n\
  kill id,id,...	Kill mysql threads");
#if MYSQL_VERSION_ID >= 32200
  puts("\
  password [new-password] Change old password to new-password in current format\n\
  old-password [new-password] Change old password to new-password in old format");
#endif
  puts("\
  ping			Check if mysqld is alive\n\
  processlist		Show list of active threads in server\n\
  reload		Reload grant tables\n\
  refresh		Flush all tables and close and open logfiles\n\
  shutdown		Take server down\n\
  status		Gives a short status message from the server\n\
  start-all-slaves	Start all slaves\n\
  start-slave		Start slave\n\
  stop-all-slaves	Stop all slaves\n\
  stop-slave		Stop slave\n\
  variables             Prints variables available\n\
  version		Get version info from server");
}


static int drop_db(MYSQL *mysql, const char *db)
{
  char name_buff[FN_REFLEN+20], buf[10];
  char *input;

  if (!option_force)
  {
    puts("Dropping the database is potentially a very bad thing to do.");
    puts("Any data stored in the database will be destroyed.\n");
    printf("Do you really want to drop the '%s' database [y/N] ",db);
    fflush(stdout);
    input= fgets(buf, sizeof(buf)-1, stdin);
    if (!input || ((*input != 'y') && (*input != 'Y')))
    {
      puts("\nOK, aborting database drop!");
      return -1;
    }
  }
  sprintf(name_buff,"drop database `%.*s`",FN_REFLEN,db);
  if (mysql_query(mysql,name_buff))
  {
    my_printf_error(0, "DROP DATABASE %s failed;\nerror: '%s'", error_flags,
		    db,mysql_error(mysql));
    return 1;
  }
  printf("Database \"%s\" dropped\n",db);
  return 0;
}


static void nice_time(ulong sec,char *buff)
{
  ulong tmp;

  if (sec >= 3600L*24)
  {
    tmp=sec/(3600L*24);
    sec-=3600L*24*tmp;
    buff=int10_to_str(tmp, buff, 10);
    buff=strmov(buff,tmp > 1 ? " days " : " day ");
  }
  if (sec >= 3600L)
  {
    tmp=sec/3600L;
    sec-=3600L*tmp;
    buff=int10_to_str(tmp, buff, 10);
    buff=strmov(buff,tmp > 1 ? " hours " : " hour ");
  }
  if (sec >= 60)
  {
    tmp=sec/60;
    sec-=60*tmp;
    buff=int10_to_str(tmp, buff, 10);
    buff=strmov(buff," min ");
  }
  strmov(int10_to_str(sec, buff, 10)," sec");
}


static void print_header(MYSQL_RES *result)
{
  MYSQL_FIELD *field;

  print_top(result);
  mysql_field_seek(result,0);
  putchar('|');
  while ((field = mysql_fetch_field(result)))
  {
    printf(" %-*s|",(int) field->max_length+1,field->name);
  }
  putchar('\n');
  print_top(result);
}


static void print_top(MYSQL_RES *result)
{
  uint i,length;
  MYSQL_FIELD *field;

  putchar('+');
  mysql_field_seek(result,0);
  while((field = mysql_fetch_field(result)))
  {
    if ((length=(uint) strlen(field->name)) > field->max_length)
      field->max_length=length;
    else
      length=field->max_length;
    for (i=length+2 ; i--> 0 ; )
      putchar('-');
    putchar('+');
  }
  putchar('\n');
}


/* 3.rd argument, uint row, is not in use. Don't remove! */
static void print_row(MYSQL_RES *result, MYSQL_ROW cur,
		      uint row __attribute__((unused)))
{
  uint i,length;
  MYSQL_FIELD *field;

  putchar('|');
  mysql_field_seek(result,0);
  for (i=0 ; i < mysql_num_fields(result); i++)
  {
    field = mysql_fetch_field(result);
    length=field->max_length;
    printf(" %-*s|",length+1,cur[i] ? (char*) cur[i] : "");
  }
  putchar('\n');
}


static void print_relative_row(MYSQL_RES *result, MYSQL_ROW cur, uint row)
{
  ulonglong tmp;
  char buff[22];
  MYSQL_FIELD *field;

  mysql_field_seek(result, 0);
  field = mysql_fetch_field(result);
  printf("| %-*s|", (int) field->max_length + 1, cur[0]);

  field = mysql_fetch_field(result);
  tmp = cur[1] ? strtoull(cur[1], NULL, 10) : (ulonglong) 0;
  printf(" %-*s|\n", (int) field->max_length + 1,
	 llstr((tmp - last_values[row]), buff));
  last_values[row] = tmp;
}


static void print_relative_row_vert(MYSQL_RES *result __attribute__((unused)),
				    MYSQL_ROW cur,
				    uint row __attribute__((unused)))
{
  uint length;
  ulonglong tmp;
  char buff[22];

  if (!row)
    putchar('|');

  tmp = cur[1] ? strtoull(cur[1], NULL, 10) : (ulonglong) 0;
  printf(" %-*s|", ex_val_max_len[row] + 1,
	 llstr((tmp - last_values[row]), buff));

  /* Find the minimum row length needed to output the relative value */
  length=(uint) strlen(buff);
  if (length > ex_val_max_len[row] && ex_status_printed)
    ex_val_max_len[row] = length;
  last_values[row] = tmp;
}


static void store_values(MYSQL_RES *result)
{
  uint i;
  MYSQL_ROW row;
  MYSQL_FIELD *field;

  field = mysql_fetch_field(result);
  max_var_length = field->max_length;
  field = mysql_fetch_field(result);
  max_val_length = field->max_length;

  for (i = 0; (row = mysql_fetch_row(result)); i++)
  {
    strmov(ex_var_names[i], row[0]);
    last_values[i]=strtoull(row[1],NULL,10);
    ex_val_max_len[i]=2;		/* Default print width for values */
  }
  ex_var_count = i;
  return;
}


static void print_relative_header()
{
  uint i;

  putchar('|');
  for (i = 0; i < ex_var_count; i++)
    printf(" %-*s|", ex_val_max_len[i] + 1, truncated_var_names[i]);
  putchar('\n');
}


static void print_relative_line()
{
  uint i;

  putchar('+');
  for (i = 0; i < ex_var_count; i++)
  {
    uint j;
    for (j = 0; j < ex_val_max_len[i] + 2; j++)
      putchar('-');
    putchar('+');
  }
  putchar('\n');
}


static void truncate_names()
{
  uint i;
  char *ptr,top_line[MAX_TRUNC_LENGTH+4+NAME_LEN+22+1],buff[22];

  ptr=top_line;
  *ptr++='+';
  ptr=strfill(ptr,max_var_length+2,'-');
  *ptr++='+';
  ptr=strfill(ptr,MAX_TRUNC_LENGTH+2,'-');
  *ptr++='+';
  ptr=strfill(ptr,max_val_length+2,'-');
  *ptr++='+';
  *ptr=0;
  puts(top_line);

  for (i = 0 ; i < ex_var_count; i++)
  {
    uint sfx=1,j;
    printf("| %-*s|", max_var_length + 1, ex_var_names[i]);
    ptr = ex_var_names[i];
    /* Make sure no two same truncated names will become */
    for (j = 0; j < i; j++)
      if (*truncated_var_names[j] == *ptr)
	sfx++;

    truncated_var_names[i][0]= *ptr;		/* Copy first var char */
    int10_to_str(sfx, truncated_var_names[i]+1,10);
    printf(" %-*s|", MAX_TRUNC_LENGTH + 1, truncated_var_names[i]);
    printf(" %-*s|\n", max_val_length + 1, llstr(last_values[i],buff));
  }
  puts(top_line);
  return;
}


static my_bool get_pidfile(MYSQL *mysql, char *pidfile)
{
  MYSQL_RES* result;

  if (mysql_query(mysql, "SHOW VARIABLES LIKE 'pid_file'"))
  {
    my_printf_error(mysql_errno(mysql),
                    "The query to get the server's pid file failed,"
                    " error: '%s'. Continuing.", error_flags,
                    mysql_error(mysql));
  }
  result = mysql_store_result(mysql);
  if (result)
  {
    MYSQL_ROW row=mysql_fetch_row(result);
    if (row)
      strmov(pidfile, row[1]);
    mysql_free_result(result);
    return row == 0;				/* Error if row = 0 */
  }
  return 1;					/* Error */
}

/*
  Return 1 if pid file didn't disappear or change
*/

static my_bool wait_pidfile(char *pidfile, time_t last_modified,
			    struct stat *pidfile_status)
{
  char buff[FN_REFLEN];
  my_bool error= 1;
  uint count= 0;
  DBUG_ENTER("wait_pidfile");

  system_filename(buff, pidfile);
  do
  {
    int fd;
    if ((fd= my_open(buff, O_RDONLY, MYF(0))) < 0)
    {
      error= 0;
      break;
    }
    (void) my_close(fd,MYF(0));
    if (last_modified && !stat(pidfile, pidfile_status))
    {
      if (last_modified != pidfile_status->st_mtime)
      {
	/* File changed;  Let's assume that mysqld did restart */
	if (opt_verbose)
	  printf("pid file '%s' changed while waiting for it to disappear!\nmysqld did probably restart\n",
		 buff);
	error= 0;
	break;
      }
    }
    if (count++ == opt_shutdown_timeout)
      break;
    sleep(1);
  } while (!interrupted);

  if (error)
  {
    DBUG_PRINT("warning",("Pid file didn't disappear"));
    fprintf(stderr,
	    "Warning;  Aborted waiting on pid file: '%s' after %d seconds\n",
	    buff, count-1);
  }
  DBUG_RETURN(error);
}
