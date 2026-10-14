#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/* 카운팅 세마포어. */
struct semaphore {
	unsigned value;             /* 현재 값. */
	struct list waiters;        /* 기다리는 스레드 목록 (스레드의 elem으로 연결). */
};

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

/* Lock. 초깃값이 1인 세마포어에 주인(holder)을 붙인 것. */
struct lock {
	struct thread *holder;      /* lock을 들고 있는 스레드. donation을 받을 대상. */
	struct semaphore semaphore; /* 접근을 제어하는 이진 세마포어. 기다리는 스레드는 이 waiters에 있다. */
	struct list_elem elem;			/* holder의 locks 리스트에 들어갈 때 쓰는 elem. */
};

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/* 조건 변수. */
struct condition {
	struct list waiters;        /* 기다리는 waiter 목록 (struct semaphore_elem의 elem으로 연결). */
};

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/* 최적화 배리어(optimization barrier).
 
   컴파일러는 이 배리어를 넘어서 연산 순서를 바꾸지 않는다.
   자세한 내용은 참고 문서의 "Optimization Barriers"를 보라. */
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */
