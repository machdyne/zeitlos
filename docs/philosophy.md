# Zeitlos philosophy

This document describes the values on which Zeitlos is built. They
guide decisions about the system and should be considered whenever it
is changed. The book [Timeless Computing](tc.md) explains how a
computer can be built to last; this document describes what that
means for Zeitlos in particular.

## Timeless

The system and its applications are designed to be useful in the
future, the present and the past.

Zeitlos is intended for tasks that people have performed with computers
for decades and will continue to perform: reading, writing, keeping
records, calculation, communication, organization, education, the
control of machines, and programming
([tc.md](tc.md#timeless-applications)). A person a century from now,
given power and the documentation, should be able to learn to use it,
as should a person from a century ago.

The system does not depend on anything that may cease to exist. It does
not require a server, an account or an online service, and it stores
information in formats that will remain readable.

## Responsive

The system and its applications respond to the user at all times. An
operation may take a long time, but it must display its progress and
must stop immediately when the user cancels it, leaving the system as
it was before the operation began.

In practice, an application answers every key press, mouse click and
message from the window manager without delay, whatever else it is
doing. Its main loop never makes a call that takes longer than about
20 milliseconds ([responsive.md](responsive.md)).

## Tool

The system is a tool. It does not create work for the user, and it does
not interrupt the user.

It acts only when asked. It does not update itself, change the behavior
of an application, or require maintenance that the user did not choose.
It does not display messages over the user's work unless they result
from something the user did. Once the user has learned an application,
the application continues to work in the same way.

## Accessible

The system and its applications can be used with a keyboard alone, a
mouse alone, a gamepad alone, a remote shell, or any combination of
these.

No method of input is treated as secondary. An on-screen keyboard
serves a machine without a keyboard ([keyboard_app.md](keyboard_app.md)).
Speech and captions allow the system to be used without the screen
([tts.md](tts.md), [captions.md](captions.md)), and remote desktop and
the shells allow it to be used from another machine. Text is
international: the system uses UTF-8 and supports many keyboard layouts,
accented letters and Japanese ([keyboard_layouts.md](keyboard_layouts.md)).

## Trusted

Zeitlos is a single-user system. The gateware, firmware, kernel,
applications and data together form one trusted system, which belongs
to its user. The system protects against accidents, not against
malicious code.

The memory protection unit prevents an application's stray write from
reaching the kernel or another application ([mpu.md](mpu.md)). It is a
protection against errors, not a security boundary.

The user's work receives the most protection. The system does not
discard work without asking, and if the user chooses to save and the
save fails, the action that required it is cancelled. Files are written
completely or not at all: the system writes a temporary file
(`NAME.part`) and renames it only when it is complete. The system writes
to the memory card only when the user has asked it to, because power
may be removed at any time and a card that is being written can be
damaged. Work that is kept for recovery is written to the RAM disk.

Data from outside the system, such as network traffic and files from
other computers, is not part of the trusted system and is checked
before it is used ([security.md](security.md), [tls.md](tls.md)).

## Recoverable

The system can always be returned to a known working state.

The core applications are stored in flash memory, so the computer
starts without a memory card ([flash_apps.md](flash_apps.md)). Device
drivers run as separate processes and are restarted if they fail
([userspace_drivers.md](userspace_drivers.md)). Settings are kept in a
text file ([config.md](config.md)). An update replaces programs and
leaves the user's files and settings unchanged ([update.md](update.md)).
A release is built from a known version of the source and checked
before it is published; a release that does not meet its timing
requirements is not published as if it did ([releases.md](releases.md)).

## Fallback

Where possible, a function that is implemented in hardware is also
implemented in software, so that the system continues to work on
hardware that lacks it.

A board built without the caches or the cryptographic blocks
([SHA-256](sha256_hw.md), [Keccak](keccak_hw.md) and the
[Montgomery multiplier](montmul.md)) performs the same work in software,
more slowly. Software determines which hardware is present by reading
the capability registers ([csrs.md](csrs.md)). Where a fallback does not
yet exist, the documentation says so; for example, software built for
RV32IM requires the CPU's multiply instruction ([muldiv.md](muldiv.md)).

The same principle applies to the rest of the computer. Without a
memory card, with only 1 MB of memory, without a network or without a
mouse, the system does less, tells the user what is missing, and
continues to work.

## Apps

Applications cost the system almost nothing: only compilation time on
the host computer and space on the memory card. A system is not useful
without useful applications.

Each application performs one task well, in the manner of the useful
programs of the 1980s, such as a text editor, a spreadsheet, a file
browser or a terminal. It starts quickly and needs little memory. A new
kind of task is served by a new application rather than by enlarging an
existing one. Applications share the code that all of them need, such
as windows, dialogs, file formats, images, printing and help, so each
new application is small ([build.md](build.md)).

The core applications are an exception. Because they are stored in
flash memory, which is limited, each has a size budget, and exceeding a
budget requires a deliberate decision ([flash_apps.md](flash_apps.md)).

## Modular

Parts of the system are isolated where possible, and code is shared
where sharing is useful. A modular system can be understood one part at
a time before it is understood as a whole.

The kernel schedules processes, manages memory and passes messages. The
filesystem, the USB stack and the network run as separate processes,
and applications communicate with them through messages
([kernel.md](kernel.md), [messaging.md](messaging.md)). Each hardware
block has one function and a documented register interface. Code is
shared when several parts need the same work done in the same way.

## Simple

No part of the system is more complex than its purpose requires.

The hardware is written in plain Verilog and the software in C, in a
form that can be read without difficulty. An error is corrected at its
cause rather than worked around. It is possible for one person to
follow a key press from the USB connection to the character on the
screen, and the system should remain so.

## Extendable

The needs of the future cannot be predicted, but the system can be made
easy to extend and to connect to interfaces that do not yet exist.

The computer is extended with modules: PMOD sockets, general-purpose
I/O with stream engines that can carry a protocol on any pin, I2C, SPI,
USB, and a link to an ESP32 ([gpio.md](gpio.md),
[usb_host.md](usb_host.md)). New programs can be written on the
computer itself, in Scheme, BASIC or C ([repl.md](repl.md),
[basic_app.md](basic_app.md), [zcc.md](zcc.md)).

The system also works with other computers. It reads and writes their
document formats ([convert.md](convert.md)), uses standard network
protocols, prints to ordinary printers without special drivers
([print.md](print.md)), and uses a memory card that any PC can read.
Nothing a user creates is confined to Zeitlos.

## Honest

The system reports what is actually happening. An error message states
what went wrong and what remains. A control that cannot be used
appears unavailable, and a list that is incomplete says so. Tests are
written so that they fail when the code is wrong, and are checked to
confirm that they do.

## Docs

Everything is documented, for users and for developers. The
documentation explains how to use the system and how to change it.

The computer contains its own documentation: each application has a
short help screen (F1), the manuals are on the memory card, and the
welcome page is in flash memory ([app_help.md](app_help.md)). Each
design decision is recorded with its reason, both in the code it
concerns and in these documents, including the alternatives that were
considered and rejected. The designs, software and documentation are
published in more than one place, so that they outlast any single copy.

## License

Anyone may use the system for their own purposes, modify it, or sell it
as part of a product, with minimal restrictions
([LICENSE.md](../LICENSE.md)).

Openness supports the other values. A design that can be read can be
checked, repaired and rebuilt by others, even after its original
makers are gone.

## Conflicting values

These values sometimes conflict. In general:

- Responsiveness and the protection of the user's work take priority.
  A feature that makes the system unresponsive or puts the user's work
  at risk is not complete.
- Simplicity takes priority over completeness. A smaller feature that
  always works is preferred to a larger one that usually works.
- A software fallback takes priority over speed. Hardware that makes
  the system faster is welcome, but hardware that the system cannot
  work without is a cost to every board that lacks it.
