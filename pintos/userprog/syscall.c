#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "intrinsic.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/synch.h"
#include "filesys/filesys.h"

void syscall_entry (void);
static void check_address (const void *addr);
static void check_buffer (const void *buffer, size_t size);
static void check_string (const char *str);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

/* 파일 시스템은 동기화가 안 되어 있으므로, 
파일 시스템 함수 호출을 감싸는 전역 lock. */
static struct lock filesys_lock;

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init (&filesys_lock);
}

/* 사용자가 넘긴 주소가 안전한지 검사한다.
   NULL, 커널 주소, 매핑되지 않은 주소면 프로세스를 exit(-1)로 끝낸다.
   TODO: Project 3에서 lazy loading이 들어가면 수정이 필요하다. */
static void 
check_address (const void *addr) {
	if (addr == NULL || !is_user_vaddr (addr) || pml4_get_page(thread_current ()->pml4, addr) == NULL) {
		thread_current ()->exit_status = -1;
		thread_exit ();
	}
}

/* BUFFER부터 SIZE바이트가 걸친 모든 페이지를 페이지마다 한 번씩 검사한다. */
static void 
check_buffer (const void *buffer, size_t size) {
	if (size == 0)
		return;

	uintptr_t start_page = (uintptr_t) pg_round_down (buffer);
	uintptr_t end = (uintptr_t) buffer + size - 1;

	for (uintptr_t page = start_page; page <= end; page += PGSIZE) {
		check_address ((const void *) page);
	}
}

/* 문자열 STR을 '\0'까지 따라가며, 첫 글자와 새 페이지에 들어설 때마다 검사한다. */
static void 
check_string (const char *str) {
	const char *p = str;

	while (true) {
		if (p == str || pg_ofs (p) == 0) {
			check_address (p);
		}

		if (*p == '\0') {
			break;
		}

		p++;
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	switch (f->R.rax) {
		case SYS_HALT:
			power_off ();
			break;
		case SYS_EXIT:
			thread_current ()->exit_status = (int) f->R.rdi;
			thread_exit ();
			break;
		case SYS_WRITE: {
			int fd = (int) f->R.rdi;
			const char *buffer = (const char *) f->R.rsi;
			size_t size = (size_t) f->R.rdx;

			check_buffer (buffer, size);
	
			if (fd == 1) {
				putbuf (buffer, size);
				f->R.rax = size;
			} else {
				f->R.rax = -1;
			}
			break;
		}
		case SYS_CREATE: {
			/* 인자 꺼내기 */
			const char *file = (const char *) f->R.rdi;
			unsigned initial_size = (unsigned) f->R.rsi;

			/* lock 잡기 전에 파일 이름 검사 */
			check_string (file);

			/* lock 잡기 */
			lock_acquire (&filesys_lock);

			/* 파일 만들고 결과를 변수에 저장 */
			bool result = filesys_create (file, initial_size);

			/* ⑤ lock 풀기 */
			lock_release (&filesys_lock);

			/* ⑥ 결과 돌려주기 */
			f->R.rax = result;

			break;
		}
		default:
			thread_exit ();
			break;
	}
}