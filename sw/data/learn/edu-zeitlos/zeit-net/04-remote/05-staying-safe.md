# Staying safe on the network

```lesson
(objective "Say which of Zeitlos's network apps encrypt, how each checks who it is talking to, and choose safely between them.")
(minutes 15)
```

This lesson puts together what the course has said about safety.

Two questions decide how safe a connection is:

1. **Is it encrypted?** If not, any machine on the way can read it.
2. **Is the other end checked?** If not, something on the way can
   answer in its place.

| | Encrypted | The other end checked by |
|---|---|---|
| web, https | yes | a certificate, vouched for by an authority |
| web, Gemini | yes | trust on first use: the pin |
| term, SSH | yes | you, comparing the host key's fingerprint |
| web, http and Gopher | no | nothing |
| irc | no | nothing |
| term, telnet | no | nothing |

```sources
(source "Zeitlos docs: tls.md; gopher_gemini.md; ssh.md; irc_app.md; netserve.md")
(source "Timeless Computing (docs/tc.md), Communication: Secure Connections")
```

```check
(choice
  (ask "Which of these send what you type unencrypted? Choose all that do.")
  (several)
  (right "irc")
  (right "telnet in term")
  (wrong "SSH in term" "SSH encrypts everything, the password included.")
  (wrong "an https page in web" "https travels inside TLS: it is encrypted.")
  (hint "Two of them: the chat app, and the older way to log in."))
```

---

## Passwords go only where it is encrypted

The rule that follows: type a password only into an encrypted
connection to a checked machine: never into `irc`, never over telnet.

```check
(choice
  (ask "You can reach a machine only by telnet, and it asks for your password. What is the risk?")
  (right "The password crosses the network unencrypted, readable on the way")
  (wrong "None: term hides the password as you type it"
    "Not showing it on your screen is not the same as protecting it on the wire. Telnet sends it as it is.")
  (wrong "It might be too short"
    "Length is not the problem here. Anything typed over telnet travels as it is.")
  (hint "What does telnet lack that SSH has?"))
```

---

## Three ways of checking

The three encrypted kinds check the other end differently:

- **https**: an authority vouches for a certificate, and `web` checks it
  against the authorities on the card. Nothing to do for you.
- **Gemini**: `web` keeps the key it saw first, and refuses any other.
- **SSH**: you compare the fingerprint with the one the owner gives,
  every time.

In all three, a check that fails is refused, never waved through.

```check
(choice
  (ask "With which of these must you check the other end yourself?")
  (right "SSH: comparing the host key's fingerprint")
  (wrong "https: reading the certificate"
    "web checks an https certificate itself, against the authorities on the card. It is SSH that asks you.")
  (wrong "Gemini: typing the pin"
    "web keeps a capsule's pin by itself. It is SSH that asks you to compare.")
  (hint "Which app asks you yes or no?"))
```

---

## When you are the server

- Switch on only what you use: every netserve service is off until set.
- Keep `subnet`, unless something is meant for the public.
- A password of 10 characters or more; SSH rather than telnet; keys
  only (`ssh_auth: key`) if the machine is ever reachable from outside.
- Nothing private in the folder you serve.

```check
(choice
  (ask "You want to reach your Zeitlos computer from your laptop at home. Which is the safest choice?")
  (right "SSH, on your subnet, with a password of 10 or more characters")
  (wrong "Telnet with allow any, so it works from anywhere"
    "That sends your password unencrypted and lets the whole internet try it. Use SSH, and keep it to your subnet.")
  (wrong "Telnet on your subnet, since it is only your home"
    "Better, but still unencrypted. SSH costs nothing more and keeps the password private.")
  (hint "Encrypted, checked, and as few machines as possible."))
```

---

```check
(choice
  (ask "Which connection is encrypted but does not check the other end the first time?")
  (right "A first visit to a Gemini capsule")
  (wrong "An https page"
    "https checks every time, with the certificate and its authority.")
  (wrong "A Gopher menu"
    "Gopher is not encrypted at all.")
  (hint "Trust on first use."))
```

So: use encrypted connections to checked machines for anything private,
and when you are the server, open as little as you can, to as few as
you can.
