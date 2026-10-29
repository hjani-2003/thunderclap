# ⚡ thunderclap

> *Sleep through the transfer. Wake up to victory.*

Fast, encrypted file and folder transfer between Linux machines, written in C.
Use it from the terminal with the `clap` command (`clap send …`) or from a live web dashboard on `http://127.0.0.1:8080`.

The name comes from *Thunder Breathing, First Form: Thunderclap and Flash*, from Demon Slayer. Zenitsu mastered exactly one move, it's blindingly fast, and he does it in his sleep. thunderclap does one thing too: it moves your files as fast as the link allows, and you can start a big transfer and walk away. If anything interrupts it, run the same command again and it resumes.

- **Fast.** It uses parallel TCP streams (8 by default), 4 MiB chunks, AES-256-GCM via AES-NI, and preallocated files written with `pwrite`.
  - Measured on one laptop over loopback: about 13 Gbit/s for a 1 GiB file, 12 Gbit/s for a 20,000-file tree, and about 24 Gbit/s in raw link benchmarks.
  - On a real LAN, the network or the disk is the limit.
- **Secure.**
  - Devices pair once with a 12-digit code using CPace, a password-authenticated key exchange (PAKE). A wrong guess burns the code, and someone watching the network can't brute-force it offline.
  - Each transfer then runs a Noise-KK-style handshake between the stored device keys, so every session gets fresh keys and forward secrecy.
  - Everything after the handshake is authenticated encryption: AES-256-GCM, or ChaCha20-Poly1305 on CPUs without AES-NI.
- **Smart with data.**
  - Small files are bundled together so 100k tiny files don't cost 100k round trips.
  - zstd compression is adaptive per chunk: logs shrink, while videos and ISOs are sent raw with almost no CPU cost.
  - Every chunk carries an XXH3 checksum, checked after decompression.
- **Resumable.** If a transfer dies (network drop, crash, Ctrl-C), run the same `clap send` again. Only the missing chunks are sent.
- **Zero config on a LAN.** Daemons find each other with UDP broadcast and multicast beacons. Each peer is shown with the interface and link speed it is reachable on, and the fastest interface wins.

📖 **[User guide (PDF)](docs/thunderclap-user-guide.pdf)** covers installation, pairing, the CLI, the web dashboard, speed tuning, resuming, security, configuration and troubleshooting. Screenshots are included.

## Install

```bash
git clone https://github.com/hjani-2003/thunderclap.git
cd thunderclap
sudo apt install build-essential pkg-config libsodium-dev libzstd-dev libxxhash-dev libcjson-dev
make
make install          # → ~/.local/bin/clap + a systemd user unit
systemctl --user enable --now clap    # optional: start the daemon at login
```

Open the ports on both machines if a firewall is on:

```bash
sudo ufw allow 7878/tcp && sudo ufw allow 7879/udp
```

Any compiler with C11 support works (tested with GCC 12 and GCC 15). libsodium must be 1.0.18 or newer; that's the release that added the ristretto255 functions pairing uses. The web UI is turned into C arrays at build time by `tools/bin2c.c`, so no extra tools are needed.

## Quick start

On machine **B**:

```console
$ clap pair
Pairing code:  4823-6195-0742
On the other machine run:
  clap pair bravo 4823-6195-0742
```

On machine **A**:

```console
$ clap pair bravo 4823-6195-0742
✓ paired with bravo (9329377ebebc2a09)

$ clap send ~/Videos/trip ~/notes.txt --to bravo
[██████████████░░░░░░░░░░░░]  54.2%  5.1 GiB / 9.4 GiB  9.41 Gbit/s  ETA 0:04
✓ sent to bravo (9.4 GiB, 312 files) in 8.6s — 9.38 Gbit/s avg
```

Files land in `~/Downloads/thunderclap` on B. Change this with `clap config download_dir /data/inbox`.

**Without pairing (one-off):** on the receiver run `clap recv --code`. Then on the sender run `clap send file --to bravo --code 1234-5678-9012`.

**Over the internet:** forward TCP 7878 on the receiver's router, then use `--to their.host.name:7878`. Discovery only works on the LAN, but pairing and transfers work anywhere.

## Commands

| Command | What it does |
|---|---|
| `clap send <paths>… --to <peer>` | Send files or folders. Options: `--streams N`, `--compress auto\|off\|1-19`, `--code C`, `--detach`. Ctrl-C cancels; send again to resume. |
| `clap recv --code` | Show a one-shot code and wait for an incoming transfer. |
| `clap pair` / `clap pair <peer> <code>` | Pair two machines. |
| `clap peers` | Paired devices and devices on the network, with interface and link speed. |
| `clap ls`, `clap history` | Current and past transfers. |
| `clap watch\|accept\|reject\|cancel <id>` | Follow a transfer or act on it. |
| `clap bench <peer> [--secs N]` | Memory-to-memory throughput test, encrypted, with no disk I/O. |
| `clap ui` | Open the web dashboard with its access token. |
| `clap status`, `clap config [key value]` | Show or change settings: `name`, `download_dir`, `auto_accept`, `streams`, `compress`. |
| `clap daemon [--port N] [--web N] [--dir D] [--name X]` | Run the daemon in the foreground. Other commands start it automatically. |
| `clap stop` | Stop the daemon. |

`<peer>` can be a device name, a fingerprint prefix, or `host[:port]`.

Several independent daemons can run on one machine with `--home DIR` (or `CLAP_HOME`). The tests use this.

## Web UI

Run `clap ui`. The dashboard lets you:
- see peers with link speed, and pair new devices;
- browse your disk and send files;
- accept or decline incoming transfers;
- follow live progress, ETA, compression ratio and per-transfer speed sparklines;
- see a 60-second throughput chart and the transfer history;
- change settings.

The UI is served only on `127.0.0.1`. Every API call needs the token in `~/.config/thunderclap/web_token`, which `clap ui` passes in the URL fragment. Requests with a foreign `Host` or `Origin` header are refused, so other websites can't drive the daemon (no CSRF or DNS rebinding).

## How it works

```
 PC A                                              PC B
┌────────────┐  unix sock  ┌──────────────┐  TCP ×N (AEAD)  ┌──────────────┐
│ clap (CLI) │────────────▶│ clap daemon  │◀───────────────▶│ clap daemon  │
└────────────┘             │  discovery   │  UDP beacons    │              │
┌────────────┐  HTTP+SSE   │  transfers   │◀ ─ ─ ─ ─ ─ ─ ─ ▶│              │
│  browser   │────────────▶│  web / API   │                 └──────────────┘
└────────────┘ 127.0.0.1   └──────────────┘
```

**Transfer sequence:**
1. The sender walks the paths into a **manifest**: paths, sizes, modes, mtimes and symlinks.
2. Both sides derive the same **chunk layout** from it. Files ≥ 1 MiB are split into 4 MiB chunks; smaller files are packed into bundle chunks.
3. The sender sends `OFFER` with the manifest on the encrypted control connection.
4. The receiver answers `ACCEPT`. The answer carries a session id and a bitmap of chunks it already has, from an earlier attempt.
5. The sender opens N data connections. Each one gets its own keys, derived from the session key plus a random salt from the receiver, so a recorded connection can't be replayed.
6. Workers pull chunks from a shared queue and run: `pread` → XXH3 → zstd (if it helps) → encrypt → send.
7. The receiver decrypts, decompresses, checks XXH3, then `pwrite`s at the chunk's offset into a preallocated `name.clap-part` file. It then marks the chunk in a journal. The journal is flushed every 3 s, *after* `syncfs`, so it never claims bytes that aren't on disk.
8. The sender sends `DONE` with per-stream counts. The receiver waits for every stream to drain, then either asks for missing chunks (`NEED`) or finalizes:
   - renames the part files,
   - applies modes and mtimes,
   - creates symlinks,
   - syncs, deletes the journal,
   - sends `COMPLETE`.

**Safety checks on the receiving side:**
- Manifest paths are rejected if they are absolute, contain `..`, or would write through a symlink that is part of the same transfer.
- Existing files are never overwritten: `photos` becomes `photos (1)`.
- On resume, any chunk whose file was deleted or moved is requested again.

**Source layout** (all in `src/`):

| File | Role |
|---|---|
| `main.c` | CLI |
| `daemon.c` | Event loop and unix-socket API |
| `http.c` | HTTP/SSE server |
| `api.c` | Commands shared by the CLI and the web UI |
| `peer.c` | Hellos, handshakes, pairing |
| `crypt.c` | Keys, CPace, KK handshake, AEAD channels |
| `sender.c`, `receiver.c` | Transfer engine |
| `manifest.c` | File list and chunk layout |
| `journal.c` | Resume bitmap |
| `compress.c` | Adaptive zstd |
| `discovery.c` | LAN beacons and interface speed |
| `transfer.c` | State, statistics, history |
| `config.c` | Settings and peer store |
| `proto.h` | Wire format |

## Testing

```bash
make test    # unit tests: CPace / KK / AEAD tamper & replay, manifest safety, chunk layout, journal, compression
make e2e     # two daemons on localhost: pairing, files, trees, name clashes, unpaired refusal,
             # one-shot codes, kill -9 mid-transfer + resume, web auth, bench
make docs    # rebuild docs/thunderclap-user-guide.pdf from docs/guide/ (needs Node.js 22+ and Chrome/Chromium)
```

## Performance notes

- **Find the ceiling first.** `clap bench <peer>` measures the link plus crypto with no disk involved. Compare it with `iperf3`.
- **Fastest cable options:**
  - A direct cable between two machines. Each port needs an address, e.g. link-local, or `ip addr add 10.0.0.1/24 dev eth0`.
  - USB4/Thunderbolt networking (`thunderbolt0`, 20–40 Gbit/s).

  thunderclap needs no changes for these, because they are just faster network interfaces.
- **Bluetooth isn't supported.** At about 2 Mbit/s in practice it would be hundreds of times slower than gigabit Ethernet.
- **More streams** help on links with high latency × bandwidth, e.g. over the internet. Try `--streams 16`.
- **`--compress off`** saves CPU when the data is known to be incompressible and the link is very fast. `auto` already skips incompressible chunks after a quick sample.

## Limitations / ideas

- Only one transfer at a time per set of files. If the same files are sent again while that transfer is still running, the second send is refused.
- No NAT traversal or relay: internet use needs a forwarded port.
- A heads-up for security folks: "Thunderclap" is also the name of a 2019 research paper on DMA attacks through Thunderbolt ports. This project is unrelated; the name is purely the anime reference.
- Ideas for later:
  - a UDP transport with its own congestion control, for lossy or long-distance links;
  - delta sync, like rsync;
  - drag-and-drop upload in the browser;
  - io_uring.

## License

Copyright (C) 2026 Harman

thunderclap is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; see [LICENSE](LICENSE) for the full text.
