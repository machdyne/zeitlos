# zwalk: find, copy and delete a tree, a step at a time

`sw/common/zwalk.[ch]`. Walks a folder tree for three jobs -- **find**
(by name, by text inside files, or both), **copy**, **delete** -- in
steps small enough that the app doing it never stops answering, and
can stop the walk at once. `files` uses it for Find and for copying,
moving and deleting folders ([file_browser.md](file_browser.md)).

## The rule: one step, one filesystem call

`zw_step()` does at most one call to the filesystem driver and
returns:

| step | the call |
|---|---|
| a folder is reached | list it (`fs_list_into()`) |
| a file, for copy | open it and its copy; then each step reads a 2KB chunk and writes it |
| a file, for a text find | open it; then each step reads a 2KB chunk and looks in it |
| a folder, for copy | make it at the destination |
| an entry, for delete | delete it (a folder once it is empty) |
| a name, for a name find | none -- the listing is already here |

The app calls it from its event loop. `files` gives it a 20ms slice,
then reads its messages again, and does not sleep while a walk has
work:

```c
if (walking) {
	uint32_t t0 = z_uptime_ticks();
	while (walking && z_uptime_ticks() - t0 < Z_TICK_HZ / 50) {
		int r = zw_step(&walk);
		if (r == ZW_HIT) add_result(walk.hit);
		else if (r == ZW_DONE || r == ZW_FAIL) finish(r);
	}
	continue;		// back to the messages
}
```

So a key press waits at most for the slice in progress and the one
call inside it: a few milliseconds on an SD card, never "the rest of
this file". **Stopping is `zw_cancel()`**, and takes effect before
the next step: it closes what is open, and the file a copy was
writing is deleted -- half a file looks like a whole one. What was
finished before stays, and `zwalk_t.partial` says whether there is any.

## Memory

The caller hands `zw_start()` one block; nothing is allocated. It holds
the 2KB chunk buffer, and the listing of the folder being walked (its
names and their types). The depth is a fixed stack in `zwalk_t`: 16
folders below the start (`ZW_DEPTH`); deeper ones are counted in
`skipped` and not entered.

Only one listing is held at a time. When the walk comes back up from
a folder it lists the folder above again -- one more call per folder,
in exchange for memory that does not grow with depth. Find and copy
pick up at the entry after the one they went into: the folder has not
changed (copy refuses a destination inside its source). Delete starts
the folder over, since everything before is gone.

A folder whose listing does not fit the block is walked as far as it
fits, and `zwalk_t.truncated` says so; delete lists such a folder again
until it is empty, so it always finishes. 4KB walks folders of about a
hundred entries, 24KB any folder the file list shows.

`files` takes a 64KB arena while a walk runs (and while find's results
are up) and gives it back after: 24KB for the walk, the rest for
results. On a board where the kernel refuses an arena it uses a 6KB
block of its own, which is enough for the walk and about eighty
results.

## Find

```c
zw_start(&w, ZW_FIND, "/home", NULL, mem, cap);
zw_find_set(&w, "*.md", "");			// name, text
```

The name is a glob when it has `*` or `?`, else a part of the name;
case-insensitive either way, and `""` matches everything. Folders whose
names match are results too (for a name-only find). The text is looked
for inside files, ASCII case folded, across chunk boundaries (the last
63 bytes of one chunk are kept in front of the next). With both, a
file's name must match before it is read. A file that cannot be read
is counted in `skipped`, and the find goes on.

Each match is a `ZW_HIT`, with the path in `w.hit`; call again to go on.

## Copy

```c
zw_start(&w, ZW_COPY, "/home/photos", "/usb/photos", mem, cap);
```

A file or a folder, to a destination that must not exist
(`ZW_E_EXISTS`: the caller decides about replacing, as `files` asks
before deleting a file it will replace). A folder is refused inside
itself (`ZW_E_INSIDE`). Empty folders are copied. A failed read or
write ends the walk with `ZW_FAIL` and `w.err`, `w.hit` naming the
file, and the half-written file deleted.

A move between volumes is a copy and then a delete of the source, two
walks: the delete starts only once the copy is complete.

## Delete

```c
zw_start(&w, ZW_DELETE, "/home/old", NULL, mem, cap);
```

A file, or a folder and everything in it, the folder itself last. An
entry that will not go (a file open for writing elsewhere) ends it with
`ZW_E_DELETE`, naming it; what was deleted before it is gone.

## Testing

`sw/common/tests/test_zwalk.c` runs all three over a real tree in
`/tmp`, through a host version of the filesystem calls that answers as
the kernel does (an empty folder lists as a failure, names come back
as full paths, listings are cut short at the caller's limits). It
checks that a step makes at most one call (two for a copy chunk), that
a text match across a chunk boundary is found, that a delete with a
tiny block still empties a 300-file folder, and it **stops a copy after
every one of its steps** in turn: nothing left open, no half file, and
`partial` true exactly when something is left.

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/test_zwalk \
   sw/common/tests/test_zwalk.c sw/common/zwalk.c && /tmp/test_zwalk
```

`sw/apps/files/tests/test_walk.c` (`make test` in `sw/apps/files`)
drives `files.c` itself: Find with results and Esc, folder copy,
copy into a folder, replacing a file, stopping part-way, move across
volumes, move within one, deleting a folder.

Board checks for the final phase: a find over the whole card while
typing in another window; Esc during a text find over a large file
(the time to stop); a 5MB copy to `/ram` stopped half-way (no half
file); deleting a folder of a few hundred files; a find on a board
without an arena.
