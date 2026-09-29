# Running a node in the cloud

A BBS and its zfed node ([fed.md](fed.md)) on a Linux cloud server:
callers by telnet and SSH, posts and mail exchanged with the rest of
the network. For Ubuntu 24.04; other systemd distributions differ
mostly in package names.

Run the commands as an ordinary user with sudo. The examples use
`bbs.example.com`, the network `mynet` and the node `mynode` --
substitute your own. Callers start on the **test ports 2323 (telnet)
and 2222 (SSH)**; "The usual ports" moves them to 23 and 22.

| port | |
|---|---|
| 22 | your own SSH -- untouched |
| 2323, later 23 | telnet, for callers |
| 2222, later 22 | SSH, for callers: a second sshd, for the BBS alone |
| 9070 | zfed: other nodes |

## 1. Before you start

- A DNS name pointing at the server.
- In the cloud provider's firewall: TCP 2222, 2323 and 9070 open.

## 2. Build and install

```
sudo apt update && sudo apt install -y build-essential git
git clone https://github.com/machdyne/zeitlos ~/zeitlos
cd ~/zeitlos
make -C sw/apps/bbs linux && make -C sw/apps/fed linux

sudo useradd --system --create-home --home-dir /var/lib/bbs --shell /bin/sh bbs
sudo passwd -d bbs
sudo install -m 755 sw/apps/bbs/bbs-linux /usr/local/bin/bbs
sudo install -m 755 sw/apps/fed/fed-linux /usr/local/bin/fed
sudo install -d -o bbs -g bbs -m 700 /var/lib/fed
sudo cp -r sw/apps/bbs/data/. /var/lib/bbs/
sudo chown -R bbs:bbs /var/lib/bbs
```

Everything runs as the `bbs` account. It has no password and the shell
`/bin/sh`, exactly: the only place it can log in is the BBS's own sshd
(step 6), which runs nothing but the BBS.

## 3. The node

```
sudo fed key
```

makes the node's key and prints its **public key** -- safe to share.
The **private key** stays in `/var/lib/fed/node.key`; nothing prints
it. **Back it up** (step 9).

Then `/var/lib/fed/fed.cfg` (`sudo -u bbs nano /var/lib/fed/fed.cfg`),
one of two ways:

**Starting a network** -- your node publishes its list:

```
name: mynode
listen: 9070
network: mynet <your public key>
subscribe: mynet/*
```

**Joining one** -- send your public key to its publisher first; they
tell you the network's name, their public key and their address:

```
name: mynode
listen: 9070
network: mynet <the publisher's public key>
peer: <the publisher's public key> bbs.publisher.example:9070
subscribe: mynet/*
```

## 4. The BBS

Two files in `/var/lib/bbs`, **both** needed for federated forums:

- **`bbs.cfg`** -- your BBS's name and sysop, and **uncomment the last
  line**, which connects the BBS to its node:

  ```
  fed: /var/lib/fed/fed.sock
  ```

- **`forums.cfg`** -- a sixth field on each forum the network carries,
  its topic, the same on every BBS:

  ```
  general; General; 0; 10; Anything at all; mynet/forum/general
  ```

## 5. Start it

```
sudo install -m 644 sw/apps/fed/linux/fed.service sw/apps/bbs/linux/bbs.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now fed bbs
journalctl -u bbs -n 20
```

The BBS's log must say **`fed: connected`**. Then call it -- **the first
account made is the sysop's**: `telnet bbs.example.com 2323`.

If you started a network, add nodes as they ask (step 8); `sudo fed
nodes` shows the list.

## 6. SSH for callers

```
sudo mkdir -p /etc/ssh/bbs /run/sshd
sudo ssh-keygen -q -t ed25519 -N "" -C bbs -f /etc/ssh/bbs/ssh_host_ed25519_key
sudo install -m 644 sw/apps/bbs/linux/sshd_bbs_config /etc/ssh/sshd_bbs_config
sudo sshd -t -f /etc/ssh/sshd_bbs_config
sudo install -m 644 sw/apps/bbs/linux/sshd-bbs.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now sshd-bbs
```

Callers: `ssh -p 2222 bbs@bbs.example.com`, Enter at the (empty)
password, then the BBS's own login.

## 7. The machine's firewall

With `ufw`, **allow your own SSH first**, or enabling it cuts you off:

```
sudo ufw allow OpenSSH
sudo ufw allow 2222/tcp && sudo ufw allow 2323/tcp && sudo ufw allow 9070/tcp
sudo ufw enable
```

## 8. Adding a node -- another server, or a Zeitlos board

1. **The new node's public key:** `sudo fed key` on a server, `run fed
   key` on a board.
2. **On the publisher:** `sudo fed add newnode <its public key>`.
3. **On the new node:** its `fed.cfg` as in step 3 ("Joining"), and its
   BBS as in step 4. On a board the files ship ready in `/fed` and
   `/bbs`: fill in the three commented lines of `/fed/fed.cfg`,
   uncomment `fed: fed0` in `/bbs/bbs.cfg`, add the topics to
   `/bbs/forums.cfg`, then `run fed` and `run bbs`. A board needs no
   `listen:` -- it calls out.

## 9. Keeping it

- **Back up `/var/lib/fed/node.key`** off the server. If your node
  publishes its network's list, **that key is the network**.
  Also `/var/lib/bbs`: the users and messages.
- **Updating:** `git -C ~/zeitlos pull`, build and install (step 2's
  `make` and `install` lines), `sudo systemctl restart fed bbs`.
- **Logs:** `journalctl -u fed -u bbs -u sshd-bbs`.

## 10. The usual ports

**Telnet to 23:** `sudo systemctl edit bbs`,

```
[Service]
ExecStart=
ExecStart=/usr/local/bin/bbs -d /var/lib/bbs -t 23
```

then open 23 in both firewalls and `sudo systemctl restart bbs`.

**SSH for callers to 22 -- move your own SSH off 22 first.** This is
the one step that can lock you out:

1. Pick a port for yourself, 2200 say, and open it in both firewalls.
2. `sudo systemctl edit ssh.socket` (Ubuntu 24.04 starts sshd from a
   socket):

   ```
   [Socket]
   ListenStream=
   ListenStream=2200
   ```

   then `sudo systemctl daemon-reload && sudo systemctl restart ssh.socket`.
3. **Keep this session open**; log in on 2200 from a second terminal.
   Only when that works:
4. `Port 22` in `/etc/ssh/sshd_bbs_config`, then `sudo systemctl restart
   sshd-bbs`.

## When something is not right

The BBS's log says most of it (`journalctl -u bbs`):

| it says | |
|---|---|
| `'general' has a topic, but bbs.cfg has no fed: line` | step 4: uncomment `fed:` in `bbs.cfg`, restart the BBS |
| `fed: nothing at ... -- is fed running?` | `sudo systemctl start fed` |
| `'announce' is not a zfed topic` | the sixth field must be the whole topic: `mynet/forum/announce` |

And:

- **`sudo fed nodes` says the running fed is older:** `sudo systemctl
  restart fed`.
- **A node does not connect:** is it on the list (`sudo fed nodes`)? Is
  9070 open at the provider? Its own log says what its session did.
- **A post does not arrive elsewhere:** the forum's topic must be the
  same on both BBSes, letter for letter, and both need step 4.
- **`ssh -p 2222` keeps asking for a password:** `sudo passwd -d bbs`,
  and `sudo usermod -s /bin/sh bbs`.
