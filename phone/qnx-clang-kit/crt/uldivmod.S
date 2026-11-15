@ Same as libgcc 4.4 bpabi.S (as found in the qcc-built workers): quotient via
@ libc's __gnu_uldivmod_helper(a, b, &rem); remainder returned in r2:r3.
    .syntax unified
    .arm
    .text
    .global __aeabi_uldivmod
    .hidden __aeabi_uldivmod
    .type __aeabi_uldivmod, %function
__aeabi_uldivmod:
    sub   sp, sp, #8
    push  {sp, lr}
    bl    __gnu_uldivmod_helper
    ldr   lr, [sp, #4]
    add   sp, sp, #8
    pop   {r2, r3}
    bx    lr
    .size __aeabi_uldivmod, .-__aeabi_uldivmod
