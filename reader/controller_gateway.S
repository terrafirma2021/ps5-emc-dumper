/* Separate owner-gated getppid controller; no task callback enters this page
 * except through its fixed marker target while the page remains resident. */
#include "task_probe.h"
.section .text.gateway,"ax",@progbits
.global tg_controller_gateway
.type tg_controller_gateway,@function
tg_controller_gateway:
    leaq tg_controller_state(%rip), %r11
    movq %rdi, TG_CTRL_LAST_TD(%r11)
    cmpq %rdi, TG_CTRL_OWNER(%r11)
    jne tg_controller_fallback
    movl $TG_CTRL_ARMED, %eax
    movl $TG_CTRL_ACTIVE, %ecx
    lock cmpxchgq %rcx, TG_CTRL_PHASE(%r11)
    jne tg_controller_fallback
    movq TG_CTRL_REQUEST(%r11), %rdx
    subq $8, %rsp
    callq *TG_CTRL_SERVICE(%r11)
    addq $8, %rsp
    movq %rax, %r10
    leaq tg_controller_state(%rip), %r11
    movl $TG_CTRL_ACTIVE, %eax
    movl $TG_CTRL_IDLE, %ecx
    lock cmpxchgq %rcx, TG_CTRL_PHASE(%r11)
    je tg_controller_return
    cmpq $TG_CTRL_DRAINING, %rax
    jne tg_controller_return
    movl $TG_CTRL_CLOSED, %ecx
    lock cmpxchgq %rcx, TG_CTRL_PHASE(%r11)
tg_controller_return:
    movq %r10, %rax
    retq
tg_controller_fallback:
    jmpq *TG_CTRL_FALLBACK(%r11)

.org 0x100, 0x90
.global tg_controller_service
tg_controller_service:
    .incbin "controller_service.bin"
.org TG_CONTROLLER_REQUEST_OFFSET, 0x90
.global tg_controller_request
tg_controller_request:
    .zero 0x100
.org TG_CONTROLLER_TASK_OFFSET, 0x90
.global tg_controller_task
tg_controller_task:
    .zero 0x20
.org TG_CONTROLLER_STATE_OFFSET, 0x90
.global tg_controller_state
tg_controller_state:
    .zero 0x40
.section .note.GNU-stack,"",@progbits
