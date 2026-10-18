/* Resident task-class gateway. Unknown identities use the original native
 * body; the one closed owned identity returns without touching its context. */
#include "task_probe.h"
.section .text.gateway,"ax",@progbits
.global tg_task_gateway
.type tg_task_gateway,@function
tg_task_gateway:
    leaq tg_task_state(%rip), %r11
    cmpq %rdi, TG_TASK_IDENTITY(%r11)
    jne tg_unknown
    testq %rdi, %rdi
    je tg_unknown
tg_claim:
    movq TG_TASK_ADMISSION(%r11), %rax
    testq %rax, %rax
    js tg_owned_closed
    leaq 1(%rax), %rcx
    lock cmpxchgq %rcx, TG_TASK_ADMISSION(%r11)
    jne tg_claim
    subq $8, %rsp
    callq *TG_TASK_TARGET(%r11)
    addq $8, %rsp
    leaq tg_task_state(%rip), %r11
    lock decq TG_TASK_ADMISSION(%r11)
    retq
tg_owned_closed:
    retq
tg_unknown:
    jmpq *TG_TASK_FALLBACK(%r11)

.org 0x100, 0x90
.global tg_task_emc_query
.type tg_task_emc_query,@function
tg_task_emc_query:
    leaq tg_task_state(%rip), %rax
    movq %rdi, TG_TASK_SEEN_IDENTITY(%rax)
    movslq %esi, %rcx
    movq %rcx, TG_TASK_PENDING(%rax)
    movq %rsp, TG_TASK_STACK(%rax)
    xorl %ecx, %ecx
    movw %cs, %cx
    movq %rcx, TG_TASK_CS(%rax)
    pushq %rbx
    movq %rax, %rbx
    leaq tg_task_message(%rip), %rdi
    movq %rdi, %rsi
    callq *TG_TASK_ICC_QUERY(%rbx)
    movslq %eax, %rcx
    leaq tg_task_state(%rip), %rax
    movq %rcx, TG_TASK_MARKER(%rax)
    lock incq TG_TASK_CALLS(%rax)
    popq %rbx
    retq

.org TG_TASK_MESSAGE_OFFSET, 0x90
.global tg_task_message
tg_task_message:
    .zero TG_TASK_MESSAGE_SIZE

.org TG_TASK_STATE_OFFSET, 0x90
.global tg_task_state
tg_task_state:
    .zero 96
.section .note.GNU-stack,"",@progbits
