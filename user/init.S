/* SutureOS: first ring-3 program.
 *
 * Assembled by scripts/build.py into build/init.bin, then turned into an object
 * file whose bytes are linked into the kernel (like the AP trampoline) and
 * executed by kernel/syscall/ring3.c through el0_enter_user_saved().
 *
 * It exercises exactly the things that were broken before: the `syscall`
 * instruction path (LSTAR), the write(1) console path, a kernel-to-user buffer
 * copy (uname), and exit(60) returning to the kernel instead of crashing.
 *
 * ABI: Linux x86-64 syscall numbers, rdi/rsi/rdx/r10/r8/r9 arguments, result in
 * rax (see kernel/syscall/syscall.h).
 */
    .section .text
    .global _start

    .set SYS_write,          1
    .set SYS_uname,          63
    .set SYS_getpid,         39
    .set SYS_exit,           60

_start:
    /* write(1, "hello from ring 3\n", 18) */
    movq    $SYS_write, %rax
    movq    $1, %rdi
    leaq    msg_hello(%rip), %rsi
    movq    $msg_hello_len, %rdx
    syscall

    /* uname(&uts): proves a kernel -> user buffer copy works */
    movq    $SYS_uname, %rax
    leaq    uts_buf(%rip), %rdi
    syscall
    cmpq    $0, %rax
    jne     skip_uname
    /* write(1, "sysname=...", len) - sysname is "Linux" + zeros */
    movq    $SYS_write, %rax
    movq    $1, %rdi
    leaq    msg_sysname(%rip), %rsi
    movq    $msg_sysname_len, %rdx
    syscall
    /* echo the 5 bytes of sysname that the kernel wrote into our buffer */
    movq    $SYS_write, %rax
    movq    $1, %rdi
    leaq    uts_buf(%rip), %rsi
    movq    $5, %rdx
    syscall
    /* newline */
    movq    $SYS_write, %rax
    movq    $1, %rdi
    leaq    msg_nl(%rip), %rsi
    movq    $1, %rdx
    syscall
skip_uname:

    /* getpid() must return a small non-negative task slot */
    movq    $SYS_getpid, %rax
    syscall
    movq    %rax, pid_result(%rip)

    /* write(1, "pid<digit>\n", 6) */
    movq    $SYS_write, %rax
    movq    $1, %rdi
    leaq    msg_pid(%rip), %rsi
    movq    $msg_pid_len, %rdx
    syscall
    movq    pid_result(%rip), %rax
    addb    $'0', %al
    movb    %al, pid_digit(%rip)
    movq    $SYS_write, %rax
    movq    $1, %rdi
    leaq    pid_digit(%rip), %rsi
    movq    $1, %rdx
    syscall
    movq    $SYS_write, %rax
    movq    $1, %rdi
    leaq    msg_nl(%rip), %rsi
    movq    $1, %rdx
    syscall

    /* exit(42) - the kernel must regain control and report the code */
    movq    $SYS_exit, %rax
    movq    $42, %rdi
    syscall

    /* not reached */
    movq    $SYS_exit, %rax
    movq    $1, %rdi
    syscall
1:  jmp 1b

    .section .rodata
msg_hello:
    .ascii "hello from ring 3\n"
    .set msg_hello_len, . - msg_hello
msg_sysname:
    .ascii "uname sysname="
    .set msg_sysname_len, . - msg_sysname
msg_pid:
    .ascii "pid="
    .set msg_pid_len, . - msg_pid
msg_nl:
    .ascii "\n"

    .section .bss
    .balign 16
uts_buf:
    .space 390                       /* sizeof(struct linux_utsname) */
pid_result:
    .space 8
pid_digit:
    .space 8

    .section .note.GNU-stack,"",@progbits
