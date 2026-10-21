#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* 지금까지 처리한 page fault 수. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* 사용자 프로그램이 일으킬 수 있는 인터럽트(예외)의 핸들러를 등록한다.

   실제 유닉스 계열 OS라면 이런 인터럽트 대부분을 시그널 형태로
   사용자 프로세스에 넘겨준다. 하지만 우리는 시그널을
   구현하지 않으므로, 그냥 사용자 프로세스를 끝낸다.

   page fault는 예외다. 지금은 다른 예외와 똑같이 처리하지만,
   가상 메모리를 구현하려면 바뀌어야 한다.

   각 예외의 설명은 5.15절 "Exception and Interrupt Reference"를 참고한다. */
void
exception_init (void) {
	/* 이 예외들은 사용자 프로그램이 INT, INT3, INTO, BOUND 명령으로
	   직접 일으킬 수 있다. 그래서 DPL==3으로 두어
	   사용자 프로그램이 이 명령으로 부르는 것을 허용한다. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* 이 예외들은 DPL==0이라 사용자 프로세스가 INT 명령으로 직접 부를 수 없다.
	   하지만 간접적으로는 일어날 수 있다. 예를 들어 #DE는 0으로 나누면 일어난다. */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* 대부분의 예외는 인터럽트를 켠 채로 처리해도 된다.
	   page fault는 인터럽트를 꺼야 한다. fault 주소가 CR2 레지스터에 담기는데,
	   읽기 전에 다른 인터럽트가 끼어들면 이 값이 바뀔 수 있기 때문이다. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계를 출력한다. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* (아마도) 사용자 프로세스가 일으킨 예외의 핸들러. */
static void
kill (struct intr_frame *f) {
	/* 이 인터럽트는 (아마도) 사용자 프로세스가 일으킨 것이다.
	   예를 들어 연결되지 않은 가상 메모리에 접근했을 수 있다 (page fault).
	   지금은 그냥 사용자 프로세스를 끝낸다. 나중에는 page fault를 커널에서
	   처리하고 싶을 것이다. 실제 유닉스 계열 OS는 대부분의 예외를 시그널로
	   프로세스에 돌려주지만, 우리는 시그널을 구현하지 않는다. */

	/* 인터럽트 프레임의 코드 세그먼트(cs) 값으로 예외가 어디서 났는지 알 수 있다. */
	switch (f->cs) {
		case SEL_UCSEG:
			/* 사용자 코드 세그먼트. 예상대로 사용자 예외이므로 사용자 프로세스를 끝낸다. */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
					thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:
			/* 커널 코드 세그먼트. 커널 버그라는 뜻이다.
			   커널 코드는 예외를 일으키면 안 된다. (page fault가 커널 예외를
			   일으킬 수는 있지만, 여기까지 오면 안 된다.) 커널을 패닉시켜 알린다. */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* 다른 코드 세그먼트? 일어나면 안 된다. 커널을 패닉시킨다. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* page fault 핸들러. 가상 메모리를 구현하려면 채워야 하는 뼈대다.
   Project 2 풀이에 따라서는 이 코드를 고쳐야 할 수도 있다.

   진입할 때 fault가 난 주소는 CR2(Control Register 2)에 있고,
   fault에 대한 정보는 F의 error_code에 exception.h의 PF_* 매크로 형식으로 들어 있다.
   아래 예제 코드가 이 정보를 읽는 방법을 보여 준다.
   두 가지 모두 5.15절 "Exception and Interrupt Reference"의
   "Interrupt 14--Page Fault Exception (#PF)" 설명에서 더 자세히 볼 수 있다. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* true: 없는 페이지, false: 읽기 전용 페이지에 쓰기. */
	bool write;        /* true: 쓰기 접근, false: 읽기 접근. */
	bool user;         /* true: 사용자 모드에서 접근, false: 커널 모드에서 접근. */
	void *fault_addr;  /* fault가 난 주소. */

	/* fault가 난 주소, 즉 접근해서 fault를 일으킨 가상 주소를 얻는다.
	   코드를 가리킬 수도, 데이터를 가리킬 수도 있다.
	   fault를 일으킨 명령의 주소(그건 f->rip)와 꼭 같지는 않다. */

	fault_addr = (void *) rcr2();

	/* 인터럽트를 다시 켠다. (CR2가 바뀌기 전에 읽으려고 잠깐 꺼 두었을 뿐이다.) */
	intr_enable ();


	/* 원인을 알아낸다. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* For project 3 and later. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* page fault 수를 센다. */
	page_fault_cnt++;

	/* 사용자 프로그램이 잘못된 주소를 건드린 거면 exit(-1)로 그 프로세스만 끝낸다. */
	if (user) {
		thread_current ()->exit_status = -1;
		thread_exit ();
	}

	/* 진짜 fault면 정보를 보여 주고 끝낸다. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}