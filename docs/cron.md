# cron -- programs on a schedule

`sw/apps/cron`. Runs programs at set times -- nightly backups, an
hourly sync -- from a list in **`/user/cron.cfg`**. A core app, in
flash; `init` starts it at boot when that file exists, and it sleeps
between jobs.

```
# /user/cron.cfg
wait_for_ntp: yes

daily 03:00          backup /docs
weekly mon 02:30     backup --full
monthly 1 04:00      tidy
hourly :15           sync
every 30m            check-mail
at boot              mount-usb
daily 05:00          text 'My Notes.txt'
```

## Rules

One per line: *when*, then the program and its arguments. `#` starts a
comment.

| when | runs |
|---|---|
| `daily HH:MM` | every day at that time |
| `weekly DAY HH:MM` | once a week -- `sun` `mon` `tue` `wed` `thu` `fri` `sat` |
| `monthly N HH:MM` | on day N of the month (a month without day N is skipped) |
| `hourly :MM` | every hour, at that minute |
| `every N` + `s` `m` `h` or `d` | every so long, counted from when cron started (10 seconds at least) |
| `at boot` | once, when cron starts |

Times are **local**: `system.rtc.timezone` from `/zeitlos.cfg`
([rtc.md](rtc.md)), summer time included. In the spring, a job set in
the hour that is skipped is missed that day -- a daily job then catches
up (below); in the autumn, a job in the hour that happens twice runs
once.

The program and arguments are split the way the posix shell splits them
([posix.md](posix.md), "Quoting"), so `'My Notes.txt'` is one argument,
and passed on the way `run` passes them: the program gets its arguments
as its launch argument. A line cron cannot read is reported on the
console with its line number, and the rest of the file still works.

## Settings

| setting | default | |
|---|---|---|
| `wait_for_ntp: yes` or `no` | `yes` | wait for the clock before doing anything |

**The clock.** No board has a battery for its clock ([rtc.md](rtc.md)):
after power-up the time is unknown until `net` sets it from NTP, and a
board with no network may never know it. With `wait_for_ntp: yes` cron
does **nothing at all** until the clock is valid -- not even `at boot`
jobs. With `no` it starts at once: `at boot` and `every` jobs need no
clock, and daily, weekly, monthly and hourly jobs run whenever the clock
is valid, which on an offline machine may be never. That is the setting
for a board without networking (`obst`), with care.

## What it does, and does not do

- **A job still running is not started again.** The skip is logged.
- **Catching up.** A daily, weekly or monthly job whose last run is more
  than its period (plus an hour) ago runs once, as soon as cron is
  running with a valid clock -- the machine was off, or not yet synced,
  at the time. Hourly and `every` jobs do not catch up: a machine off
  for a day should not run 24 syncs at once. When each job last ran is
  kept in **`/user/cron.state`**, one line per job, keyed by a hash of its
  rule's line -- so editing a rule starts its history over.
- **The log.** One line per start, skip, failed start and exit in
  **`/user/cron.log`**, and on the console:

  ```
  2026-09-25 03:00  started backup /docs
  2026-09-25 03:04  exited 0: backup /docs
  ```

  Without a valid clock the time is `boot+SECONDSs`. The log is cut back
  to nothing when it passes 16KB.
- **Changes.** `cfg reload` (or `settings`' Reload) re-reads this file
  too; a job whose line did not change keeps its history. A new file
  needs cron started -- `run cron`, or reboot.
- **Only programs.** A job runs a program, like `run` does. The posix
  shell's commands -- `cp`, `rm` and the rest -- are builtins of the
  shell, not programs, so a job cannot run them directly.
- **One cron.** It registers as `cron0`; a second copy exits at once.

## Size

About 22KB, in flash with `wm`, `net`, `term` and `console`
([flash_apps.md](flash_apps.md)). It formats its own output rather than
linking stdio, and its RAM is its stack plus about 12KB of job table and
buffers. It wakes at most once a minute, at the minute, or sooner for an
`every` job.

## Tests

`sw/apps/cron/tests/sched.c` runs the real `cron.c` against a fake
clock, uptime, filesystem and program starts:

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/cron_sched \
   sw/apps/cron/tests/sched.c sw/common/zrtc.c
/tmp/cron_sched
```

It covers parsing (and bad lines), daily, weekly, monthly and hourly
jobs, a job still running, catching up once, state surviving a restart,
local time in Berlin, the repeated hour when summer time ends, `at boot`
and `every` with no clock, `cfg reload`, and a program that will not
start.
