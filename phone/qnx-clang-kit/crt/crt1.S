@ QNX 6.5 armle-v7 program entry, mirroring the sequence in qcc-built binaries:
@ _init_libc(argc, argv, envp, auxv, exitfunc); _preinit_array; atexit(_fini);
@ _fini_array; _init; _init_array; errno = 0; exit(main(argc, argv, envp)).
    .syntax unified
    .arm
    .text
    .global _start
    .type _start, %function
_start:
    mov   r4, r0              @ exit function passed by ldqnx
    ldr   r0, [sp]            @ argc
    add   r1, sp, #4          @ argv
    add   r2, r1, r0, lsl #2
    add   r2, r2, #4          @ envp
    mov   r3, r2
1:  ldr   r12, [r3], #4       @ skip env to reach auxv
    teq   r12, #0
    bne   1b
    str   r4, [sp, #-4]!      @ 5th argument on the stack
    mov   r4, r0
    mov   r5, r1
    mov   r6, r2
    bl    _init_libc
    add   sp, sp, #4
    ldr   r0, =__preinit_array_start
    ldr   r1, =__preinit_array_end
    bl    _preinit_array
    ldr   r0, =_fini
    bl    atexit
    ldr   r0, =__fini_array_start
    ldr   r1, =__fini_array_end
    bl    _fini_array
    bl    _init
    ldr   r0, =__init_array_start
    ldr   r1, =__init_array_end
    bl    _init_array
    ldr   r0, =errno
    mov   r1, #0
    str   r1, [r0]
    mov   r0, r4
    mov   r1, r5
    mov   r2, r6
    bl    main
    bl    exit
2:  b     2b
    .size _start, .-_start

    .global _init
    .type _init, %function
_init: bx lr
    .global _fini
    .type _fini, %function
_fini: bx lr

