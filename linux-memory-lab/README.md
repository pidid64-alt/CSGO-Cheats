# Linux memory lab

The techniques from the Windows lessons in this repository — attach to a process, find a module
base, follow a pointer, read and write a struct, project positions onto a 2D radar — running
natively on Linux, with a web interface, against a target process you start yourself.

**This is not a game tool.** It contains no game offsets, it never looks for a game window, and it
never draws over another application. The target is `target_sim`, a simulator in this folder whose
whole job is to hold entities in memory so there is something real to read. Point memory tools at
software you own. Not at online multiplayer games: CS:GO is gone, CS2 is live, and reading its
memory means cheating against other people and being banned by VAC.

## Quick start

```sh
make                  # builds target_sim with gcc
make run-target       # terminal 1: the process that owns the memory
make run-inspector    # terminal 2: reader + web interface on http://localhost:7100
```

No root, no pip install, no dependencies beyond gcc and Python 3.8+.

Prefer the terminal? `python3 inspector.py --once` prints one snapshot and exits:

```
pid           1625  (/home/user/CSGO-Cheats/linux-memory-lab/target_sim)
module base   0x56402474b000  pie=True
g_world       0x40b0 -> pointer at 0x56402474f0b0
world         0x564024a8b2a0  magic_ok=True  read in 313 us

idx    id name       team   hp   origin
  0  1000 alpha         2   50   (  -137.7,     59.6,  -16.4)
  1  1001 bravo         3   48   (    60.0,   -232.4,  -18.5)
```

## Windows lesson, Linux equivalent

| The repository's Windows code | This lab |
|---|---|
| `FindWindowA` + `GetWindowThreadProcessId` | scan `/proc/<pid>/comm` for the process name |
| `GetModuleBaseAddress` via Toolhelp32 | lowest mapping of the binary in `/proc/<pid>/maps` |
| `#define dwLocalPlayer 0xD30B94` | the ELF symbol `g_world`, resolved from the binary itself |
| `ReadProcessMemory` | `process_vm_readv`, falling back to `/proc/<pid>/mem` |
| `WriteProcessMemory` | `process_vm_writev`, same fallback |
| `#define m_iHealth 0x100` | the offset table at the top of `inspector.py` |
| `WorldToScreen` + GDI overlay | the same projection, drawn on the lab's own canvas |

The important difference is the third row. The Windows projects hard code addresses from a dump,
which is why a single game patch killed all of them. Here the inspector resolves the symbol out of
the ELF symbol table and adds the runtime module base, which is what a debugger does and what
never goes stale.

## The attach chain

Each step is shown live in the web interface:

1. **Find the process.** `/proc/<pid>/comm` holds the name, so a scan of `/proc` is the whole
   search. `/proc/<pid>/exe` then gives the real path of the binary.
2. **Module base.** A position independent executable is loaded at a random address; the first
   mapping of the binary in `/proc/<pid>/maps` is its base.
3. **Static offset.** `g_world` is a global pointer inside the binary image. Its offset comes from
   the ELF symbol table, parsed directly in `inspector.py`.
4. **Pointer address** = module base + static offset, for a PIE build. For a fixed load address
   the symbol value is already absolute, and the inspector handles both.
5. **Dereference.** Reading 8 bytes at that address gives the address of the `World` struct on the
   heap, which is the one read that returns everything the interface shows.

`magic ok` in the interface means the first four bytes of the struct are `4C41424F`, a cheap sanity
check that the pointer chain still lands where it should.

## Reading permission

`process_vm_readv` needs the same permission as a debugger. Same user is usually enough, but on
distributions where `/proc/sys/kernel/yama/ptrace_scope` is `1` only a parent process qualifies.
`target_sim` calls `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` on startup, which is a process
explicitly consenting to be inspected — the clean version of what this lab is about. A real game
would never do that, and working around it is a different thing with a different name.

## The write demo

The web interface can write one thing: the `health` field of one entity. The server validates the
index and the value and computes the address itself, so there is no endpoint that takes a raw
address. Press the button and `target_sim` prints:

```
target_sim: entity 1003 (delta) health changed externally: 54 -> 7
```

That line is the whole lesson about writing to another process: nothing in the target was called,
no function was hooked, a number in its heap simply changed under it.

## Files

| File | What it is |
|---|---|
| `target_sim.c` | the simulator: a `World` struct, 12 entities, 60 Hz, plus the global pointer to follow |
| `inspector.py` | `process_vm_readv` / `process_vm_writev` via ctypes, `/proc` parsing, ELF symbol lookup, JSON API, static file server |
| `static/` | the interface: attach chain, radar canvas, entity table, hex view, write control |
| `Makefile` | `make`, `make run-target`, `make run-inspector`, `make clean` |
