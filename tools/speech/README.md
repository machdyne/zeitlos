# Speech data: `tools/speech`

The data pipeline behind `sw/apps/tts`: the speech pack (a pronunciation
lexicon, letter-to-sound rules and the recorded voice), and the tools
that measure the voice.

**The documentation is [docs/tts_data.md](../../docs/tts_data.md)** --
one copy, kept with the rest of the docs.

Quick start:

```
./speech fetch dist/en.spec      # the public-domain sources
./speech align                   # place phones in the LJSpeech recordings
./speech diphones                # the recorded voice's inventory
./speech build                   # -> build/en/speech.zspk
./speech listen                  # hear it
```

The pack goes on the card as `/speech/en.spk`; `release/lib/mkfatimg.py`
copies it when it exists.
