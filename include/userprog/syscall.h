#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

#include "threads/synch.h"
/* Filesys Lock */
struct lock filesys_lock;

void exit(int status);
void check_address(void* addr);

#endif /* userprog/syscall.h */
