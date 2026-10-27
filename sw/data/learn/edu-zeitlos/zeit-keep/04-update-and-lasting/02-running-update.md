# Running update

```lesson
(objective "See which release the card has, see what an update would change, run it, and finish it afterwards.")
(minutes 20)
```

You will learn to bring your card up to date from the console.

update fetches releases over the internet, so the computer must be
connected to it, by cable or however your board connects. Open
**term** from the dock and press **CONSOLE**. First, see what the card
has:

```
> run update
```

It shows the card's release, such as `zeitlos 0.0.7`, and its app sets.

```sources
(source "docs/update.md, On the board: update")
(source "docs/console.md")
```

```check
(text
  (ask "Type the command, at the console, that shows which release the card has.")
  (accept "run update")
  (wrong-if "update" "At the console a program is started with run: run update.")
  (wrong-if "run update latest" "That one updates the card. With nothing after it, update only shows what the card has.")
  (hint "run, and the app's name, and nothing else."))
```

---

## Look first

Before an update, make a backup: your files are kept by update, but a
backup is how you get anything back.

Then ask what it would do. `-n` changes nothing:

```
> run update -n latest
```

`latest` means the newest release Machdyne has published. It lists what
it would write and remove.

```check
(choice
  (ask "What does run update -n latest change on the card?")
  (right "Nothing: it only says what an update would change")
  (wrong "Only the programs"
    "-n changes nothing at all. It is a look ahead.")
  (wrong "Everything, but more slowly"
    "-n writes nothing. Leave it out to update.")
  (hint "-n is the safe way to look first."))
```

```sources
(source "docs/update.md, On the board: update")
```

---

## Update

```
> run update latest
```

It checks the signature, compares the card with the release, then
fetches each file that differs, with its progress, and ends by saying
how many files it wrote and removed. Run again, it says `up to date`.

Programs that were running still use their old files: restart them, or
simply restart the computer.

```check
(text
  (ask "Type the command, at the console, that brings the card up to the newest release.")
  (accept "run update latest")
  (wrong-if "update latest" "At the console a program is started with run: run update latest.")
  (wrong-if "run update -n latest" "With -n it only shows what would change. Leave -n out to update.")
  (wrong-if "run update" "That only shows what the card has. Add which release: latest.")
  (hint "run update, and the word for the newest release."))
```

```sources
(source "docs/update.md, On the board: update")
```

---

## Stopped part way

An update is safe to stop at any moment. Each file is fetched beside
the old one, checked, and only then put in its place. Running the same
command again finishes the job, using what was already fetched.

A file that a program has open for writing is left; update says which,
and the next run puts it in place.

```check
(choice
  (ask "An update stops half way, because the internet connection went down. What do you do when it is back?")
  (right "Run the same update again: it finishes the job")
  (wrong "Write a new card: the card is now half updated"
    "Each file is put in place only once it is whole, so the card is fine. Running update again finishes it.")
  (wrong "Nothing: update carries on by itself"
    "update does not wait in the background. Run it again.")
  (hint "update was made so that it is safe to stop and run again."))
```

```sources
(source "docs/update.md, How it is safe to stop and Files in use")
```

---

## More sets, or an older release

`-s` adds app sets: `run update latest -s games` brings the card up to
date and adds the games. `-s all` adds every set.

Naming a release instead of `latest` goes to that release, even an
older one: `run update v0.0.6` goes back, while Machdyne still offers
it. Your files stay, though an older app may not read something a newer
one saved.

```check
(choice
  (ask "Your card has only the base set. You want the games as well. Which command?")
  (right "run update latest -s games")
  (wrong "run update games"
    "games is not a release. Name the release, then add the set with -s.")
  (wrong "run update -n latest -s games"
    "With -n nothing is written: it only shows what adding the games would do.")
  (hint "Sets are added with -s, after the release."))
```

```sources
(source "docs/update.md, Sets and Going back")
```

---

```example
(example
  (problem "Bring a card up to date safely.")
  (step "Make a backup, and Remove drive.")
  (step "Open term, press CONSOLE, type run update -n latest, and read what would change.")
  (step "Type run update latest and wait for it to say it is done.")
  (step (choice (ask "Some programs were running during the update. What now?")
          (right "Restart them, or restart the computer")
          (wrong "Nothing: they are already new"
            "Programs that were running still use the old files until they start again.")))
  (answer "The card is up to date, and your files are as they were."))
```

So: `run update` to see, `-n` to look first, `run update latest` to
update, and the same command again to finish one that stopped.
