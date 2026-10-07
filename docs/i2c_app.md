# i2c -- I2C from the posix shell

Talks to I2C devices on a PMOD port's pins, or to parts on a bench bus
([bench.md](bench.md)), the same way for both.

```
$ i2c scan                              # PMOD port 0
0x20
$ i2c -b main write 0x20 6 0x00         # a bench bus
$ i2c -b main write 0x20 0 read 2
aa ff
```

| Command | Does |
|---|---|
| `i2c [-b BUS] buses` | the buses there are: PMOD ports, and the bench's |
| `i2c [-b BUS] scan` | the addresses that answer, 0x08-0x77 |
| `i2c [-b BUS] read ADDR N` | N bytes from ADDR |
| `i2c [-b BUS] write ADDR BYTE...` | bytes to ADDR |
| `i2c [-b BUS] write ADDR BYTE... read N` | bytes, then N back, with a repeated start between: how almost every device's registers are read |

`BUS` is `pmod0` (the default) to `pmodN`, or the name of a bus in the
running bench's netlist. On a PMOD port, pin 1 is SCL and pin 2 SDA (GPIO
bits 0 and 1, [gpio.md](gpio.md)), as a Wolfszahn wires a module.

Numbers are decimal, or hex with `0x`. At most 64 bytes each way. The
exit status is 0; 1 if the device did not answer; 2 for a usage error, or
a bus that is not there.

It is built on zi2cx ([i2c.md](i2c.md)), and prints through the shell's
output relay, as `zcc` does.
