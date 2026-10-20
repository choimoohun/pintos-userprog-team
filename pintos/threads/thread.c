#include "threads/thread.h"
#include "threads/thread-internal.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 magic 멤버에 넣는 임의의 값.
   스택 오버플로를 감지하는 데 쓴다. 자세한 내용은 thread.h 맨 위의 큰 주석을 보라. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드용 임의의 값.
   이 값은 바꾸지 말 것. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태인 스레드 목록 (ready list).
   실행할 준비는 됐지만 지금 실행 중은 아닌 스레드들이다.
   넣을 때는 맨 뒤에 넣고(list_push_back), 고를 때 next_thread_to_run에서
   list_max로 우선순위가 가장 높은 스레드를 꺼낸다. */
static struct list ready_list;

/* 잠든 스레드 목록 (Alarm Clock), wakeup_tick 오름차순. */
static struct list sleep_list;

/* idle 스레드. 실행할 스레드가 없을 때 돈다. */
static struct thread *idle_thread;

/* 처음 스레드. init.c의 main()을 실행하는 스레드다. */
static struct thread *initial_thread;

/* allocate_tid()가 쓰는 lock. */
static struct lock tid_lock;

/* 정리(해제) 요청이 들어온 스레드 목록. */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;    /* idle 상태로 보낸 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드에서 보낸 타이머 틱 수. */
static long long user_ticks;    /* 사용자 프로그램에서 보낸 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4            /* 스레드 하나에 주는 타이머 틱 수 (타임 슬라이스). */
static unsigned thread_ticks;   /* 마지막으로 양보한 뒤 지난 타이머 틱 수. */

/* false(기본값)이면 MLFQS를 쓰지 않는다 (지금은 우선순위 스케줄러).
   true이면 multi-level feedback queue 스케줄러(MLFQS)를 쓴다.
   커널 명령줄 옵션 "-o mlfqs"로 정한다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 올바른 스레드를 가리키는 것 같으면 true를 돌려준다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 지금 실행 중인 스레드를 돌려준다.
   CPU의 스택 포인터 rsp를 읽어서 페이지 시작 주소로 내림한다.
   struct thread는 항상 페이지 맨 앞에 있고 스택 포인터는
   페이지 중간 어딘가에 있으므로, 이렇게 하면 현재 스레드를 찾을 수 있다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start용 GDT(전역 디스크립터 테이블).
// GDT는 thread_init 다음에 설정되므로,
// 그 전에 쓸 임시 GDT를 먼저 설정해 둔다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 지금 실행 중인 코드를 스레드로 바꿔서 스레드 시스템을 초기화한다.
   보통은 이렇게 할 수 없고, loader.S가 스택 바닥을 페이지 경계에
   맞춰 두었기 때문에 이 경우에만 가능하다.
 
   ready list, sleep_list와 tid lock도 초기화한다.
 
   이 함수를 부른 뒤 thread_create()로 스레드를 만들기 전에
   반드시 페이지 할당자(palloc)를 먼저 초기화해야 한다.
 
   이 함수가 끝나기 전에는 thread_current()를 부르면 안전하지 않다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널용 임시 GDT를 다시 불러온다.
	   이 GDT에는 사용자 문맥이 들어 있지 않다.
	   커널은 나중에 gdt_init ()에서 사용자 문맥을 포함한 GDT를 다시 만든다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 자료구조를 초기화한다. */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&destruction_req);

	/* 지금 실행 중인 코드를 위한 struct thread를 설정한다. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 켜서 선점형 스레드 스케줄링을 시작한다.
   idle 스레드도 만든다. */
void
thread_start (void) {
	/* idle 스레드를 만든다. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링을 시작한다. */
	intr_enable ();

	/* idle 스레드가 idle_thread를 초기화할 때까지 기다린다. */
	sema_down (&idle_started);
}

/* 매 타이머 틱마다 타이머 인터럽트 핸들러가 부른다.
   따라서 이 함수는 외부 인터럽트 문맥에서 실행된다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계를 갱신한다. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 타임 슬라이스를 다 쓰면 인터럽트가 끝날 때 양보하게 한다 (선점). */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계를 출력한다. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 이름이 NAME이고 처음 우선순위가 PRIORITY인 새 커널 스레드를 만든다.
   새 스레드는 AUX를 인자로 FUNCTION을 실행하고, ready list에 들어간다.
   새 스레드의 tid를 돌려주고, 만들지 못하면 TID_ERROR를 돌려준다.
 
   thread_start()가 불린 뒤라면, thread_create()가 돌아오기 전에
   새 스레드가 먼저 스케줄될 수도 있고 심지어 끝나 버릴 수도 있다.
   반대로 원래 스레드가 새 스레드보다 한참 먼저 실행될 수도 있다.
   순서를 보장해야 하면 세마포어 같은 동기화 도구를 써야 한다.
 
   Priority Scheduling: 새 스레드를 ready list에 넣은 직후
   thread_yield_to_higher ()로, 새 스레드가 지금 스레드보다 높으면 바로 양보한다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드용 페이지를 할당한다. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드를 초기화한다. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄되면 kernel_thread를 실행하도록 설정한다.
	   참고) rdi는 첫 번째 인자, rsi는 두 번째 인자다. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* ready list에 넣고, 새 스레드가 더 높으면 양보한다. */
	thread_unblock (t);
	thread_yield_to_higher ();

	return tid;
}

/* 현재 스레드를 재운다(BLOCKED). thread_unblock()으로 깨울 때까지
   다시 스케줄되지 않는다.
 
   인터럽트를 끈 상태에서 불러야 한다.
   보통은 직접 부르기보다 synch.h의 동기화 도구를 쓰는 편이 낫다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* BLOCKED 상태인 스레드 T를 READY 상태로 바꾸고 ready list 맨 뒤에 넣는다.
   T가 BLOCKED가 아니면 오류다. (실행 중인 스레드를 READY로 만들려면
   thread_yield()를 쓴다.)
 
   이 함수는 실행 중인 스레드를 선점하지 않는다. 이게 중요할 수 있다.
   호출한 쪽이 직접 인터럽트를 끈 상태라면, 스레드를 unblock하는 것과
   다른 데이터를 바꾸는 것을 한 번에(원자적으로) 처리하길 기대할 수 있다.
   그래서 더 높은 스레드에게 양보할지는 호출한 쪽이 할 일을 다 끝낸 뒤
   thread_yield_to_higher ()로 판단한다 (예: sema_up은 value++ 다음에). */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_push_back (&ready_list, &t->elem);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* list_insert_ordered()용 비교 함수.
   A의 wakeup_tick이 B보다 작으면 true, 같거나 크면 false를 반환한다.
   같을 때 false이므로, 기상 틱이 같은 스레드는 먼저 잠든 순서대로 놓인다. */
static bool
wakeup_tick_less (const struct list_elem *a, const struct list_elem *b,
		void *aux UNUSED) {
	struct thread *ta = list_entry (a, struct thread, elem);
	struct thread *tb = list_entry (b, struct thread, elem);

	return ta->wakeup_tick < tb->wakeup_tick;
}

/* list_max()용 비교 함수 (elem 기준).
   A의 priority가 B보다 작으면 true, 같거나 크면 false를 반환한다.
   같을 때 false이므로, 우선순위가 같으면 리스트 앞쪽(먼저 들어온) 스레드가 골라진다.
   ready list, 세마포어 waiters, lock의 waiters에서 모두 이 함수로 비교한다. */
bool
priority_less (const struct list_elem *a, const struct list_elem *b,
		void *aux UNUSED) {
	struct thread *ta = list_entry (a, struct thread, elem);
	struct thread *tb = list_entry (b, struct thread, elem);

	return ta->priority < tb->priority;
}

/* 현재 스레드를 WAKEUP_TICK까지 재운다.
   sleep_list에 wakeup_tick 오름차순으로 넣고 block한다.
   깨우는 쪽은 thread_wakeup()이다. sleep_list를 타이머 인터럽트와
   함께 쓰므로, 삽입부터 block까지 인터럽트를 끈 채로 진행한다. */
void
thread_sleep (int64_t wakeup_tick) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_ON);
	ASSERT (curr != idle_thread);

	curr->wakeup_tick = wakeup_tick;

	old_level = intr_disable ();
	list_insert_ordered (&sleep_list, &curr->elem, wakeup_tick_less, NULL);
	thread_block ();
	intr_set_level (old_level);
}

/* 타이머 인터럽트에서 매 틱 호출된다.
   sleep_list 앞에서부터 wakeup_tick <= CURRENT_TICK인 스레드를 꺼내 unblock한다.
   리스트가 오름차순이라 아직 깰 때가 아닌 스레드를 만나면 멈춘다.
   elem을 ready_list에서 재사용하므로 반드시 리스트에서 뺀 뒤 unblock한다.
   깨울 스레드를 모두 넣은 뒤 한 번만 양보를 판단한다 (인터럽트 안이라 intr_yield_on_return). */
void
thread_wakeup (int64_t current_tick) {
	ASSERT (intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);

	while (!list_empty (&sleep_list)) {
		struct thread *t =
			list_entry (list_front (&sleep_list), struct thread, elem);

		if (t->wakeup_tick > current_tick)
			break;

		list_pop_front (&sleep_list);
		thread_unblock (t);
	}

	thread_yield_to_higher ();
}

/* 지금 실행 중인 스레드의 이름을 돌려준다. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 지금 실행 중인 스레드를 돌려준다.
   running_thread()에 몇 가지 검사를 더한 것이다.
   자세한 내용은 thread.h 맨 위의 큰 주석을 보라. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 정말 스레드인지 확인한다.
	   이 assertion 중 하나라도 터지면 스레드의 스택이 넘쳤을 수 있다.
	   스레드마다 스택이 4 kB보다 작아서, 큰 지역 배열 몇 개나
	   적당한 깊이의 재귀만으로도 스택이 넘칠 수 있다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 지금 실행 중인 스레드의 tid를 돌려준다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 스케줄에서 빼고 없앤다.
   호출한 곳으로 절대 돌아오지 않는다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 DYING으로 바꾸고 다른 스레드를 스케줄한다.
	   실제 정리는 다음 schedule()이 시작될 때 일어난다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보한다. 현재 스레드는 잠들지 않고 ready list 맨 뒤에 들어가며,
   스케줄러 판단에 따라 바로 다시 실행될 수도 있다.
   다음에 누가 실행될지는 next_thread_to_run이 정한다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_push_back (&ready_list, &curr->elem);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* ready list에 지금 스레드보다 우선순위가 높은 스레드가 있으면 양보한다.
   같으면 양보하지 않는다.
   핸들러 안에서는 intr_yield_on_return (), 밖에서는 thread_yield ()로 양보한다.
   ready list에 스레드를 넣거나 우선순위를 바꾼 뒤 부른다.
   호출하는 곳: thread_create, thread_wakeup, thread_set_priority, sema_up. */
void
thread_yield_to_higher (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	old_level = intr_disable ();

	if (!list_empty (&ready_list)) {
		struct list_elem *high_elem = list_max (&ready_list, priority_less, NULL);

		if (priority_less (&curr->elem, high_elem, NULL)) {
			if (intr_context ())
				intr_yield_on_return ();
			else
				thread_yield ();
		}
	}

	intr_set_level (old_level);
}

/* 스레드 T의 우선순위를 P까지 올린다 (priority donation).
   T도 lock을 기다리는 중이면(waiting_lock이 있으면) 그 lock의 holder로 이어서 올린다 (nested donation).
 
   - T가 이미 P 이상이면 멈춘다. 더 올릴 필요가 없고, 그 위쪽도 이미 그만큼 올라가 있다.
   - 체인은 최대 8단계까지만 따라간다 (KAIST 문서의 "such as 8 levels").
     위의 멈춤 조건 덕분에 제한이 없어도 루프는 끝난다.
   - T가 READY여도 ready list를 다시 정렬하지 않는다.
     next_thread_to_run이 list_max로 고르므로 리스트 안의 위치는 상관없다.
 
   lock_acquire에서 holder가 있을 때 thread_donate_priority (lock->holder, curr->priority)로 부른다. */
void
thread_donate_priority (struct thread *t, int p) {
	int depth = 0;

	while (depth < 8 && t != NULL)
	{
		if (t->priority >= p) {
			break;
		}

		t->priority = p;

		if (t->waiting_lock == NULL) {
			break;
		}

		t = t->waiting_lock->holder;
		depth++;
	}
}

/* 현재 스레드의 우선순위를 다시 계산한다.
   priority = max(base_priority, 내가 들고 있는 각 lock의 waiters 중 가장 높은 우선순위)
 
   기부자를 따로 기록하지 않는다. lock을 기다리는 스레드는 이미 그 lock의
   세마포어 waiters에 있으므로, 내 locks를 돌며 waiters에서 바로 최댓값을 찾는다.
   waiters의 스레드 우선순위에는 nested donation으로 올라간 값도 이미 반영돼 있다.
 
   부르는 곳:
   - lock_release: 푼 lock을 locks에서 뺀 다음, sema_up 전에
   - thread_set_priority: base_priority를 바꾼 다음 */
void 
thread_refresh_priority (void) {
	struct thread *curr = thread_current ();
	struct list_elem *e;
	
	curr->priority = curr->base_priority;

	for (e = list_begin (&curr->locks); e != list_end (&curr->locks); e = list_next (e)) {
		struct lock *lock = list_entry (e, struct lock, elem);

		if (!list_empty (&lock->semaphore.waiters)) {
			struct list_elem *high_elem = list_max (&lock->semaphore.waiters, priority_less, NULL);
			struct thread *high = list_entry (high_elem, struct thread, elem);

			if (high->priority > curr->priority)
				curr->priority = high->priority;
		}
	}
}

/* 현재 스레드의 원래 우선순위(base_priority)를 NEW_PRIORITY로 바꾼다.
   priority를 직접 바꾸지 않고, base를 바꾼 뒤 다시 계산한다.
   그래서 donation을 받고 있으면 기부받은 우선순위가 유지되고,
   lock을 다 풀었을 때 NEW_PRIORITY가 적용된다 (priority-donate-lower).
   다시 계산해서 내려갔으면 더 높은 스레드에게 양보한다. */
void
thread_set_priority (int new_priority) {
	thread_current ()->base_priority = new_priority;
	thread_refresh_priority ();
	thread_yield_to_higher ();
}

/* 현재 스레드의 우선순위를 돌려준다.
   donation을 받고 있으면 기부받은 (더 높은) 우선순위를 돌려준다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 바꾼다. (MLFQS) */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: MLFQS 구현 */
}

/* 현재 스레드의 nice 값을 돌려준다. (MLFQS) */
int
thread_get_nice (void) {
	/* TODO: MLFQS 구현 */
	return 0;
}

/* 시스템 load_avg에 100을 곱한 값을 돌려준다. (MLFQS) */
int
thread_get_load_avg (void) {
	/* TODO: MLFQS 구현 */
	return 0;
}

/* 현재 스레드의 recent_cpu에 100을 곱한 값을 돌려준다. (MLFQS) */
int
thread_get_recent_cpu (void) {
	/* TODO: MLFQS 구현 */
	return 0;
}

/* idle 스레드. 실행할 다른 스레드가 없을 때 실행된다.
 
   idle 스레드는 처음에 thread_start()가 ready list에 넣는다.
   처음 한 번 스케줄되면 idle_thread를 초기화하고, 전달받은 세마포어를
   up해서 thread_start()가 계속 진행하게 한 뒤, 곧바로 block된다.
   그 뒤로 idle 스레드는 ready list에 다시 나타나지 않는다.
   ready list가 비어 있을 때 next_thread_to_run()이 특별히 돌려준다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드가 실행되게 한다. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 켜고 다음 인터럽트를 기다린다.
		
		   sti 명령은 바로 다음 명령이 끝날 때까지 인터럽트를 막아 두기 때문에,
		   아래 두 명령은 한 번에(원자적으로) 실행된다. 이게 중요하다.
		   그렇지 않으면 인터럽트를 다시 켠 순간과 다음 인터럽트를 기다리기
		   시작하는 순간 사이에 인터럽트가 처리되어, 최대 한 틱만큼의
		   시간을 낭비할 수 있다.
		
		   [IA32-v2a] "HLT", [IA32-v2b] "STI", [IA32-v3a] 7.11.1 "HLT Instruction"을 보라. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드가 실행을 시작하는 바탕 함수. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 채 실행되므로 다시 켠다. */
	function (aux);       /* 스레드 함수를 실행한다. */
	thread_exit ();       /* function()이 끝나면 스레드를 종료한다. */
}


/* T를 이름이 NAME인 BLOCKED 상태 스레드로 기본 초기화한다.
   donation 필드도 여기서 초기화한다.
   base_priority는 처음 우선순위와 같고, 기다리는 lock은 없고(NULL),
   들고 있는 lock 목록은 비어 있다. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->base_priority = priority;
	t->waiting_lock = NULL;
	list_init (&t->locks);
	t->magic = THREAD_MAGIC;
#ifdef USERPROG
	t->exit_status = -1;
	t->fd_table = NULL;			/* fd_table을 NULL로 */
	list_init (&t->children);
	t->my_info = NULL;
#endif
}

/* 다음에 실행할 스레드를 골라서 돌려준다.
   ready list에서 고르고, ready list가 비어 있으면 idle_thread를 돌려준다.
   (실행 중인 스레드가 계속 실행될 수 있다면, 그 스레드도 ready list에 들어 있다.)
 
   ready list에서 우선순위가 가장 높은 스레드를 고른다 (list_max + priority_less).
   우선순위가 같으면 먼저 들어온 스레드가 골라진다.
   thread_yield, thread_block, thread_exit가 모두 schedule()을 거쳐 여기로 오므로,
   우선순위로 고르는 곳은 이 함수 한 곳이다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else {
		struct list_elem *high_elem = list_max (&ready_list, priority_less, NULL);
		list_remove (high_elem);

		return list_entry (high_elem, struct thread, elem);
	}
}

/* iretq 명령으로 스레드를 시작한다. */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 페이지 테이블을 활성화해서 스레드를 바꾸고,
   이전 스레드가 DYING 상태면 정리한다.
 
   이 함수가 불릴 때는 이전 스레드 PREV에서 막 바뀐 상태이고,
   새 스레드는 이미 실행 중이며, 인터럽트는 아직 꺼져 있다.
 
   스레드 전환이 끝나기 전에는 printf()를 부르면 안전하지 않다.
   실제로는 printf()를 이 함수 맨 끝에만 넣어야 한다는 뜻이다. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 스레드 전환의 핵심 부분.
	   먼저 현재 실행 문맥 전체를 intr_frame에 저장하고,
	   do_iret을 불러서 다음 스레드로 전환한다.
	   주의: 전환이 끝날 때까지 여기서부터는 스택을 쓰면 안 된다. */
	__asm __volatile (
			/* 사용할 레지스터를 저장한다. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* 입력을 한 번만 읽어 온다. */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rcx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rcx
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // 현재 rip을 읽는다.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 스레드를 스케줄한다. 들어올 때 인터럽트가 꺼져 있어야 한다.
   현재 스레드의 상태를 STATUS로 바꾸고,
   실행할 다른 스레드를 찾아서 그 스레드로 전환한다.
   schedule() 안에서는 printf()를 부르면 안전하지 않다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

/* 다음 스레드를 골라서 그 스레드로 전환한다.
   do_schedule()이 현재 스레드의 상태를 바꾼 뒤 부른다. 인터럽트가 꺼져 있어야 한다. */
static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* 다음 스레드를 실행 중 상태로 표시한다. */
	next->status = THREAD_RUNNING;

	/* 새 타임 슬라이스를 시작한다. */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새 주소 공간을 활성화한다. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 방금 전환되어 나온 스레드가 DYING 상태면 그 struct thread를 정리한다.
		   thread_exit()가 자기 발밑을 치워 버리지 않도록 이 정리는 늦게 해야 한다.
		   그 페이지는 지금 스택으로 쓰이고 있으므로, 여기서는 해제 요청만 넣어 둔다.
		   실제 해제는 다음 schedule()의 시작 부분(do_schedule)에서 일어난다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 바꾸기 전에 현재 실행 중인 스레드의 정보를 먼저 저장한다. */
		thread_launch (next);
	}
}

/* 새 스레드에 쓸 tid를 돌려준다. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}