/* 이 파일은 Nachos 교육용 운영체제의 소스 코드를 바탕으로 한다.
   아래는 Nachos 저작권 고지이다. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 세마포어 SEMA를 VALUE로 초기화한다.
   세마포어는 음수가 아닌 정수 하나와, 그 값을 다루는
   두 가지 원자적 연산으로 이루어진다.
 
   - down 또는 "P": 값이 양수가 될 때까지 기다린 뒤 1 줄인다.
 
   - up 또는 "V": 값을 1 늘린다 (기다리는 스레드가 있으면 하나를 깨운다). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* 세마포어의 down("P") 연산. SEMA의 값이 양수가 될 때까지 기다린 뒤
   원자적으로 1 줄인다.
 
   잠들 수 있으므로 인터럽트 핸들러 안에서 부르면 안 된다.
   인터럽트를 끈 상태에서 불러도 되지만, 잠들게 되면 다음에 스케줄된
   스레드가 인터럽트를 다시 켤 것이다.
 
   waiters에는 맨 뒤에 넣기만 한다(list_push_back). 기다리는 동안
   donation으로 우선순위가 바뀔 수 있으므로, 고르는 건 sema_up에서
   꺼내는 순간 list_max로 한다.
   깨어나면 while 조건부터 다시 검사한다. 그래서 sema_up은 value를 올린 뒤에
   양보해야 한다. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_push_back (&sema->waiters, &thread_current ()->elem);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* 세마포어의 down("P") 연산. 단, 세마포어가 0이 아닐 때만 한다.
   값을 줄였으면 true, 아니면 false를 돌려준다.
 
   인터럽트 핸들러 안에서 불러도 된다. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* 세마포어의 up("V") 연산. SEMA의 값을 1 늘리고,
   기다리는 스레드가 있으면 하나를 깨운다.
 
   인터럽트 핸들러 안에서 불러도 된다.
 
   Priority Scheduling:
   - waiters에서 우선순위가 가장 높은 스레드를 list_max로 골라 깨운다.
   - value++를 한 다음에 thread_yield_to_higher ()로 양보를 판단한다.
     순서가 반대면, 깨운 스레드가 sema_down의 while 조건에서 value가 0인 걸 보고
     다시 잠들어 버린다. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)) {
		struct list_elem *high_elem = list_max (&sema->waiters, priority_less, NULL);
		list_remove (high_elem);
		thread_unblock (list_entry (high_elem,
					struct thread, elem));
	}
	sema->value++;
	intr_set_level (old_level);
	thread_yield_to_higher ();
}

static void sema_test_helper (void *sema_);

/* 두 스레드 사이에서 제어가 "핑퐁"처럼 오가게 하는 세마포어 자체 테스트.
   무슨 일이 일어나는지 보려면 printf()를 넣어 보라. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* sema_self_test()가 쓰는 스레드 함수. */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* LOCK을 초기화한다. lock은 한 순간에 최대 한 스레드만 가질 수 있다.
   우리 lock은 "재귀적"이지 않다. 즉 lock을 이미 가진 스레드가
   같은 lock을 또 얻으려고 하면 오류다.
 
   lock은 초깃값이 1인 세마포어를 특수하게 만든 것이다.
   그런 세마포어와 lock의 차이는 두 가지다.
   첫째, 세마포어는 값이 1보다 클 수 있지만, lock은 한 번에 한 스레드만 가질 수 있다.
   둘째, 세마포어는 주인이 없어서 한 스레드가 down하고 다른 스레드가 up해도 되지만,
   lock은 얻은 스레드가 직접 풀어야 한다.
   이런 제약이 번거롭게 느껴진다면 lock 대신 세마포어를 써야 한다는 좋은 신호다.
 
   elem은 holder의 locks에 넣을 때만 연결 고리로 쓰므로 따로 초기화하지 않는다. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* LOCK을 얻는다. 필요하면 얻을 수 있을 때까지 잠든다.
   현재 스레드가 이미 이 lock을 가지고 있으면 안 된다.
 
   잠들 수 있으므로 인터럽트 핸들러 안에서 부르면 안 된다.
   인터럽트를 끈 상태에서 불러도 되지만, 잠들어야 하면 인터럽트가 다시 켜진다.
 
   Priority donation 흐름 (인터럽트를 끈 한 구간):
   1. holder가 있으면 waiting_lock에 이 lock을 적고, holder를 내 우선순위까지 올린다.
      (holder도 다른 lock을 기다리면 thread_donate_priority가 체인을 따라 올린다.)
   2. sema_down으로 기다린다. 나는 이 lock의 세마포어 waiters에 들어간다.
   3. 얻었으면 waiting_lock을 지우고, holder가 되고, 내 locks에 이 lock을 넣는다.
   holder 확인부터 sema_down까지 끼어들면, 이미 holder가 아닌 스레드에게
   기부할 수 있어서 인터럽트를 끈 채로 진행한다. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	struct thread *curr = thread_current ();
	enum intr_level old_level;
	old_level = intr_disable ();
	
	if (lock->holder != NULL) {
		curr->waiting_lock = lock;
		thread_donate_priority (lock->holder, curr->priority);
	}

	sema_down (&lock->semaphore);
	curr->waiting_lock = NULL;
	lock->holder = curr;
	list_push_back (&curr->locks, &lock->elem);

	intr_set_level (old_level);
}

/* LOCK을 얻어 보고, 얻으면 true, 못 얻으면 false를 돌려준다.
   현재 스레드가 이미 이 lock을 가지고 있으면 안 된다.
 
   잠들지 않으므로 인터럽트 핸들러 안에서 불러도 된다.
 
   기다리지 않으니 donation은 없다. 하지만 얻었다면 lock_acquire와 똑같이
   내 locks에 넣어야 한다. 빠뜨리면 이 lock을 기다리는 스레드의 기부가
   thread_refresh_priority 계산에서 빠진다. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success) {
		lock->holder = thread_current ();
		list_push_back (&thread_current ()->locks, &lock->elem);
	}
	return success;
}

/* LOCK을 놓는다. 현재 스레드가 가지고 있는 lock이어야 한다.
 
   인터럽트 핸들러는 lock을 얻을 수 없으므로, 핸들러 안에서
   lock을 놓으려는 것은 말이 되지 않는다.
 
   순서 (인터럽트를 끈 한 구간):
   1. 내 locks에서 이 lock을 뺀다. 이 lock의 waiters는 이제 나에게 기부하지 않는다.
   2. thread_refresh_priority ()로 남은 lock들 기준으로 우선순위를 다시 계산한다.
   3. holder를 비우고 sema_up으로 가장 높은 waiter를 깨운다.
   다시 계산이 sema_up보다 먼저여야, sema_up의 양보 판단에서 깨운 스레드에게
   바로 양보한다 (priority-donate-one).
   다시 계산과 holder = NULL 사이에 다른 스레드가 이 lock을 요청하면
   이미 푸는 중인 나에게 기부해서, lock을 푼 뒤에도 우선순위가 올라간 채
   남을 수 있다. 그래서 인터럽트를 끈 채로 진행한다. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));
	
	enum intr_level old_level;
	old_level = intr_disable ();

	list_remove (&lock->elem);
	thread_refresh_priority ();
	lock->holder = NULL;
	sema_up (&lock->semaphore);

	intr_set_level (old_level);
}

/* 현재 스레드가 LOCK을 가지고 있으면 true, 아니면 false를 돌려준다.
   (다른 스레드가 lock을 가지고 있는지 검사하는 것은 경쟁 상태(race)가 생길 수 있다는 점에 주의.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* 리스트에 들어가는 세마포어 하나. 조건 변수의 waiter 하나에 해당한다.
   각 waiter는 자기 전용 세마포어에서 잠든다. */
struct semaphore_elem {
	struct list_elem elem;              /* 리스트 원소 (cond->waiters에 들어간다). */
	struct semaphore semaphore;         /* 이 waiter 전용 세마포어. */
	struct thread *thread;				/* 이 세마포어에서 기다리는 스레드. cond_signal에서 우선순위 비교에 쓴다. */
};

/* list_max()용 비교 함수 (struct semaphore_elem 기준).
   condvar의 waiters에는 스레드가 아니라 semaphore_elem이 들어 있으므로,
   priority_less 대신 이 함수로 각 waiter의 스레드 우선순위를 비교한다.
   A의 스레드 우선순위가 B보다 작으면 true, 같거나 크면 false를 반환한다. */
static bool
sema_elem_priority_less (const struct list_elem *a, const struct list_elem *b,
		void *aux UNUSED) {
	struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

	return sa->thread->priority < sb->thread->priority;
}

/* 조건 변수 COND를 초기화한다.
   조건 변수는 한 코드가 어떤 조건을 알리고(signal),
   함께 동작하는 다른 코드가 그 신호를 받아 행동할 수 있게 해 준다. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* LOCK을 원자적으로 놓고, 다른 코드가 COND를 signal할 때까지 기다린다.
   COND가 signal되면 돌아오기 전에 LOCK을 다시 얻는다.
   이 함수를 부르기 전에 LOCK을 가지고 있어야 한다.
 
   이 함수가 구현하는 모니터는 "Hoare" 방식이 아니라 "Mesa" 방식이다.
   즉 신호를 보내는 것과 받는 것이 하나의 원자적 연산이 아니다.
   그래서 보통 호출한 쪽은 기다림이 끝난 뒤 조건을 다시 확인하고,
   필요하면 다시 기다려야 한다.
 
   조건 변수 하나는 lock 하나와만 연결되지만, lock 하나에는 조건 변수
   여러 개가 연결될 수 있다. 즉 lock과 조건 변수는 일대다 관계다.
 
   잠들 수 있으므로 인터럽트 핸들러 안에서 부르면 안 된다.
   인터럽트를 끈 상태에서 불러도 되지만, 잠들어야 하면 인터럽트가 다시 켜진다.
 
   waiter.thread에 현재 스레드를 기록해 두어서, cond_signal이 우선순위로 고를 수 있게 한다. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	waiter.thread = thread_current ();
	list_push_back (&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* COND(LOCK으로 보호됨)에서 기다리는 스레드가 있으면 그중 하나를 깨운다.
   이 함수를 부르기 전에 LOCK을 가지고 있어야 한다.
 
   인터럽트 핸들러는 lock을 얻을 수 없으므로, 핸들러 안에서
   조건 변수에 signal하려는 것은 말이 되지 않는다.
 
   Priority Scheduling: waiters 중 기다리는 스레드의 우선순위가 가장 높은
   waiter를 list_max로 골라서 그 세마포어를 up한다. 양보 판단은 sema_up이 한다. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
		struct list_elem *high_elem = list_max (&cond->waiters, sema_elem_priority_less, NULL);
		list_remove (high_elem);
		sema_up (&list_entry (high_elem, struct semaphore_elem, elem)->semaphore);
	}				
}

/* COND(LOCK으로 보호됨)에서 기다리는 스레드를 모두 깨운다.
   이 함수를 부르기 전에 LOCK을 가지고 있어야 한다.
 
   인터럽트 핸들러는 lock을 얻을 수 없으므로, 핸들러 안에서
   조건 변수에 signal하려는 것은 말이 되지 않는다. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
