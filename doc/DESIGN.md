# unfsd Design Notes

## Overview

Userspace NFSv2/v3 daemon for FreeBSD 8. Uses kernel `fhandle_t` via
`getfh()`/`fhopen()` for stateless operation. Single-process `poll()` event loop.

## Design Decisions

- **File handles**: kernel `fhandle_t` via `getfh()`, `fhopen()`, `fhstat()`, `fhstatfs()`
- **Credentials**: own UNIX mode-bit check against mapped UID/GID; `seteuid()`/`setegid()` for creates
- **Concurrency**: single process, `poll()` event loop (workers planned later)
- **RPC/XDR**: own thin bounds-checked buffer codec
- **IPv4 only**, no DNS lookups
- **Exports**: one path per line in `/etc/exports`-compatible syntax
- **Target**: FreeBSD 8 with ZFS, netbooting via pxeboot (NFSv2/UDP)

## Layering

net -> rpc -> (mount | nfs2 | nfs3) -> cred + fh -> fs

## Handler Shape

Every procedure: `int proc(struct req *r)`

## Ports

- NFS: 2049 (UDP, TCP planned)
- MOUNT: 1058 (configurable with -m)

## Build

```
make && make check
```

## Milestones

- M0: Build system, logging, XDR codec with tests
- M1: RPC over UDP, NULL procedures, registration (done)
- M2: Exports parser, MOUNT, handles, read-only v2
- M3: Read-only v3, TCP, record marking
- M4: Write operations, credential switching, DRC, wcc
- M5: v2 complete, netboot test
- M6: Hardening, fuzzing, performance

## Supported Exports Syntax

```
path [-ro] [-alldirs] [-maproot=USER] [-mapall=USER] [-network ADDR] [-mask ADDR]
```

No hostnames, no netgroups, no DNS.
