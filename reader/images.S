.section .rodata.task_probe_images,"a",@progbits
.global tg_task_image
tg_task_image:
    .incbin "task_gateway.bin"
.global tg_controller_image
tg_controller_image:
    .incbin "controller_gateway.bin"
.section .note.GNU-stack,"",@progbits
