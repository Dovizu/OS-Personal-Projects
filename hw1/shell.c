#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>

#define FALSE 0
#define TRUE 1
#define INPUT_STRING_SIZE 80

#include "io.h"
#include "parse.h"
#include "process.h"
#include "shell.h"

int asprintf(char **strp, const char *fmt, ...);

int cmd_quit(tok_t arg[]) {
  printf("Bye\n");
  exit(0);
  return 1;
}

int cmd_help(tok_t arg[]);

int cmd_change_dir(tok_t arg[]);

/* Command Lookup table */
typedef int cmd_fun_t (tok_t args[]); /* cmd functions take token array and return int */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_quit, "quit", "quit the command shell"},
  {cmd_change_dir, "cd", "change the current working directory"},
};

int cmd_help(tok_t arg[]) {
  int i;
  for (i=0; i < (sizeof(cmd_table)/sizeof(fun_desc_t)); i++) {
    printf("%s - %s\n",cmd_table[i].cmd, cmd_table[i].doc);
  }
  return 1;
}

int cmd_change_dir(tok_t arg[]) {
  chdir(*arg);
  return 1;
}

int lookup(char cmd[]) {
  int i;
  for (i=0; i < (sizeof(cmd_table)/sizeof(fun_desc_t)); i++) {
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0)) return i;
  }
  return -1;
}

void init_shell()
{
  /* Check if we are running interactively */
  shell_terminal = STDIN_FILENO;

  /** Note that we cannot take control of the terminal if the shell
      is not interactive */
  shell_is_interactive = isatty(shell_terminal);

  if(shell_is_interactive){

    /* force into foreground */
    while(tcgetpgrp (shell_terminal) != (shell_pgid = getpgrp()))
      kill( - shell_pgid, SIGTTIN);

    shell_pgid = getpid();
    /* Put shell in its own process group */
    if(setpgid(shell_pgid, shell_pgid) < 0){
      perror("Couldn't put the shell in its own process group");
      exit(1);
    }

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);
    tcgetattr(shell_terminal, &shell_tmodes);
  }
  /** YOUR CODE HERE */
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);
}

/**
 * Add a process to our process list
 */
 void add_process(process* p)
 {
  /** YOUR CODE HERE */
 }



 char* resolve_path(char *fname) {
  char *ORIGINAL_PATH = getenv("PATH");
  char *PATH;
  asprintf(&PATH, "%s", ORIGINAL_PATH);
  tok_t *t = getToks(PATH);
  int i = 0;
  for (i=0; t[i]; i++) {
    char *full_path;
    asprintf(&full_path, "%s/%s", t[i], fname);
    if (access(full_path, F_OK) != -1) {
      // file exists
      return full_path;
    }
  }
  return NULL;
}

// void setup_io(tok_t *t, tok_t* *cmds, process *proc) {
//   int i;
//   for (i=0; t[i]; ++i) {
//     if (strcmp(t[i], ">") == 0) {
//       tok_t *commands = malloc(sizeof(tok_t)*i);
//       memcpy(commands, t, sizeof(tok_t)*i);
//       *cmds = commands;
//       tok_t outfile = t[i+1];
//       fflush(stdout);
//       FILE *out_desc = fopen(outfile, "w");
//       dup2(fileno(out_desc), STDOUT_FILENO);
//       fclose(out_desc);
//       proc->stdout = fileno(stdout);
//       tcgetattr(fileno(stdout), &(proc->tmodes));
//       return;
//     }
//     if (strcmp(t[i], "<") == 0) {
//       tok_t *commands = malloc(sizeof(tok_t)*i);
//       memcpy(commands, t, sizeof(tok_t)*i);
//       *cmds = commands;
//       tok_t infile = t[i+1];
//       fflush(stdin);
//       int in_desc = open(infile, O_RDONLY);
//       dup2(in_desc, STDIN_FILENO);
//       close(in_desc);
//       proc->stdin = fileno(stdin);
//       tcgetattr(fileno(stdin), &(proc->tmodes));
//       return;
//     }
//   }
//   *cmds = t;
// }
/**
 * Creates a process given the inputString from stdin
 */
 process* create_process(char* inputString) {
  
  // create process on heap
  process *p = malloc(sizeof(process));
  // tokenize command line input
  tok_t *t = getToks(inputString); /* break the line into tokens */
  // figure out argv, argc and io
  int argc;
  bool io_found = false;
  p->argv = t;
  p->stdin = STDIN_FILENO;
  p->stdout = STDOUT_FILENO;
  p->stderr = STDERR_FILENO;
  for (argc=0; t[argc]; ++argc) {
    if (!io_found) {
      // figure out argv
      if (strcmp(t[argc], ">")==0 || strcmp(t[argc], "<")==0) {
        // copy every token before the pipe sign
        p->argv = malloc(sizeof(tok_t)*argc);
        memcpy(p->argv, t, sizeof(tok_t)*argc);
        io_found = true;
      }
      // figure out io
      if (strcmp(t[argc], ">")==0) {
        tok_t outfile = t[argc+1];
        p->stdout = fileno(fopen(outfile, "w"));
      } else if (strcmp(t[argc], "<")==0) {
        tok_t infile = t[argc+1];
        p->stdin = fileno(fopen(infile, "r"));
      }
    }
  }
  // set argument count
  p->argc = argc;
  // resolve file path
  bool access_ok = (access(p->argv[0], F_OK) != -1);
  if (!access_ok) {
    p->argv[0] = resolve_path(p->argv[0]);
    if (!p->argv[0]) {
      return NULL;
    }
  }
  return p;
}

void run_program(tok_t *t, char *s) {
  // execute another program specified in command
  int exit_status;
  process* proc;
  if ((proc = create_process(s)) != NULL) {
    pid_t pid = fork();
  // both parent and child should set put the child into its own process group
    if (pid == 0) {
      proc->pid = getpid();
      launch_process(proc);
    } else {
      proc->pid = pid;
      if (shell_is_interactive) {
        setpgid(proc->pid, proc->pid);
      }
      waitpid(proc->pid, &exit_status, 0);
      tcsetpgrp(shell_terminal, shell_pgid);
      tcgetattr(shell_terminal, &shell_tmodes);
      tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
    // init_shell();
    // if (WIFEXITED(exit_status)) {
    //   proc->status = WEXITSTATUS(exit_status);
    //   if (WIFSTOPPED(exit_status)) {
    //     proc->stopped = 1;
    //   } else {
    //     proc->completed = 1;
    //   }
    // }
    }
    /* Clean up after pipes.  */
    // if (proc->stdin != STDIN_FILENO) close(proc->stdin);
    // if (proc->stdout != STDOUT_FILENO) close(proc->stdout);
    // if (proc->stderr != STDERR_FILENO) close(proc->stderr);
  }
  
}

int shell (int argc, char *argv[]) {
  char *s = malloc(INPUT_STRING_SIZE+1);			/* user input string */
  tok_t *t;			/* tokens parsed from input */
  int lineNum = 0;
  int fundex = -1;
  pid_t pid = getpid();		/* get current processes PID */
  pid_t ppid = getppid();	/* get parents PID */
  // pid_t cpid, tcpid, cpgid;

  init_shell();

  printf("%s running as PID %d under %d\n",argv[0],pid,ppid);

  lineNum=0;
  char *cwd = getcwd(NULL, 0);
  fprintf(stdout, "%d [%s]:", lineNum, cwd);
  free(cwd);
  while ((s = freadln(stdin))){
    char *s_copy;
    asprintf(&s_copy, "%s", s);

    t = getToks(s); /* break the line into tokens */
    fundex = lookup(t[0]); /* Is first token a shell literal */
    if(fundex >= 0) cmd_table[fundex].fun(&t[1]);
    else {
      run_program(t, s_copy);
    }
    ++lineNum;
    cwd = getcwd(NULL, 0);
    fprintf(stdout, "%d [%s]:", lineNum, cwd);
    free(cwd);
  }
  return 0;
}
