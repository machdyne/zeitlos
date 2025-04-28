.section .text
.globl _start
.type _start, @function

_start:
.option norelax
    # get PC-relative base
    auipc t0, 0           # t0 = base

    # set global pointer
0:  auipc gp, %pcrel_hi(__global_pointer$)
    addi  gp, gp, %pcrel_lo(0b)

    # copy .data from _etext -> _sdata
    la    t1, _sdata
    la    t2, _edata
    la    t3, _etext

    sub   t1, t1, t0      # offset _sdata
    sub   t2, t2, t0      # offset _edata
    sub   t3, t3, t0      # offset _etext

    add   a0, t0, t1      # dst = _sdata
    add   a1, t0, t2      # end = _edata
    add   a2, t0, t3      # src = _etext

1:
    beq   a0, a1, 2f
    lw    t4, 0(a2)
    sw    t4, 0(a0)
    addi  a0, a0, 4
    addi  a2, a2, 4
    j     1b

2:
    # zero .bss
    la    t1, _sbss
    la    t2, _ebss
    sub   t1, t1, t0
    sub   t2, t2, t0

    add   a0, t0, t1
    add   a1, t0, t2

3:
    beq   a0, a1, 4f
    sw    zero, 0(a0)
    addi  a0, a0, 4
    j     3b

4:
    # set stack pointer
    li    t1, 0x2000      # 8 KB stack
    add   sp, t0, t1

    #la a0, __libc_fini_array
    #call atexit
    call __libc_init_array

    lw a0, 0(sp)								# a0 = argc
    addi a1, sp, 4							# a1 = argv
    li a2, 0     
    call  main

    # main returned, exit gracefully
    li    a0, 0
    call  exit


  .global _init
  .type   _init, @function
  .global _fini
  .type   _fini, @function

_init:
_fini:
  ret
  .size  _init, .-_init
  .size  _fini, .-_fini
