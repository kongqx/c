#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>


#include "boolean.h"
#include "socket.h"
#include "timer.h"

#include "forker_messages.h"

/* For error messages */
#define PROG_NAME "FORKER"

/* Exit code when something is wrong */
#define FATAL_ERROR_EXIT_CODE 1

/* Rights of redirections: -rw-r--r-- 0644 */
#define FILE_RIGHTS  (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)

/* Running command */
typedef struct com_cell {
  command_number  number;
  int             pid;
  soc_host        client_host;
  soc_port        client_port;
  struct com_cell *next, *prev;
} command_cell;
command_cell *first_cell, *last_cell;

/* Fd to write on when child exited */
static int write_on_me;

/* Debug flag */
#ifdef DEBUG
static int debug=TRUE;
#else
static int debug=FALSE;
#endif
#define FORKER_DEBUG "FORKER_DEBUG"

/* Display error and fataf error message */
void fatal (char *msg, char *extra) {
  fprintf(stderr, "%s FATAL: %s%s.\n", PROG_NAME, msg, extra);
}
void error (char *msg, char *extra) {
  fprintf(stderr, "%s ERROR: %s%s.\n", PROG_NAME, msg, extra);
}

/* Toggle debug on SIGUSR1 */
void sigusr1_handler (int signum) {
  if (signum != SIGUSR1) {
    error("Handler called but not on sig usr1", "");
    return;
  }
  debug = !debug;
  printf("Caught sigusr1, debug %s.\n", (debug ? "on" : "off"));
}

/* Write a byte on pipe when a child exits */
void sigchild_handler (int signum) {

  int res;
  char c = 'C';

  if (signum != SIGCHLD) {
    error("Handler called but not on sig child", "");
    return;
  }

  if (debug) {
    printf("Caught sigchild\n");
  }
  
  /* Write on pipe */
  for (;;) {
    res = write(write_on_me, &c, 1);
    if ( (res > 0) || ( (res == -1) && (errno != EINTR) ) ) {
      break;
    }
  }
  if (debug) {
    printf("Pipe written\n");
  }
}

/* Check there is one terminator ('\0') in string */
boolean has_1_nul (char *str, int len) {
  int i;
  for (i = 0; i < len; i++) {
    if (str[i] == '\0') {
      return TRUE;
    }
  }
  return FALSE;
} 

/* Check there are two successive terminators ("\0\0") in string */
boolean has_2_nuls (char *str, int len) {
  int i;
  char p;

  p = ' ';
  for (i = 0; i < len; i++) {
    if ( (str[i] == '\0') && (p == '\0') ) {
      return TRUE;
    }
    p = str[i];
  }
  return FALSE;
} 

/* Fork and launch a command, return forked pid or -1 */
int do_start_command (start_request_t *request,
                          soc_host *client_host,
                          soc_port client_port) {
  int res;
  command_cell *cur_cell;
  int fd;
  char *start;
  int nargs;
  char **args;

  /* Check there is a command */
  if (request->command_text[0] == '\0') {
    error("Empty command in start message", "");
    return -1;
  }

  /* Check all strings are propoerly terminated */
  if (! has_2_nuls(request->command_text,
                   sizeof(request->command_text)) ) {
    error("Invalid text in start command", "");
    return -1;
  }
  if (! has_2_nuls(request->environ_variables,
                   sizeof(request->environ_variables)) ) {
    error("Invalid text in start environ", "");
    return -1;
  }
  if (! has_1_nul(request->currend_dir,
                  sizeof(request->currend_dir)) ) {
    error("Invalid text in start curdir", "");
    return -1;
  }
  if (! has_1_nul(request->output_flow,
                  sizeof(request->output_flow)) ) {
    error("Invalid text in start stdout", "");
    return -1;
  }
  if (! has_1_nul(request->error_flow,
                  sizeof(request->error_flow)) ) {
    error("Invalid text in start stderr", "");
    return -1;
  }

  /* Procreate */
  res = fork();
  if (res == -1) {
    perror("fork");
    error("Cannot fork", "");
    return -1;
  } else if (res != 0) {
    /***************/
    /* Forker code */
    /***************/
    cur_cell = malloc(sizeof(command_cell));
    if (cur_cell == NULL) {
      perror("malloc");
      error("Cannot malloc a new cell", "");
      return -1;
    }
    /* Allocate a cell and store command and pid */
    cur_cell->pid = res;
    cur_cell->number = request->number;
    memcpy (&(cur_cell->client_host), client_host, sizeof(soc_host));
    cur_cell->client_port = client_port;
    cur_cell->next = first_cell;
    cur_cell->prev = NULL;
    first_cell = cur_cell;
    if (cur_cell->next != NULL) {
      (cur_cell->next)->prev = cur_cell;
    } else {
      last_cell = cur_cell;
    }
    if (debug) {
      printf ("Forked pid %d\n", res);
    }
    /* Done */
    return res;
  }

  /**************/
  /* Child code */
  /**************/
  /* Set current directory */
  if (request->currend_dir[0] != '\0') {
    if (chdir(request->currend_dir) != 0) {
      perror("chdir");
      error("Cannot change to current directory: ", request->currend_dir);
      exit(FATAL_ERROR_EXIT_CODE);
    }
    if (debug) {
      printf ("Curdir changed to >%s<\n", request->currend_dir);
    }

  }

  /* Set environnement variables */ 
  start = request->environ_variables;
  while (*start != '\0') {
    if(putenv(start) != 0) {
      perror("putenv");
      error("Cannot set environement variable: ", start);
      exit(FATAL_ERROR_EXIT_CODE);
    }
    if (debug) {
      printf ("Envir added: >%s<\n", start);
    }
    /* Find end of string */
    while (*start != '\0') start++;
    /* Start of next env */
    start++;
  }

  /* Output flow */
  if (request->output_flow[0] != '\0') {
    fd = open (request->output_flow,
                O_CREAT | O_WRONLY | (request->append_output ? O_APPEND : O_TRUNC),
                FILE_RIGHTS);
    if (fd == -1) {
      perror("open");
      error("Cannot open output flow: ", request->output_flow);
      exit(FATAL_ERROR_EXIT_CODE);
    }
    if (dup2(fd, fileno(stdout)) == -1) {
      perror("dup2");
      error("Cannot duplicate output flow", "");
      exit(FATAL_ERROR_EXIT_CODE);
    }
    close(fd);
    if (debug) {
      printf ("Stdout set to >%s< append %d\n", request->output_flow,
              (int)request->append_output);
    }
  }

  /* Error flow */
  if (request->error_flow[0] != '\0') {
    fd = open (request->error_flow,
                O_CREAT | O_WRONLY | (request->append_error ? O_APPEND : O_TRUNC),
                FILE_RIGHTS);
    if (fd == -1) {
      perror("open");
      error("Cannot open error flow: ", request->error_flow);
      exit(FATAL_ERROR_EXIT_CODE);
    }
    if (dup2(fd, fileno(stderr)) == -1) {
      perror("dup2");
      error("Cannot duplicate error flow", "");
      exit(FATAL_ERROR_EXIT_CODE);
    }
    close(fd);
    if (debug) {
      printf ("Stderr set to >%s< append %d\n", request->error_flow,
              (int)request->append_error);
    }
  }

  /* Count program name + arguments */
  nargs = 0;
  start = request->command_text;
  while (*start != '\0') {
    nargs++;
    while (*start != '\0') start++;
    start++;
  }

  /* Need one slot more for NULL */
  nargs++;

  /* Allocate table */
  args = malloc(nargs * sizeof(char*));
  if (args == NULL) {
    perror("malloc");
    error("Cannot malloc array of arguments", "");
    exit(FATAL_ERROR_EXIT_CODE);
  }

  /* Fill table[0] <- program name */
  args[0] = request->command_text;
  start = request->command_text;
  while (*start != '\0') start++;
  start++;
  nargs = 1;

  /* Fill table with arguments */
  while (*start != '\0') {
    args[nargs] = start;
    while (*start != '\0') start++;
    start++;
    nargs++;
  }
  args[nargs] = NULL;

  if (debug) {
    int li = 0;
    printf ("Command line: >%s<", request->command_text);
    while (args[li] != NULL) {
      printf (" >%s<", args[li]);
      li++;
    }
    printf ("\n\n");
  }

  /* Now the exec */    
  if (execv(request->command_text, args) == -1) {
    perror("execv");
    error("Cannot exec command: ", request->command_text);
    exit(FATAL_ERROR_EXIT_CODE);
  } 
  /* Never reached */
}


/* Check message kind and size are correct and match */
/* len has to be size of request_u or size of current request_t */
boolean msg_ok (soc_length len, request_kind_list kind) {
  switch (kind) {
    case start_command:
      if (len == sizeof(start_request_t)) return TRUE;
    break;
    case kill_command:
      if (len == sizeof(kill_request_t)) return TRUE;
    break;
    case fexit_command:
      if (len == sizeof(fexit_request_t)) return TRUE;
    break;
    case ping_command:
      if (len == 0) return TRUE;
    break;
    default:
      error("Received a message with invalid command", "");
      return FALSE;
    break;
  }
  if (len == sizeof(request_u)) {
    return TRUE;
  } else {
    error("Received a message of invalid size", "");
    return FALSE;
  }
}

int main (int argc, char *argv[]) {

  char *debug_var;
  soc_token soc = NULL;
  int port_no;
  int nfds;
  fd_set saved_mask, select_mask;
  int soc_fd;
  int pipe_fd[2];
  int res;
  char c;
  pid_t a_pid;
  command_cell *cur_cell;
  report_message_t report;

  /* Init debug */
  debug_var = getenv(FORKER_DEBUG);
  if ( (debug_var != NULL)
    && (strcmp(debug_var, "Y") == 0) ) {
    debug = TRUE;
  } else if ( (debug_var != NULL)
            && (strcmp(debug_var, "N") == 0) ) {
    debug = FALSE;
  } /* Else unchanged from compilation option */

  /* Create the socket */
  if (soc_open(&soc, udp_socket) != SOC_OK) {
    perror("opening socket");
    fatal("Cannot create socket", "");
    exit(FATAL_ERROR_EXIT_CODE);
  }

  /* Check and parse the argument: port num or port no */
  if (argc != 2) {
    fatal("Invalid argument. Port name/num expected.", "");
    exit(FATAL_ERROR_EXIT_CODE);
  }

  /* Bind */
  port_no = atoi(argv[1]);
  if (port_no <= 0) {
    if (soc_link_service(soc, argv[1]) != SOC_OK) {
      perror ("linking socket");
      fatal("Cannot bind socket to name: ", argv[1]);
      exit (FATAL_ERROR_EXIT_CODE);
    }
  } else {
    if (soc_link_port(soc, port_no) != SOC_OK) {
      perror ("linking socket");
      fatal("Cannot bind socket to no: ", argv[1]);
      exit (FATAL_ERROR_EXIT_CODE);
    }
  }

  /* Get socket fd  for the select */
  if (soc_get_id(soc, &soc_fd) != SOC_OK) {
    perror ("getting socket fd");
    fatal("Cannot get socket fd", "");
    exit (FATAL_ERROR_EXIT_CODE);
  }

  /* Create pipe and save writting fd for sigchild handler */
  if (pipe(pipe_fd) == -1) {
    perror("pipe");
    fatal("Cannot create pipe", "");
    exit (FATAL_ERROR_EXIT_CODE);
  }
  write_on_me = pipe_fd[1];

  /* Close on exec on pipe fds */
  if (fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC) < 0) {
    perror("fcntl(FD_CLOEXEC)");
    fatal("Cannot set close on exec on pipe_fd[0]", "");
    exit (FATAL_ERROR_EXIT_CODE);
  }
  if (fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC) < 0) {
    perror("fcntl(FD_CLOEXEC)");
    fatal("Cannot set close on exec on pipe_fd[1]", "");
    exit (FATAL_ERROR_EXIT_CODE);
  }


  /* Build select mask */
  nfds = 0;
  FD_ZERO (&saved_mask);
  FD_SET(soc_fd, &saved_mask);
  if (soc_fd > nfds) nfds = soc_fd;
  FD_SET(pipe_fd[0], &saved_mask);
  if (pipe_fd[0] > nfds) nfds = pipe_fd[0];

  /* Hook handler for child signal */
  if (signal (SIGCHLD, sigchild_handler) == SIG_ERR) {
    perror("signal");
    fatal("Cannot hook signal child handler", "");
    exit (FATAL_ERROR_EXIT_CODE);
  }

  /* Hook handler for usr1 signal */
  if (signal (SIGUSR1, sigusr1_handler) == SIG_ERR) {
    perror("signal");
    fatal("Cannot hook signal usr1 handler", "");
    exit (FATAL_ERROR_EXIT_CODE);
  }

  /* Init list of running commands */
  first_cell = NULL;
  last_cell = NULL;

  /* Clear environ */
  if (clearenv() != 0) {
    perror("clearenv");
    fatal("Cannot clear environment", "");
    exit (FATAL_ERROR_EXIT_CODE);
  }
 

  /* Ready */
  fprintf(stderr, "%s ready. %s\n", PROG_NAME, (debug ? "Debug on." : ""));

  /* Main loop */
  for (;;) {

    /* Select loop while EINTR */
    for (;;) {
      memcpy(&select_mask, &saved_mask, sizeof(fd_set));
      res = select(nfds+1, &select_mask, (fd_set*)NULL, (fd_set*)NULL,
                   (struct timeval*)NULL);
      if ( (res > 0) || ( (res == -1) && (errno != EINTR) ) ) {
        break;
      }
    }
    if (res < 0) {
      perror("select");
      continue;
    }

    /* Where data to read? */
    if (FD_ISSET(pipe_fd[0], &select_mask)) {

      /* Data on pipe, read the byte */
      if (debug) {
        printf ("Data on pipe\n");
      }
      for (;;) {
        res = read(pipe_fd[0], &c, 1);
        if ( (res > 0) || ( (res == -1) && (errno != EINTR) ) ) {
          break;
        }
      }
      if (res < 0) {
        perror("read");
        continue;
      }
      if (debug) {
        printf ("Data read on pipe\n");
      }

      /* Look for several exited children */
      report.kind = exit_report;
      for (;;) {
        boolean do_send;

        /* Get a dead pid */
        for (;;) {
          res = waitpid((pid_t)-1, &(report.exit_rep.exit_status), WNOHANG);
          if ( (res >= 0) || ( (res == -1) && (errno != EINTR) ) ) {
            break;
          }
        }
        if ((res == 0) || ( (res == -1) && (errno == ECHILD) ) ) {
          /* No more child */
          if (debug) {
            printf ("No more child\n\n");
          }
          break;
        } else if (res < 0) {
          perror("waitpid");
          continue;
        }

        /* Child pid */
        a_pid = res;
        if (debug) {
          printf ("Pid %d exited code %d\n", a_pid, report.exit_rep.exit_status);
        }

        /* Look for pid in list */
        cur_cell = last_cell;
        while ( (cur_cell != NULL) && (cur_cell->pid != a_pid) ) {
          cur_cell = cur_cell->prev;
        }
        if (cur_cell == NULL) {
          char dbg[50];
          sprintf(dbg, "%d", a_pid);
          error("Cannot find in list pid: ", dbg);
          continue;
        }

        /* Found the command number */
        report.exit_rep.number = cur_cell->number;
        report.exit_rep.exit_pid = cur_cell->pid;
        if (debug) {
          printf ("Command was %d\n", report.exit_rep.number);
        }

        /* Set dest to client for report */
        do_send = TRUE;
        if (soc_set_dest(soc, &(cur_cell->client_host), cur_cell->client_port)
             != SOC_OK) {
          error("Cannot set dest to client", "");
          do_send = FALSE;
        }
        if (debug) {
          printf ("Dest set to host %u port %u\n",
                  cur_cell->client_host.integer,
                  cur_cell->client_port);
        }

        /* Free the cell */
        if (cur_cell->next != NULL) {
          (cur_cell->next)->prev = cur_cell->prev;
        } else {
          last_cell = cur_cell->prev;
        }
        if (cur_cell->prev != NULL) {
          (cur_cell->prev)->next = cur_cell->next;
        } else {
          first_cell = cur_cell->next;
        }
        free(cur_cell);

        /* Send report */
        if (do_send) {
          if (soc_send(soc, (soc_message)&report,
                             sizeof(report)) != SOC_OK) {
            perror("sending on socket");
            error("Cannot send exit report message", "");
          }
          if (debug) {
            printf ("Exit report sent\n");
          }
        }

      } /* For each dead child */

    } else if (FD_ISSET(soc_fd, &select_mask)) {
      /* A request */
      request_message_t request_message;
      soc_length        request_len;
      soc_host          request_host;
      soc_port          request_port;

      if (debug) {
        printf ("Data on socket\n");
      }

      /* Read request */
      request_len = soc_receive(soc, &request_message,
                                sizeof(request_message), TRUE);
      if (request_len < SOC_OK) {
        perror("receiving from socket");
        error("Cannot receive request message", "");
        continue;
      }

      /* Check message */
      if (request_len < sizeof(request_message.kind)) {
        error("Received a message of invalid size", "");
        continue;
      }
      if (! msg_ok (request_len - sizeof(request_message.kind), request_message.kind) ) {
        continue;
      }


      /* Get client host and port */
      if (soc_get_dest_host(soc, &request_host) != SOC_OK) {
        error("Cannot get client host", "");
        continue;
      }
      if (soc_get_dest_port(soc, &request_port) != SOC_OK) {
        error("Cannot get client port", "");
        continue;
      }
      if (debug) {
        printf ("Client is host %u port %u\n",
                request_host.integer,
                request_port);
      }

      if (request_message.kind == kill_command) {

        report.kind = kill_report;
        report.kill_rep.number = request_message.kill_req.number;
        if (debug) {
          printf ("Request kill num %d sig %d\n",
                request_message.kill_req.number,
                request_message.kill_req.signal_number);
        }

        /* Kill command: Find pid from criteria Num, host, port */
        report.kill_rep.killed_pid = -1;
        cur_cell = last_cell;
        while (cur_cell != NULL) {
          if ( (cur_cell->number == request_message.kill_req.number)
            && (cur_cell->client_port == request_port)
            && (memcmp(&(cur_cell->client_host),
                       &request_host,
                       sizeof(soc_host)) == 0) ) {
              break;
          }
          cur_cell = cur_cell->prev;
        }
        if (cur_cell == NULL) {
          if (debug) {
            printf("Cannot find command-host-port in list\n");
          }
          report.kill_rep.killed_pid = -1;
        } else {
          report.kill_rep.killed_pid = cur_cell->pid;
          if (debug) {
            printf ("Pid to kill is %d\n", cur_cell->pid);
          }
        }

        /* Kill the child */
        if (report.kill_rep.killed_pid != -1) {
          if (kill(cur_cell->pid, request_message.kill_req.signal_number) == -1) {
            char dbg[50];
            perror("kill");
            sprintf(dbg, "%d", cur_cell->pid);
            error("Cannot kill child pid: ", dbg);
            report.kill_rep.killed_pid = -1;
          }
        }
      } else if (request_message.kind == start_command) {

        /* Start command */
        if (debug) {
          printf ("Request start %d\n", request_message.start_req.number);
        }
        report.kind = start_report;
        report.start_rep.number = request_message.start_req.number;
        report.start_rep.started_pid =
               do_start_command(&request_message.start_req,
                                &request_host, request_port);

      } else if (request_message.kind == fexit_command) {

        /* Exit command */
        if (debug) {
          printf ("Request exit %d\n", request_message.fexit_req.exit_code);
        }
        report.kind = fexit_report;

      } else if (request_message.kind == ping_command) {

        /* Ping command */
        if (debug) {
          printf ("Request ping\n");
        }
        report.kind = pong_report;

      } else {

        error("Received a message with invalid command", "");

      }

      /* Send report */
      if (soc_send(soc, (soc_message)&report,
                         sizeof(report)) != SOC_OK) {
        perror("sending on socket");
        error("Cannot send exit report message", "");
      }
      if (debug) {
        printf ("Start/Kill/Exit/Pong report sent\n\n");
      }

      if (request_message.kind == fexit_command) {
        if (debug) {
          printf ("Exiting code %d\n", request_message.fexit_req.exit_code);
        }
        exit(request_message.fexit_req.exit_code);
      }
        

    } else {
      error("Select set unknown fd", "");
    }
      
  } /* Main loop */
  
}
