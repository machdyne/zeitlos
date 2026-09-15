# Settings every pack shares. Included, not duplicated.
#
# A pack that wants different dimensions can override any of these; a
# pack that overrides the ENCODER should expect its results to be fused
# with the others by rank rather than by score, since two encoders do
# not share a vector space. See docs/ask_app.md, "Packs".

fine_dim    = 128
coarse_dim  = 32
vocab       = 4096
chunk_chars = 1800
encoder     = bow

# numeric | long. FatFs here is FF_USE_LFN 0, so `numeric` is the only
# one the device can open today. `long` exists so that turning LFN on
# later -- and getting the Codex's real topic names -- is a recipe edit
# and a rebuild, not a redesign.
naming      = numeric

# Warn about documents big enough that jumping into them is slow.
# read.c indexes lazily and its frontier only moves forward, so opening
# at byte N streams N bytes off the card first.
seek_warn   = 524288

# Which local model `ask genq` asks for questions, so the name lives in
# one place rather than in a shell command somebody has to remember.
# Override per run with `--model`.
#
# A small NON-REASONING model is the right choice here and the choice
# matters more than it looks. The task is "read this and say what it
# answers" -- comprehension, not reasoning -- and there are ~10,000
# passages, so throughput dominates. The qwen3 family, deepseek-r1 and
# openthinker emit a <think> block of thousands of tokens before
# answering, which is minutes per passage instead of seconds.
genq_model = llama3.2:latest

# Contrastive training epochs, when `encoder = trained`.
#
# Four is measured, not guessed, and too many is actively harmful:
# at 30 the encoder overfits its training pairs and gold-set recall
# drops BELOW the untrained baseline. Find the right number for a
# given corpus with
#
#     ./tools/ask/ask train <recipe> --sweep 1,2,4,8,16
#
# which trains and evaluates each and prints the best.
train_epochs = 4

repo = ark https://github.com/machdyne/ark
