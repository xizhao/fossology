/* **************************************************************
Copyright (C) 2010 Hewlett-Packard Development Company, L.P.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
************************************************************** */

/* local includes */
#include <agent.h>
#include <database.h>
#include <event.h>
#include <host.h>
#include <interface.h>
#include <logging.h>
#include <scheduler.h>

/* std library includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* unix system includes */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

/* glib includes */
#include <glib.h>
#include <gio/gio.h>

char* process_name = "fossology-scheduler";

#ifndef PROJECT_USER
#define PROJECT_USER "fossology"
#endif
#ifndef PROJECT_GROUP
#define PROJECT_GROUP "fossyology"
#endif

/* global flags */
int verbose = 0;
int closing = 0;

/* user group and password of scheduler */
struct group*  grp;
struct passwd* pwd;

/* ************************************************************************** */
/* **** signals and events ************************************************** */
/* ************************************************************************** */

/**
 * handle signals from the child process. This will only be called on a SIGCHLD
 * and will handle the effects of the death of the child process.
 *
 * @param signo
 * @param INFO
 * @param context
 */
void chld_sig(int signo)
{
  int idx;          // index of the next empty location in the pid list
  pid_t* pid_list;  // list of dead agent pid's
  pid_t n;          // the next pid that has died
  int status;       // status returned by waitpit()

  /* initialize memory */
  pid_list = (pid_t*)calloc(num_agents() + 1, sizeof(pid_t));
  idx = 0;

  /* get all of the dead children's pids */
  while((n = waitpid(-1, &status, WNOHANG)) > 0)
  {
    VERBOSE2("SIGNALS: received sigchld for pid %d\n", n);
    pid_list[idx++] = n;
  }

  event_signal(agent_death_event, pid_list);
}

/**
 * TODO
 *
 * @param signo
 * @param INFO
 * @param context
 */
void prnt_sig(int signo)
{
  switch(signo)
  {
    case SIGALRM:
      lprintf("SIGNALS: Scheduler received alarm signal, checking job states\n");
      event_signal(agent_update_event, NULL);
      event_signal(database_update_event, NULL);
      alarm(CHECK_TIME);
      break;
    case SIGTERM:
      lprintf("SIGNALS: Scheduler received terminate signal, shutting down scheduler\n");
      event_signal(scheduler_close_event, NULL);
      break;
    case SIGQUIT:
      lprintf("SIGNALS: Scheduler received quit signal, shutting down scheduler\n");
      event_signal(scheduler_close_event, NULL);
      break;
    case SIGINT:
      lprintf("SIGNALS: Scheduler received interrupt signal, shutting down scheduler\n");
      event_signal(scheduler_close_event, NULL);
      break;
  }
}

/* ************************************************************************** */
/* **** The actual scheduler ************************************************ */
/* ************************************************************************** */

/**
 * The heart of the scheduler, the actual scheduling algorithm. This will be
 * passed to the event loop as a call back and will be called every time an event
 * is executed. Therefore the code should be light weight since it will be run
 * very frequently.
 *
 * TODO:
 *   currently this will only grab a job and create a single agent to execute
 *   the job.
 *
 *   TODO: allow for runonpfile jobs to have multiple agents based on size
 *   TODO: allow for job preemption. The scheduler can pause jobs, allow it
 *   TODO: allow for specific hosts to be chossen.
 */
void update_scheduler()
{
  job j;
  host h;

  if(closing && num_agents() == 0 && active_jobs() == 0)
  {
    event_loop_terminate();
    return;
  }

  while((j = next_job()) != NULL)
  {
    if((h = get_host(1)) == NULL)
      continue;

    agent_init(h, j);
  }
}

/* ************************************************************************** */
/* **** main utility functions ********************************************** */
/* ************************************************************************** */

/**
 * TODO
 *
 * @return
 */
int unlock_scheduler()
{
  return shm_unlink(process_name);
}

/**
 * TODO
 *
 * @return
 */
pid_t get_locked_pid()
{
  pid_t pid = 0;
  ssize_t bytes;
  int handle, rc;
  char buf[10];

  /* Initialize memory */
  handle = rc = 0;
  memset(buf, '\0', sizeof(buf));

  /* open the shared memory */
  if((handle = shm_open(process_name, O_RDONLY, 0444)) < 0)
  {
    if(errno != ENOENT)
      ERROR("failed to acquire shared memory", process_name);
    return 0;
  }

  /* find out who owns the shared memory */
  bytes = read(handle, buf, sizeof(buf));
  if((pid = atoi(buf)) < 2)
  {
    if(shm_unlink(process_name) == -1)
      ERROR("failed to remove invalid lock");
    return 0;
  }

  /* check to see if the pid is a valid process */
  if(kill(pid, 0) == 0)
    return pid;

  /* process that created lock is dead, create new lock */
  VERBOSE2("LOCK: PID[%d] is stale. Attempt to unlock.\n")
  if(unlock_scheduler())
    ERROR("LOCK: PID[%d] is stale but unlock failed");

  return 0;
}

/**
 * TODO
 *
 * @return
 */
pid_t lock_scheduler()
{
  pid_t pid;
  int handle;
  char buf[10];

  /* return if lock already exists */
  if((pid = get_locked_pid()))
    return pid;

  /* no lock, create a new lock file */
  if((handle = shm_open(process_name, O_RDWR|O_CREAT|O_EXCL, 0744)) == -1)
  {
    ERROR("failed to open shared memory");
    return -1;
  }

  sprintf(buf, "%-9.9d", getpid());
  if(write(handle, buf, sizeof(buf)) < 1)
  {
    ERROR("failed to write pid to lock file");
    return -1;
  }

  return 0;
}

/**
 * Correctly set the project user and group. The fossology scheduler must run as
 * the user specified by PROJECT_USER and PROJECT_GROUP since the agents must be
 * able to connect to the database. This ensures that that happens correctly.
 */
void set_usr_grp()
{
  /* make sure group exists */
  grp = getgrnam(PROJECT_GROUP);
  if(!grp)
  {
    // TODO error message
  }

  /* set the project group */
  setgroups(1, &(grp->gr_gid));
  if((setgid(grp->gr_gid) != 0) || (setegid(grp->gr_gid) != 0))
  {
    fprintf(stderr, "FATAL %s.%d: %s must be run as root or %s\n", __FILE__, __LINE__, process_name, PROJECT_USER);
    fprintf(stderr, "FATAL Set group '%s' aborting due to error: %s\n", PROJECT_GROUP, strerror(errno));
    exit(-1);
  }

  /* run as project user */
  pwd = getpwnam(PROJECT_USER);
  if(!pwd)
  {
    fprintf(stderr, "FATAL %s.%d: user '%s' not found\n", __FILE__, __LINE__, PROJECT_USER);
    exit(-1);
  }

  /* run as correct user, not as root or any other user */
  if((setuid(pwd->pw_uid) != 0) || (seteuid(pwd->pw_uid) != 0))
  {
    fprintf(stderr, "FATAL %s.%d: %s must run this as %s\n", __FILE__, __LINE__, process_name, PROJECT_USER);
    fprintf(stderr, "FATAL SETUID aborting due to error: %s\n", strerror(errno));
    exit(-1);
  }
}

/**
 * TODO
 */
void kill_scheduler()
{
  pid_t pid;

  if((pid = get_locked_pid()))
  {
    if(kill(pid, SIGQUIT) == -1)
    {
      ERROR("Unable to send SIGQUIT to PID %d", pid);
      return;
    }
    else
    {
      fprintf(stderr, "Exiting %s PID %d\n", process_name, pid);
      lprintf(        "Exiting %s PID %d\n", process_name, pid);
    }

    unlock_scheduler();
  }
}

/**
 * TODO
 */
void load_config()
{
  DIR* dp;                  // directory pointer used to load meta agents;
  struct dirent* ep;        // information about directory
  FILE* istr;               // file pointer to agent.conf
  char* tmp;                // pointer into a string
  char buffer[2048];        // standard string buffer
  char name[MAX_NAME + 1];  // buffer to hold the host and agent names
  char cmd [MAX_CMD  + 1];  // buffer to hold the cmd associated with an agent
  int max = -1;             // the number of agents to a host or number of one type running
  int special = 0;          // anything that is special about the agent (EXCLUSIVE)

  // TODO set this up with DEFAULT_SETUP instead of this
  if((dp = opendir("./agents/")) == NULL)
  {
    FATAL("Could not opend agent.conf directory");
  }

  /* clear all previous configurations */
  agent_list_clean();
  host_list_clean();

  /* load the scheduler configuration */
  istr = fopen("Scheduler.conf", "r"); //< change file path
  while(fgets(buffer, sizeof(buffer) - 1, istr) != NULL)
  {
    /* skip comments and blank lines */
    if(buffer[0] == '#');
    else if(buffer[0] == '\0');
    /* check the port that the interface wil use */
    else if(strncmp(buffer, "port=", 5) == 0 && !is_port_set())
      set_port(atoi(&buffer[5]));
    /* check for the list of available hosts */
    else if(strncmp(buffer, "hosts:", 6) == 0)
    {
      while(fscanf(istr, "%s %s %s %d", name, cmd, buffer, &max) == 4)
      {
        if(strcmp(cmd, "localhost") == 0)
          strcpy(buffer, AGENT_DIR);

        host_init(name, cmd, buffer, max);

        VERBOSE2("CONFIG: added new host\n   name      = %s\n   address   = %s\n   directory = %s\n   max       = %d\n",
            name, cmd, buffer, max);
      }
    }
  }
  fclose(istr);

  /* load the configureation for the agents */
  while((ep = readdir(dp)) != NULL)
  {
    sprintf(buffer, "agents/%s", ep->d_name);
    if(ep->d_name[0] != '.' && (istr = fopen(buffer, "rb")) != NULL)
    {
      VERBOSE2("CONFIG: loading config file %s\n", buffer);

      /* initialize data */
      memset(buffer, '\0', sizeof(buffer));
      memset(name,   '\0', sizeof(name));
      memset(cmd,    '\0', sizeof(cmd));

      /* this is not actually a loop this is used */
      /* so that all error cases still cause the  */
      /* file to close                            */
      while(fgets(buffer, sizeof(buffer) - 1, istr) != NULL)
      {
        if(buffer[0] == '#');
        else if(strncmp(buffer, "name=", 5) == 0)
          strncpy(name, buffer + 5, strlen(buffer + 5) - 1);
        else if(strncmp(buffer, "command=", 8) == 0)
          strncpy(cmd, buffer + 8, strlen(buffer + 8) - 1);
        else if(strncmp(buffer, "max=", 4) == 0)
          max = atoi(buffer + 4);
        else if(strncmp(buffer, "special=", 6) == 0)
        {
          tmp = strtok(buffer, "=|") + 1;
          special = 0;

          while((tmp = strtok(NULL, "=|")) != NULL)
          {
            if(strncmp(tmp, "EXCLUSIVE", 9) == 0)
              special |= SAG_EXCLUSIVE;
            else if(strncmp(tmp, "NONE", 4) == 0)
            {
              special = SAG_NONE;
              break;
            }
          }
        }
        else
        {
          // TODO handle error case
        }
      }

      if(!add_meta_agent(name, cmd, max, special))
      {
        VERBOSE2("CONFIG: could not create meta agent using %s\n", ep->d_name);
      }
      else if(TVERBOSE2)
      {
        lprintf("CONFIG: added new meta agent\n");
        lprintf("   name    = %s\n   command = %s\n   max     = %d\n   special = %d\n",
            name, cmd, max, special);
      }

      fclose(istr);
    }
  }
  closedir(dp);

  for_each_host(test_agents);
}

/**
 * TODO
 *
 * @param unused
 */
void scheduler_close_event(void* unused)
{
  closing = 1;
  kill_agents();
}

/**
 * TODO
 *
 * @return
 */
int close_scheduler()
{
  job_list_clean();
  host_list_clean();
  agent_list_clean();
  interface_destroy();
  database_destroy();
  event_loop_destroy();
  return 0;
}

/**
 * Utility function that enables the use of the strcmp function with a GTree.
 *
 * @param a The first string
 * @param b The second string
 * @param user_data unused in this function
 * @return integral value idicating the relatioship between the two strings
 */
gint string_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
  return strcmp((char*)a, (char*)b);
}

/**
 * Utility function that enable the agents to be stored in a GTree using
 * the PID of the associated process.
 *
 * @param a The pid of the first process
 * @param b The pid of the second process
 * @param user_data unused in this function
 * @return integral value idicating the relationship between the two pids
 */
gint int_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
  return *(int*)a - *(int*)b;
}

/* ************************************************************************** */
/* **** main types ********************************************************** */
/* ************************************************************************** */

/**
 * main function for FOSSology scheduler, does command line parsing,
 * Initialization and then simply enters the event loop.
 *
 * @param argc the command line argument cound
 * @param argv the command line argument values
 * @return if the program ran correctly
 */
int main(int argc, char** argv)
{
  /* locals */
  gboolean db_reset = FALSE;  // flag to reset the job queue upon database connection
  gboolean ki_sched = FALSE;  // flag that indicates that the scheduler will be killed after start
  gboolean db_init  = FALSE;  // flag indicating a database test
  gboolean s_daemon = FALSE;  // flag indicating if the scheduler should run as a daemon
  gboolean test_die = FALSE;  // flag to run the tests then die
  int port = -1;              // the port the scheduler will listen on
  char* log = NULL;           // used when a different log from the default is used
  GOptionContext* options;    // option context used for command line parsing
  int rc;                     // used for return values of

  /* the options for the command line parser */
  GOptionEntry entries[] =
  {
      { "daemon",   'd', 0, G_OPTION_ARG_NONE,   &s_daemon, "Run scheduler as daemon"                     },
      { "database", 'i', 0, G_OPTION_ARG_NONE,   &db_init,  "Initialize database connection and exit"     },
      { "kill",     'k', 0, G_OPTION_ARG_NONE,   &ki_sched, "Kills all running schedulers and exit"       },
      { "log",      'L', 0, G_OPTION_ARG_STRING, &log,      "Prints log here instead of default log file" },
      { "port",     'p', 0, G_OPTION_ARG_INT,    &port,     "Set the port the interface listens on"       },
      { "reset",    'R', 0, G_OPTION_ARG_NONE,   &db_reset, "Reset the job queue upon startup"            },
      { "test",     't', 0, G_OPTION_ARG_NONE,   &test_die, "Close the scheduler after running tests"     },
      { "verbose",  'v', 0, G_OPTION_ARG_INT,    &verbose,  "Set the scheudler verbose level"             },
      {NULL}
  };

  /* make sure we are running as fossy */
  set_usr_grp();

  /* ********************* */
  /* *** parse options *** */
  /* ********************* */
  options = g_option_context_new("- scheduler for FOSSology");
  g_option_context_add_main_entries(options, entries, NULL);
  g_option_context_set_ignore_unknown_options(options, TRUE);
  g_option_context_parse(options, &argc, &argv, NULL);
  g_option_context_free(options);

  if(s_daemon) { rc = daemon(0, 0); }
  if(db_init) { database_init(); return 0; }
  if(ki_sched) { kill_scheduler(); return 0; }
  if(port >= 0) {set_port(port); }
  if(log != NULL) {set_log(log); }

  if(lock_scheduler() <= 0 && !get_locked_pid())
    FATAL("scheduler lock error");

  /* ********************************** */
  /* *** do all the initializations *** */
  /* ********************************** */
  g_thread_init(NULL);
  g_type_init();
  load_config();
  interface_init();
  database_init();

  signal(SIGCHLD, chld_sig);
  signal(SIGALRM, prnt_sig);
  signal(SIGTERM, prnt_sig);
  signal(SIGQUIT, prnt_sig);
  signal(SIGINT,  prnt_sig);

  /* *********************************** */
  /* *** post initialization checks **** */
  /* *********************************** */
  if(db_reset)
    database_reset_queue();
  if(test_die)
    closing = 1;

  /* *************************************** */
  /* *** enter the scheduler event loop **** */
  /* *************************************** */
  alarm(CHECK_TIME);
  event_loop_enter(update_scheduler);

  return close_scheduler();
}
