# cache_soc_lz4

The OS's own LZ4 decoder (`sw/common/zexec_lz4.c`, which unpacks every
compressed executable) as a program for `rtl/tb/tb_cache_soc.v`. It
unpacks real RV32IM code (a slice of libc.a, `blob.h`, made by
`gen.py` with `tools/mkzar.py`'s encoder) four times -- data cache on
and off, posted writes on and off, an I-cache flush between -- and
checks every byte.

It found the zeitlos32p bug in docs/zeitlos32p.md, "Bugs found after
round 3" (1).

```
make PREFIX=riscv-none-elf-        # or riscv64-unknown-elf-
cd ../../.. && iverilog -g2005 -DCPU_Z32P -DBURST=1 -o tb_lz4 rtl/tb/tb_cache_soc.v \
  rtl/cache_id.v rtl/cache.v rtl/cache_split.v rtl/mpu_split.v rtl/mem/sdram_kianv.v \
  rtl/tb/sdram_model.v rtl/cpu/picorv32/picorv32.v rtl/cpu/zeitlos32/zeitlos32.v \
  rtl/cpu/zeitlos32/zeitlos32_muldiv.v rtl/cpu/zeitlos32/zeitlos32_decode.v \
  rtl/cpu/zeitlos32/zeitlos32_alu.v rtl/cpu/zeitlos32/zeitlos32_lsu.v \
  rtl/cpu/zeitlos32p/zeitlos32p.v rtl/arbiter_main.v
vvp tb_lz4 +prog=rtl/tb/cache_soc_lz4/lz4.hex +expect=099a2a6b
```

`gen.py <file> [bytes]` makes new data; the expected FNV-1a is
`ORIG_FNV` in `blob.h`.
