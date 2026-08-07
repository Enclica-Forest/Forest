/* Forest-OS userspace C runtime startup */
/* Provides _start entry point that calls main() */

    .text
    .globl _start

_start:
    /* Stack already has argc, argv, envp from kernel */
    /* argc is at [esp] */
    /* argv is at [esp+4] */
    /* envp is at [esp+4+argc*4] */
    call main
    /* main returned - exit with return value */
    pushl %eax
    call exit
    /* Should never reach here */
    hlt
