#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>

void syscall_init (void);
void destroy_fd_table (void);

struct thread;
bool duplicate_fd_table (struct thread *parent);

#endif /* userprog/syscall.h */
