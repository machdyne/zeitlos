# release/abi

`snapshot.txt` is the interfaces between flash and the card (the
syscalls, their argument structs, the message envelope, the executable
format, the core apps' protocols) as they stood at a point worth
comparing against: written by

    tools/gen_abi.py --snapshot release/abi/snapshot.txt

and refreshed when a release is made. CI's `build` job runs
`tools/gen_abi.py --compare release/abi/snapshot.txt` on every push and
puts what was added, changed and removed since then in the job's
summary.

It is a reference, not a promise: nothing fails because of it. See
docs/freeze.md ("Checking a change") and docs/ci.md.
