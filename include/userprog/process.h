#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"


tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);


/* file descriptor struct */
struct fd{
  int fd_num;
  struct file* file;
  struct list_elem elem;
};

/* process control block for WAITs. */
struct pcb{
  /* Info needed for waits : "Did the process exit?", "Exit status?", "Whats the tid?" */
  struct thread* thread;
  tid_t tid;
  bool exited;
  int exit_status;
  struct list_elem elem;
};

int process_add_file(struct file* file);
struct file* process_get_file(int fd);
void process_close_file(int fd);


#endif /* userprog/process.h */
