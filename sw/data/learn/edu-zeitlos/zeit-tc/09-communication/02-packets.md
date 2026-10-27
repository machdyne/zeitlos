# Packets and layers

```lesson
(objective "Say why networks cut messages into packets, and how layers each add their own header.")
(minutes 15)
(keywords "packet" "header (of a packet)" "checksum" "frame (on a link)")
```

A wire of its own works for two machines, but joining many machines
that way is impractical, so they share connections instead.

The idea that made modern networks possible is to cut every message
into **packets**: short blocks of data, each with a **header** in front
that gives its source, its destination, its length, and a
**checksum**, a number worked out from the data so that damage can be
noticed. Packets from many conversations then share the same wires,
taking turns, and each finds its own way to where it is going.

```sources
(source "Timeless Computing (docs/tc.md), Communication: Packets")
```

```check
(choice
  (ask "A packet arrives, and the checksum worked out from its data does not match the one in its header. What does that tell the receiver?")
  (right "The packet was damaged on the way")
  (wrong "It went to the wrong machine"
    "The destination is a separate part of the header. A checksum that does not match means the data changed on the way.")
  (wrong "Nothing: checksums often differ"
    "On an undamaged packet the two always match. A mismatch is the sign of damage.")
  (hint "What is the checksum for?"))
```

---

## Layers

Network software is built in **layers**. Each layer uses the one below
it and offers a simpler service to the one above, the way a program
uses the operating system without knowing how the disk works.

| layer | does |
|---|---|
| physical and link | moves data to neighbours, by cable or radio |
| network | carries packets across networks |
| transport | delivers data to programs |
| application | gives a service to the user |

The next lessons take these in turn.

```sources
(source "Timeless Computing (docs/tc.md), Communication: Layers")
```

```check
(choice
  (ask "Which layer gets packets across many networks, from one machine to another far away?")
  (right "The network layer")
  (wrong "The link layer"
    "The link layer moves data only to neighbours on the same link. Crossing networks is the network layer's job.")
  (wrong "The application layer"
    "The application layer gives the service, such as a web page. It relies on the layers below to move the data.")
  (hint "Look at the table: which one says across networks?"))
```

---

## Each layer adds a header

A message from an application goes down through the layers, and each
adds its own header. It crosses the wire as a **frame**, the link
layer's name for a packet, and goes back up at the far end, each layer
taking its header off. Choose a layer to see what it hands to the one
below.

```figure
(define c (choice 'layer '("app" "trans" "net" "link") "Layer"))
(define names '("link" "network" "transport"))
(define w (list 40 46 52))
(define (hdr k x) (wo-box x 100 (list-ref w k) 34 (list-ref names k) 'tiny))
(define nh c)
(define x0 (- 160 (/ (+ 90 (if (> nh 2) 40 0) (if (> nh 1) 46 0) (if (> nh 0) 52 0)) 2)))
(if (> nh 2) (hdr 0 x0))
(define x1 (+ x0 (if (> nh 2) 40 0)))
(if (> nh 1) (hdr 1 x1))
(define x2 (+ x1 (if (> nh 1) 46 0)))
(if (> nh 0) (hdr 2 x2))
(define x3 (+ x2 (if (> nh 0) 52 0)))
(frame x3 100 90 34 'ink 2)
(text (+ x3 45) 121 "data" 'centre 'small 'bold)
(text 160 70 (list-ref '("from the application" "from transport: one header" "from network: two headers" "on the link: a frame") c) 'centre 'small 'bold)
(caption "Down the layers: each adds its header in front.")
```

```check
(choice
  (ask "A web page goes over a radio link today and over a cable tomorrow. What must change in the web browser?")
  (right "Nothing: only the link layer is different")
  (wrong "The browser must learn to use radio"
    "Each layer uses only the one below it. The browser hands its data down and never sees the link layer.")
  (wrong "Every layer must change"
    "Only the link layer changes. The layers above work the same over any link: that is the point of layers.")
  (hint "Which layer deals with cables and radio?"))
```

So: messages travel as packets with headers, and each layer adds its
own header going down and removes it coming up.
