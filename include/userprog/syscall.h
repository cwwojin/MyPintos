#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

void exit(int status);
void check_address(void* addr);
bool create(const char *file, unsigned initial_size);
bool remove(const char* file);

#endif /* userprog/syscall.h */
