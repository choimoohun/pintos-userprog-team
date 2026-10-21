/* 사용자 프로세스를 만들고(initd, fork), 바꾸고(exec), 기다리고(wait), 끝내는(exit) 곳.
   부모와 자식은 struct child_info(thread.h)로 이어져 있다. */

#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* 명령줄 단어 최대 개수 (프로그램 이름 포함) */
#define WORD_SIZE 64

/* 부모의 process_fork 지역 변수로 만들어서 자식에게 넘긴다.
   부모는 fork_sema에서 기다리므로, 자식이 sema_up 하기 전까지는 살아 있다.
   자식은 sema_up 이후로는 이 구조체를 건드리지 않는다. */
struct fork_args {
	/* 복사할 부모 스레드 (pml4, fd 표, 실행 파일) */
	struct thread *parent;
	/* 부모가 fork를 부른 순간의 사용자 레지스터 */
	struct intr_frame *parent_if;
	/* 결과를 적고 부모를 깨울 자식 정보 */
	struct child_info *info;
};

/* process_create_initd에서 malloc으로 만들어 initd에 넘긴다.
   fork_args와 달리 지역 변수로 두면 안 된다.
   process_create_initd는 initd를 기다리지 않고 바로 돌아가므로
   initd가 꺼내 쓰기 전에 지역 변수가 사라질 수 있다.
   initd가 내용을 꺼낸 뒤 free한다. */
struct initd_args {
	char *fn_copy;
	struct child_info *info;
};

static void process_cleanup (void);
static bool load (const char *file_name, char *words[], int argc, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static struct child_info *child_info_create (void);
static void child_info_release (struct child_info *info);

/* initd와 fork 자식이 처음에 공통으로 하는 준비.
   지금은 fd 표 페이지만 받는다. PAL_ZERO라 모든 칸이 NULL로 시작한다. */
static void
process_init (void) {
	struct thread *current = thread_current ();
	/* current의 fd_table에 페이지 한 장 받기 */
	current->fd_table = palloc_get_page (PAL_ZERO);
}

/* FILE_NAME 명령줄을 첫 사용자 프로그램(initd)을 실행할 스레드를 만든다.
   initd의 tid를 돌려주고 만들지 못하면 TID_ERROR를 돌려준다.
   Pintos가 켜질 때 main 스레드가 한 번만 부른다.

   main은 이 tid로 process_wait를 불러 initd가 끝날 때까지 기다린다.
   그래서 fork와 마찬가지로 main과 initd가 같이 쓸 자식 정보를 만들어
   main의 children 줄에 건다. 자식 정보가 없으면 main의 wait가 initd를 찾지 못해
   바로 -1을 돌려주고, 테스트가 시작하기도 전에 Pintos가 꺼진다.

   새 스레드는 이 함수가 돌아오기 전에 실행될 수도, 끝나 버릴 수도 있다.
   그래서 initd에게 넘길 묶음(initd_args)은 지역 변수가 아니라 malloc으로 만든다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* 명령줄을 페이지 한 장에 복사해 둔다.
	   FILE_NAME은 main이 들고 있는 메모리라, initd가 load에서 읽기 전에
	   main이 바꾸거나 버릴 수 있다. 그래서 initd만 쓰는 복사본을 넘긴다. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* 스레드 이름은 첫 단어(프로그램 이름)만 쓴다.
	   exec 후에도 바뀌지 않도록 여기서 한 번만 정한다. */
	char name[16];

	strlcpy (name, file_name, sizeof name);

	char *first_space = strchr (name, ' ');

	if (first_space != NULL) {
		*first_space = '\0';
	}

	/* main과 initd가 같이 쓸 자식 정보를 만든다. main의 children 줄에 걸린다. */
	struct child_info *info = child_info_create ();

	if (info == NULL) {
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}

	/* initd에게 넘길 묶음을 malloc으로 만든다.
	   지역 변수면 initd가 꺼내기 전에 사라질 수 있다. */
	struct initd_args *args = malloc (sizeof (struct initd_args));

	if (args == NULL) {
		list_remove (&info->elem);
		free (info);
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}

	args->fn_copy = fn_copy;
	args->info = info;

	/* initd 스레드를 만들고 묶음을 넘긴다. */
	tid = thread_create (name, PRI_DEFAULT, initd, args);

	/* 스레드를 못 만들었으면 지금까지 받은 것을 모두 돌려준다. */
	if (tid == TID_ERROR) {
		free (args);
		list_remove (&info->elem);
		free (info);
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}

	/* main의 wait가 tid로 자식 정보를 찾는다. */
	info->tid = tid;
	return tid;
}

/* 첫 사용자 프로그램(initd)을 실행하는 스레드 함수.
   process_create_initd가 malloc으로 만들어 넘긴 묶음(initd_args)을 받는다. */
static void
initd (void *aux) {
	/* 묶음에서 명령줄과 자식 정보를 꺼낸다. */
	struct initd_args *args = (struct initd_args *) aux;
	char *f_name = args->fn_copy;

	/* 실행되자마자 자식 정보를 연결한다.
	   main이 thread_create 뒤에 채워 주는 방식이면, 그 전에 initd가 끝나 버릴 경우
	   exit에서 my_info가 NULL이라 main을 깨우지 못하고 main이 영원히 기다린다. */
	thread_current ()->my_info = args->info;

	/* 두 칸을 다 꺼냈으므로 묶음을 반납한다.
	   fn_copy가 가리키는 페이지는 process_exec가 다 쓴 뒤 반납한다. */
	free (args);
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 자식 정보 INFO를 쓰던 사람 중 한 명이 이제 안 쓴다고 떠날 때 부른다.
   부모와 자식 중 나중에 떠나는 쪽이 자식 정보를 지운다.

   부르는 곳은 세 곳이다.
   - 부모: wait에서 종료 상태를 읽은 뒤, fork 복제 실패를 확인한 뒤,
           children 줄에 남은 자식 정보마다
   - 자식: exit 할 때 자기 자식 정보(my_info)에 대해

   이 함수는 children 줄에서 빼지 않는다.
   children 줄은 부모의 것이므로, 부모가 이 함수를 부르기 전에 직접 뺀다.
   (이 함수는 자식 정보를 지울 수 있으므로, 빼는 것이 반드시 먼저다.) */
static void
child_info_release (struct child_info *info) {
	/* 부모와 자식이 거의 동시에 떠날 수 있다.
	   1 줄이기와 0인지 확인이 따로 실행되면 둘 다 0을 보고
	   같은 메모리를 두 번 지울 수 있으므로, 두 동작을 lock 안에서 한 번에 한다. */
	lock_acquire (&info->ref_lock);

	/* 나는 떠나므로 남은 사용자 수를 1 줄인다. */
	info->ref_cnt--;

	/* 0이 됐다면 상대는 이미 떠났고, 내가 마지막으로 남았다.
	   바로 지우지 않고 결과만 기억해 둔다. */
	bool last = (info->ref_cnt == 0);

	/* ref_lock은 자식 정보 안에 있으므로, 지우기 전에 먼저 푼다.
	   지운 뒤에 풀면 사라진 lock을 건드리게 된다. */
	lock_release (&info->ref_lock);

	/* 내가 마지막으로 떠나는 쪽이면 자식 정보를 지운다.
	   ref_cnt가 0이 된 뒤에는 상대가 이미 떠났으므로,
	   lock을 푼 뒤에 지워도 아무도 이 메모리를 건드리지 않는다. */
	if (last) {
		free (info);
	}
}

/* 자식 정보를 하나 만들어 처음 값을 채우고 현재 스레드의 children 줄에 건다.
   만든 자식 정보를 돌려준다. 메모리가 부족하면 NULL을 돌려준다.
   fork와 첫 프로세스(initd)를 만들 때 쓴다. */
static struct
child_info *child_info_create (void) {
	/* 자식 정보를 malloc으로 받는다.
	   지역 변수로 두면 이 함수가 끝날 때 사라지므로
	   wait, exit에서도 쓸 수 있게 따로 받은 메모리에 둔다. */
	struct child_info *info = malloc (sizeof (struct child_info));

	/* 메모리가 부족해 받지 못하면 NULL을 돌려준다. */
	if (info == NULL) {
		return NULL;
	}

	/* 자식이 exit을 부르지 못하고 죽으면(예: page fault) 이 값이 그대로 남아
	   부모의 wait가 -1을 받는다. */
	info->exit_status = -1;

	/* fork일 때만 쓴다. 자식이 복제에 성공하면 __do_fork에서 true로 바꾼다.
	   첫 프로세스(initd)는 복제를 하지 않으므로 이 칸을 쓰지 않는다. */
	info->fork_success = false;

	/* 두 세마포어 모두 0에서 시작해야, 부모가 sema_down에서 잠든다.
	   fork_sema: fork일 때만 쓴다. 부모는 fork에서 기다리고, 자식은 복제를 끝낸 뒤 깨운다.
	   wait_sema: 부모는 wait에서 기다리고, 자식은 exit할 때 깨운다.
	   하나로 같이 쓰면 fork 때 올린 신호가 남아 wait가 바로 통과해 버리므로 따로 둔다. */
	sema_init (&info->fork_sema, 0);
	sema_init (&info->wait_sema, 0);

	/* 이 자식 정보를 쓰는 사람은 부모와 자식, 둘이다.
	   한 명씩 떠날 때마다 1씩 줄이고, 0으로 만든 쪽이 지운다. */
	info->ref_cnt = 2;

	/* 부모와 자식이 동시에 ref_cnt를 줄이다가 둘 다 0을 보고
	   이중 해제하는 일을 막는다. */
	lock_init (&info->ref_lock);

	/* 현재 스레드(부모)의 children 줄 끝에 건다.
	   나중에 wait가 이 줄에서 tid로 자식 정보를 찾는다. */
	list_push_back (&thread_current ()->children, &info->elem);

	return info;
}

/* 현재 프로세스를 복제해 NAME이라는 이름의 자식 프로세스를 만든다.
   자식 복제에 성공하면 자식의 tid를, 실패하면 TID_ERROR를 돌려준다.

   부모와 자식이 함께 쓰는 자식 정보(struct child_info)를 child_info_create로 만든다.
   자식 정보는 struct thread와 따로 있어서,
   부모나 자식 중 한쪽이 먼저 끝나도 사라지지 않는다.

   부모는 자식이 복제를 끝낼 때까지 fork_sema에서 기다린 뒤 돌아온다.
   부모는 복제 성공 여부를 알기 전에 fork에서 돌아오면 안 된다. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* 자식 정보를 만든다. 메모리가 부족하면 자식을 만들 수 없다. */
	struct child_info *info = child_info_create ();

	/* 메모리가 부족해 받지 못하면 자식을 만들 수 없다. */
	if (info == NULL) {
		return TID_ERROR;
	}

	/* 자식에게 넘길 세 가지를 하나로 묶는다.
	   thread_create의 aux 칸이 하나뿐이라 구조체 주소 하나로 넘긴다.
	   args는 지역 변수지만, 부모가 아래 sema_down에서 기다리는 동안은
	   process_fork가 끝나지 않으므로 자식이 써도 안전하다. */
	struct fork_args args = {
		.parent = thread_current (),   /* 복사할 부모 (pml4, fd 표) */
		.parent_if = if_,              /* fork를 부른 순간의 사용자 레지스터 */
		.info = info,                  /* 결과를 적고 부모를 깨울 자식 정보 */
	};

	/* 자식 스레드를 만든다.
	   자식은 __do_fork에서 args를 받아 부모를 복제한다. */
	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, &args);


	/* 스레드를 만들지 못했으면 자식은 태어나지도 않았다.
	   자식 정보를 쓸 사람이 부모뿐이므로 ref_cnt를 따질 필요 없이 바로 정리한다.
	   줄에서 먼저 빼고 나서 반납한다. (반납한 뒤에 빼면 사라진 elem을 건드린다.) */
	if (tid == TID_ERROR) {
		list_remove (&info->elem);
		free (info);

		return TID_ERROR;
	}

	/* wait가 이 번호로 자식 정보를 찾을 수 있게 tid를 적는다. */
	info->tid = tid;

	/* 자식이 복제를 끝내고 sema_up 할 때까지 잠든다.
	   이 동안 부모의 process_fork는 끝나지 않으므로 args와 if_가 살아 있다. */
	sema_down (&info->fork_sema);

	/* 복제에 실패했으면(fork_success == false) 줄에서 빼고
	   child_info_release로 떠난 뒤 TID_ERROR를 돌려준다. */
	if (!info->fork_success) {
		list_remove (&info->elem);
		child_info_release (info);

		return TID_ERROR;
	}

	/* 복제에 성공했으면 자식의 tid를 돌려준다. */
	return tid;
}

#ifndef VM
/* 부모의 주소 공간을 복제한다. pml4_for_each가 부모 페이지마다 이 함수를 부른다.
   Project 2에서만 쓴다. false를 돌려주면 복사를 멈추고 fork가 실패한다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();			/* 자식 */
	struct thread *parent = (struct thread *) aux;		/* 부모 */
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. 커널 페이지는 복사하지 않는다. pml4_create가 이미 같이 쓰게 연결해 둔다. */
	if (is_kernel_vaddr (va)) {
		return true;
	}

	/* 2. 부모 페이지 찾기 */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. 자식용 PAL_USER 페이지를 새로 받는다. */
	newpage = palloc_get_page (PAL_USER);

	if (newpage == NULL) {
		return false;
	}

	/* 4. 부모 페이지를 newpage로 복사하고, 쓰기 가능 여부도 부모와 똑같이 */
	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. 자식 페이지 표의 같은 va 자리에 newpage를 연결 */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. 연결 실패하면 받은 페이지를 돌려준다.
		      이미 연결한 페이지들은 자식이 exit할 때 process_cleanup이 정리한다. */
		palloc_free_page (newpage);
		return false;
	}
	return true;
}
#endif

/* fork로 만들어진 자식 스레드가 처음 실행하는 함수.
   부모의 사용자 레지스터, 메모리, fd 표, 실행 파일을 복제한 뒤,
   부모가 fork를 부른 바로 그 지점에서 사용자 프로그램을 이어서 실행한다.

   복제 결과는 자식 정보(info)의 fork_success에 적고,
   fork_sema로 부모를 깨운다. 부모는 그때까지 process_fork에서 기다린다. */
static void
__do_fork (void *aux) {
	/* process_fork가 넘긴 부모, 부모 if_, 자식 정보 묶음.
	   부모의 지역 변수이므로, fork_sema로 부모를 깨운 뒤에는 건드리지 않는다. */
	struct fork_args *args = (struct fork_args *) aux;

	/* 부모 레지스터를 복사해 둘 자식의 레지스터 묶음. 마지막에 do_iret으로 이걸 들고
	   사용자 모드로 간다. */
	struct intr_frame if_;

	/* 메모리(pml4)와 fd 표를 복사해 올 부모. */
	struct thread *parent = args->parent;

	/* 지금 실행 중인 자식 자신. */
	struct thread *current = thread_current ();

	/* 부모가 fork 시스템 콜을 부른 순간의 사용자 레지스터.
	   parent->tf에는 사용자 레지스터가 없으므로 process_fork가 받은 if_를 넘겨받아 쓴다.
	   부모의 커널 스택에 있으므로 부모를 깨우기 전에 복사를 마쳐야 한다. */
	struct intr_frame *parent_if = args->parent_if;

	/* 부모와 함께 쓰는 자식 정보. 복제 결과를 적고 부모를 깨우는 데 쓴다. */
	struct child_info *info = args->info;

	/* 원래 뼈대에 있던 변수. 실패하면 바로 error로 가서 실제로는 항상 true다. */
	bool succ = true;

	/* 자식이 자기 자식 정보를 기억해 둔다.
	   나중에 exit할 때 여기로 종료 상태를 적고, wait_sema로 부모를 깨우고 떠난다. */
	current->my_info = info;

	/* 부모의 레지스터를 자식의 if_로 복사하고 자식이 fork에서 0을 받게 한다.
	   rip도 같이 복사되니까 자식은 부모와 같은 곳(fork 다음)부터 실행한다. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;

	/* 페이지 표 복제 */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	/* 메모리를 복사하는 곳 */
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	process_init ();

	if (!duplicate_fd_table (parent)) {
		goto error;
	}

	/* 자식도 같은 프로그램을 실행하므로, 실행 파일을 복제해 쓰기 막기를 하나 더 건다.
	   실패 검사는 lock을 푼 다음에 한다. error 뒤에 destroy_fd_table이 같은 lock을 또 잡는다. */
	if (parent->running_file != NULL) {
		lock_acquire (&filesys_lock);
		current->running_file = file_duplicate (parent->running_file);
		lock_release (&filesys_lock);

		if (current->running_file == NULL) {
			goto error;
		}
	}

	info->fork_success = true;
	sema_up (&info->fork_sema);

	/* 이제 새 프로세스로 넘어간다. */
	if (succ)
		do_iret (&if_);
error:
	/* fork_success는 false 그대로. 부모를 깨우고 끝낸다. 받은 것들은 process_exit가 치운다. */
	sema_up (&info->fork_sema);
	thread_exit ();
}

/* 현재 프로세스를 F_NAME 명령줄의 프로그램으로 바꾼다 (exec).
   실패하면 -1을 돌려주고, 성공하면 돌아오지 않는다.
   F_NAME은 palloc 페이지고 여기서 반납한다.
   스레드는 그대로라서 tid, 이름, fd 표, 자식 정보는 exec 뒤에도 남는다. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	char *word;				/* strtok_r이 돌려준 지금 자른 단어의 주소. */
	char *word_pointer;		/* strtok_r이 어디까지 잘랐는지 기억하는 칸. */
	char *words[WORD_SIZE];	/* 자른 단어 주소들을 차례로 담는 배열. */
	int argc = 0;			/* 단어 개수. */

	/* words[i]는 f_name 페이지 안을 가리킨다. 그래서 load가 끝날 때까지 반납하면 안 된다. */
	word = strtok_r (f_name," ", &word_pointer);

	while (word != NULL && argc < WORD_SIZE) {
		words[argc] = word;
		argc++;
		word = strtok_r (NULL," ", &word_pointer);
	}

	/* 단어가 WORD_SIZE개를 넘으면 버리지 않고 실패로 처리한다. */
	if (word != NULL)
	{
		palloc_free_page (file_name);
		return -1;
	}

	/* struct thread 안의 intr_frame은 쓸 수 없다.
	   현재 스레드가 다시 스케줄될 때 실행 정보를 그 칸에 저장하기 때문이다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 먼저 지금 프로그램을 치운다 (메모리, 실행 파일). */
	process_cleanup ();

	/* 그다음 새 프로그램을 올린다. */
	success = load (words[0], words, argc, &_if);

	/* load가 실패하면 그만둔다. -1을 받은 sys_exec가 exit(-1)로 끝낸다. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* 바뀐 프로그램을 시작한다. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* 자식 CHILD_TID가 끝날 때까지 기다리고, 그 자식의 종료 상태를 돌려준다.
   자식이 커널에 의해 강제로 끝났다면(예: page fault) -1을 돌려준다.

   CHILD_TID가 잘못된 값이거나, 내 자식이 아니거나, 이미 wait한 자식이면
   기다리지 않고 바로 -1을 돌려준다.
   셋 다 children 줄에서 못 찾는 경우라서 따로 처리하지 않는다.
   (두 번째 wait는 첫 wait가 줄에서 빼 버려서 못 찾는다.) */
int
process_wait (tid_t child_tid) {
	struct list *children = &thread_current ()->children;

	/* 내 children 줄을 처음부터 돌면서, tid가 child_tid인 자식 정보를 찾는다. */
	for (struct list_elem *e = list_begin (children); e != list_end (children); e = list_next (e)) {
		/* e가 걸린 자식 정보 상자를 꺼낸다. */
		struct child_info *info = list_entry (e, struct child_info, elem);

		/* 내가 찾는 자식이면 */
		if (info->tid == child_tid) {
			/* 자식이 exit할 때까지 잠든다. */
			sema_down (&info->wait_sema);

			/* 떠나기 전에 종료 상태를 꺼내 둔다. */
			int status = info->exit_status;

			/* 다시 wait하면 못 찾도록 줄에서 뺀다. */
			list_remove (&info->elem);

			/* 부모도 떠난다. */
			child_info_release (info);

			/* 종료 상태를 돌려준다. */
			return status;
		}
	}

	return -1;
}

/* 프로세스를 끝낸다. thread_exit ()이 부른다.
   커널 스레드도 여기를 지나가서, 받은 게 없으면 다 건너뛴다.
   자원을 먼저 다 돌려주고 나서 부모를 깨운다.
   부모가 먼저 깨서 끝나 버리면 자식 자원이 남은 채로 꺼질 수 있어서다. */
void
process_exit (void) {
	struct thread *curr = thread_current ();

	/* 종료 메시지 출력. process_cleanup이 pml4를 NULL로 바꾸니까 맨 앞에 둔다. */
	if (curr->pml4) {
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
	}

	destroy_fd_table ();
	process_cleanup ();

	if (curr->my_info != NULL) {
		/* 부모가 wait에서 읽을 수 있게 종료 상태를 적는다. */
		curr->my_info->exit_status = curr->exit_status;

		/* wait 중인 부모를 깨운다. */
		sema_up (&curr->my_info->wait_sema);

		/* 자식이 떠난다. */
		child_info_release (curr->my_info);

		curr->my_info = NULL;
	}

	/* wait 안 한 자식 정보가 남아 있으면 부모로서 떠난다. */
	while (!list_empty (&curr->children)) {
		struct list_elem *e = list_pop_front (&curr->children);
		struct child_info *info = list_entry (e, struct child_info, elem);
		child_info_release (info);
	}
}

/* 현재 프로세스가 쓰던 자원(실행 파일, 메모리)을 돌려준다.
   exit와 exec 둘 다 부르고, exec가 실패하면 두 번 불릴 수도 있다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

	/* 실행 파일을 닫아 쓰기 막기를 푼다.
	   exit와 exec 둘 다 여기를 거치므로 여기서 한 번에 처리한다. */
	if (curr->running_file != NULL) {
		lock_acquire (&filesys_lock);
		file_close (curr->running_file);
		lock_release (&filesys_lock);
		curr->running_file = NULL;				/* exec 실패 시 여기가 두 번 불려도 다시 닫지 않도록 */
	}

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 페이지 표를 지우고 커널 전용 페이지 표로 돌아간다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서는 순서가 중요하다.
		   페이지 표를 바꾸기 전에 curr->pml4를 먼저 NULL로 만들어야
		   타이머 인터럽트가 걸려도 프로세스 페이지 표로 다시 돌아가지 않는다.
		   또 프로세스 페이지 표를 지우기 전에 기본 페이지 표를 켜야 한다.
		   안 그러면 이미 지워진 페이지 표 위에서 돌게 된다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* 다음 스레드에서 사용자 코드를 돌릴 수 있게 CPU를 맞춘다.
   컨텍스트 스위치마다 불린다. */
void
process_activate (struct thread *next) {
	/* 스레드의 페이지 표를 켠다. */
	pml4_activate (next->pml4);

	/* 인터럽트, 시스템 콜 때 쓸 커널 스택 주소를 TSS에 적는다. */
	tss_update (next);
}

/* ELF 실행 파일을 올리는 부분.
   아래 정의들은 ELF 명세 [ELF1]에서 거의 그대로 가져왔다. */

/* ELF 타입. [ELF1] 1-2 참고. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시. */
#define PT_LOAD    1            /* 올릴 세그먼트. */
#define PT_DYNAMIC 2            /* 동적 링크 정보. */
#define PT_INTERP  3            /* 동적 로더 이름. */
#define PT_NOTE    4            /* 부가 정보. */
#define PT_SHLIB   5            /* 예약됨. */
#define PT_PHDR    6            /* 프로그램 헤더 표. */
#define PT_STACK   0x6474e551   /* 스택 세그먼트. */

#define PF_X 1          /* 실행 가능. */
#define PF_W 2          /* 쓰기 가능. */
#define PF_R 4          /* 읽기 가능. */

/* 실행 파일 헤더. [ELF1] 1-4 ~ 1-8 참고.
   ELF 파일 맨 앞에 있다. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 줄임말 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static void push_arguments (char *words[], int argc, struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME 실행 파일을 현재 스레드에 올린다.
   시작 주소는 IF_->rip에, 스택 위치는 IF_->rsp에 넣는다.
   성공하면 true, 실패하면 false.

   파일을 여는 곳부터 done까지 filesys_lock을 쥔다.
   안 쥐면 다른 프로세스가 시스템 콜로 파일 시스템을 쓰는 중에 같이 들어간다 (syn-read에서 터졌다). */
static bool
load (const char *file_name, char *words[], int argc, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	bool has_lock = false;
	int i;

	/* 페이지 표를 만들고 켠다. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* 실행 파일 열기 */
	lock_acquire (&filesys_lock);
	has_lock = true;

	file = filesys_open (file_name);

	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* 열자마자 쓰기를 막는다. 실행 중인 프로그램 파일이 바뀌면 안 된다. */
	file_deny_write (file);

	/* ELF 헤더를 읽고 확인한다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* 프로그램 헤더를 읽는다. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 이 세그먼트는 무시한다. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 보통 세그먼트.
						   앞부분은 디스크에서 읽고 나머지는 0으로 채운다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전부 0인 세그먼트.
						   디스크에서 아무것도 안 읽는다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* 스택을 만든다. */
	if (!setup_stack (if_))
		goto done;

	/* 시작 주소. */
	if_->rip = ehdr.e_entry;

	push_arguments (words, argc, if_);

	success = true;

done:
	/* 성공하면 실행 중에 쓰기를 계속 막아야 하므로 닫지 않고 running_file에 남겨 둔다.
	   (file_close가 쓰기 막기를 풀기 때문이다. 닫는 건 process_cleanup에서 한다.)
	   실패하면 실행할 프로그램이 없으니 바로 닫는다. */
	if (success) {
		t->running_file = file;
	}
	else {
		file_close (file);
	}

	/* pml4_create에서 실패하면 lock을 안 잡고 여기로 온다. 그때는 풀면 안 된다. */
	if (has_lock) {
		lock_release (&filesys_lock);
	}

	return success;
}

/* 명령줄에서 나눈 단어들을 사용자 스택에 쌓고,
   main(argc, argv)가 받을 수 있도록 레지스터를 맞춘다.

   "echo x y"면 이렇게 쌓인다 (위가 높은 주소).
       "y\0" "x\0" "echo\0"
       (8바이트 맞추기)
       NULL
       argv[2] argv[1] argv[0]   ← rsi
       가짜 return address       ← rsp,  rdi = argc */
static void
push_arguments (char *words[], int argc, struct intr_frame *if_) {
	uintptr_t user_words[WORD_SIZE];	/* 사용자 스택에 복사한 단어들의 주소. */

	/* 1. 단어 글자들을 마지막 단어부터 복사한다. */
	for (int i = argc - 1; i >= 0; i--) {
		int len = strlen (words[i]) + 1;
		if_->rsp -= len;
		memcpy ((void *) if_->rsp, words[i], len);
		user_words[i] = if_->rsp;
	}
	/* 2. rsp를 8의 배수로 내린다. */
	if_->rsp = ROUND_DOWN(if_->rsp, 8);

	/* 3. NULL과 argv 주소들을 넣는다. */
	if_->rsp -= 8;
	*(uintptr_t *) if_->rsp = 0;

	for (int i = argc - 1; i >= 0; i--) {
		/* rsp를 8 빼고, 그 자리에 user_words[i]를 쓴다. */
		if_->rsp -= 8;
		*(uintptr_t *) if_->rsp = user_words[i];
	}

	if_->R.rsi = if_->rsp;

	/* 4. 가짜 return address를 넣고 레지스터를 맞춘다. */
	if_->rsp -= 8;
	*(uintptr_t *) if_->rsp = 0;
	if_->R.rdi = argc;
}


/* PHDR이 FILE 안의 올바른, 올릴 수 있는 세그먼트인지 확인한다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr의 페이지 안 위치가 같아야 한다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 안을 가리켜야 한다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 p_filesz보다 작으면 안 된다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 빈 세그먼트는 안 된다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 시작과 끝이 모두 사용자 주소 안이어야 한다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 커널 주소 공간을 넘어 한 바퀴 돌면 안 된다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 0번 페이지는 연결하지 않는다.
	   허용하면 시스템 콜에 NULL을 넘긴 사용자 코드가
	   memcpy() 등의 NULL assertion으로 커널을 패닉시킬 수 있다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 괜찮다. */
	return true;
}

#ifndef VM
/* 이 블록은 Project 2에서만 쓴다.
   Project 2 이후에도 쓸 함수는 #ifndef 밖에 만든다. */

/* load() 도우미. */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 OFS부터 세그먼트를 주소 UPAGE에 올린다.
   READ_BYTES는 파일에서 읽고, 그 뒤 ZERO_BYTES는 0으로 채운다.
   WRITABLE이 false면 읽기 전용 페이지가 된다.
   메모리가 없거나 디스크 읽기에 실패하면 false. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산한다.
		   PAGE_READ_BYTES만큼 파일에서 읽고, 나머지 PAGE_ZERO_BYTES는 0으로. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 페이지 한 장 받기 */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 페이지 채우기 */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 프로세스 주소 공간에 연결 */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* 다음 페이지로 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK 바로 아래에 0으로 채운 페이지 한 장을 붙여 스택을 만든다.
   Project 2에서 스택은 이 한 장(4 kB)이 전부다. */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* 사용자 주소 UPAGE를 커널 주소 KPAGE에 연결한다.
   WRITABLE이 false면 읽기 전용.
   UPAGE는 아직 연결 안 된 주소여야 하고, KPAGE는 PAL_USER로 받은 페이지여야 한다.
   UPAGE가 이미 연결돼 있거나 메모리가 없으면 false. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* 그 주소에 페이지가 없는지 확인하고 연결한다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */