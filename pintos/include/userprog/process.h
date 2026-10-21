#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);   /* 첫 프로세스 */
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);                       /* 성공하면 안 돌아옴 */
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

#endif /* userprog/process.h */