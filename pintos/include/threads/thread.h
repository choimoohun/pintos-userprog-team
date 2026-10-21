#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* struct thread의 waiting_lock 필드를 위한 전방 선언.
   struct lock의 정의는 synch.h에 있는데, 여기서는 포인터만 쓰므로 이름만 알려 주면 된다. */
struct lock;

/* 스레드의 생애 주기 상태. */
enum thread_status {
	THREAD_RUNNING,     /* 지금 CPU에서 실행 중인 스레드. */
	THREAD_READY,       /* 실행 중은 아니지만 실행할 준비가 됨 (ready list에 있음). */
	THREAD_BLOCKED,     /* 어떤 사건을 기다리는 중 (sema, lock, sleep 등). */
	THREAD_DYING        /* 곧 정리(해제)될 스레드. */
};

/* 스레드 식별자 타입.
   원하는 타입으로 다시 정의해도 된다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t의 오류 값. */

/* 스레드 우선순위 범위. 숫자가 클수록 우선순위가 높다. */
#define PRI_MIN 0                       /* 가장 낮은 우선순위. */
#define PRI_DEFAULT 31                  /* 기본 우선순위. */
#define PRI_MAX 63                      /* 가장 높은 우선순위. */
#define FD_MAX 128                      /* 프로세스당 fd 표 칸 수 (페이지 한 장). */

/* 커널 스레드 또는 사용자 프로세스.
 
   각 struct thread는 자기만의 4 kB 페이지 하나에 저장된다.
   struct thread 자체는 페이지 맨 아래(오프셋 0)에 있고,
   페이지의 나머지는 그 스레드의 커널 스택으로 쓴다.
   커널 스택은 페이지 맨 위(오프셋 4 kB)에서 아래쪽으로 자란다.
   그림으로 보면 이렇다.
 
        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |            intr_frame           |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+
 
   여기서 두 가지를 알 수 있다.
 
      1. struct thread가 너무 커지면 안 된다. 커지면 커널 스택을 쓸
         공간이 부족해진다. 기본 struct thread는 몇 바이트밖에 안 되고,
         1 kB보다 한참 작게 유지하는 게 좋다.
 
      2. 커널 스택이 너무 커지면 안 된다. 스택이 넘치면(overflow)
         스레드 상태를 덮어써서 망가뜨린다. 그래서 커널 함수에서는
         큰 구조체나 배열을 static이 아닌 지역 변수로 만들면 안 된다.
         대신 malloc()이나 palloc_get_page()로 동적 할당한다.
 
   둘 중 어느 문제든 첫 증상은 보통 thread_current()의 assertion 실패다.
   thread_current()는 실행 중인 스레드의 magic 멤버가 THREAD_MAGIC인지
   검사하는데, 스택이 넘치면 보통 이 값이 바뀌어서 assertion이 터진다. */

struct child_info {
   tid_t tid;                           /* pid */
   int exit_status;                     /* 종료 상태 */
   bool fork_success;                   /* fork 성공 여부 */
   struct semaphore fork_sema;          /* fork용 세마포어 */
   struct semaphore wait_sema;          /* wait용 세마포어 */
   int ref_cnt;                         /* 남은 사용자 수 */
   struct lock ref_lock;                /* 남은 사용자 수를 지키는 lock */
   struct list_elem elem;               /* 부모 줄에 걸리는 elme */
};

/* elem 멤버는 두 가지 용도로 쓴다.
   ready list(thread.c)의 원소가 될 수도 있고,
   세마포어의 waiters(synch.c)의 원소가 될 수도 있다.
   두 용도는 동시에 일어나지 않기 때문에 이렇게 같이 쓸 수 있다.
   ready 상태인 스레드만 ready list에 있고,
   blocked 상태인 스레드만 세마포어 waiters에 있다.
   (Alarm Clock에서 잠든 스레드의 sleep_list도 blocked 상태라 같은 elem을 쓴다.) */
struct thread {
	/* thread.c가 관리하는 필드. */
	tid_t tid;                          /* 스레드 식별자. */
	enum thread_status status;          /* 스레드 상태. */
	char name[16];                      /* 이름 (디버깅용). */
	int priority;                       /* 실제로 적용되는 우선순위 (donation 반영). */

	/* Priority donation용 필드. */
	int base_priority;					/* 원래 우선순위 (donation 반영 전). thread_set_priority가 바꾸는 값. */
	struct lock *waiting_lock;			/* 내가 기다리는 lock, 없으면 NULL. */
	struct list locks;					/* 내가 들고 있는 lock 목록 (struct lock의 elem으로 연결). */

	/* thread.c와 synch.c가 함께 쓰는 필드. */
	struct list_elem elem;              /* 리스트 원소. ready list, 세마포어 waiters, sleep_list 중 한 곳에 들어간다. */

#ifdef USERPROG
	/* userprog/process.c가 관리하는 필드. */
	uint64_t *pml4;                     /* 4단계 페이지 맵 (PML4). */
   int exit_status;                    /* 종료 코드 */
   /* fd 표 (palloc으로 받은 페이지). fd_table[fd] = 열린 파일. */
   struct file **fd_table;
   /* 부모로서 내 자식 정보들의 줄 */
   struct list children;
   /* 자식으로서 부모 줄에 걸려 있는 나의 자식 정보 */
   struct child_info *my_info;
   /* 열어 둔 실행 파일을 저장하는 곳 */
   struct file *running_file;
#endif
#ifdef VM
	/* 스레드가 가진 가상 메모리 전체에 대한 테이블. */
	struct supplemental_page_table spt;
#endif

	/* thread.c가 관리하는 필드. */
	struct intr_frame tf;               /* 컨텍스트 스위칭 때 레지스터를 저장하는 곳. */
	int64_t wakeup_tick;                /* 깨어날 틱, sleep_list 정렬 기준 */
	unsigned magic;                     /* 스택 오버플로를 감지하는 값 (THREAD_MAGIC). */
};

/* false(기본값)이면 MLFQS를 쓰지 않는다 (지금은 우선순위 스케줄러).
   true이면 multi-level feedback queue 스케줄러(MLFQS)를 쓴다.
   커널 명령줄 옵션 "-o mlfqs"로 정한다. */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);
void thread_yield_to_higher (void);

/* 우선순위와 priority donation. */
int thread_get_priority (void);
void thread_set_priority (int);
void thread_donate_priority (struct thread *t, int p);
void thread_refresh_priority (void);
bool priority_less (const struct list_elem *a, const struct list_elem *b, void *aux);

/* MLFQS(Advanced Scheduler)용. 아직 구현하지 않았다. */
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */