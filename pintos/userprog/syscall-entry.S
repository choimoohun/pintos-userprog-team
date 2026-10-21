/* syscall 명령을 실행하면 여기로 온다.
   커널 스택으로 바꾸고, 사용자 레지스터를 intr_frame 모양으로 쌓은 뒤
   syscall_handler (f)를 부른다. 돌아오면 레지스터를 되돌리고 sysretq로 나간다.
   syscall 명령이 rcx에 돌아갈 주소, r11에 eflags를 넣어 둔다.*/

#include "threads/loader.h"

.text
.globl syscall_entry
.type syscall_entry, @function
syscall_entry:
	movq %rbx, temp1(%rip)
	movq %r12, temp2(%rip)     /* callee saved registers */
	movq %rsp, %rbx            /* 사용자 rsp 저장 */
	movabs $tss, %r12
	movq (%r12), %r12
	movq 4(%r12), %rsp         /* TSS에서 커널 스택 주소 읽기 */
	/* 이제 커널 스택. 아래부터 intr_frame을 뒤 칸부터 쌓는다. */
	push $(SEL_UDSEG)      /* if->ss */
	push %rbx              /* if->rsp */
	push %r11              /* if->eflags */
	push $(SEL_UCSEG)      /* if->cs */
	push %rcx              /* if->rip */
	subq $16, %rsp         /* error_code, vec_no 건너뛰기 */
	push $(SEL_UDSEG)      /* if->ds */
	push $(SEL_UDSEG)      /* if->es */
	/* 여기부터 if->R */
	push %rax              /* 시스템 콜 번호 */
	movq temp1(%rip), %rbx
	push %rbx
	pushq $0               /* rcx 자리 (rip로 이미 저장) */
	push %rdx              /* 3번째 인자 */
	push %rbp
	push %rdi              /* 1번째 인자 */
	push %rsi              /* 2번째 인자 */
	push %r8               /* 5번째 인자 */
	push %r9               /* 6번째 인자 */
	push %r10              /* 4번째 인자 */
	pushq $0 /* skip r11 */
	movq temp2(%rip), %r12
	push %r12
	push %r13
	push %r14
	push %r15
	movq %rsp, %rdi        /* syscall_handler의 인자 f */

check_intr:
	btsq $9, %r11          /* 원래 인터럽트가 켜져 있었는지 (IF 비트) */
	jnb no_sti
	sti                    /* 다시 켠다 */
no_sti:
	movabs $syscall_handler, %r12
	call *%r12
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rsi
	popq %rdi
	popq %rbp
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax              /* 반환값 */
	addq $32, %rsp
	popq %rcx              /* if->rip */
	addq $8, %rsp
	popq %r11              /* if->eflags */
	popq %rsp             
	sysretq

.section .data
.globl temp1
temp1:
.quad	0
.globl temp2
temp2:
.quad	0