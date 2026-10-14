#!/usr/bin/env python3
"""
inspector.py - reads the memory of target_sim and serves a small web interface.

This is the Linux counterpart of the Windows helpers in this repository:

    Windows                         Linux
    ------------------------------  --------------------------------------
    FindWindowA + GetWindowThread   scan /proc/<pid>/comm for the process
    GetModuleBaseAddress (Toolhelp) first mapping of the binary in /proc/<pid>/maps
    hard coded dwLocalPlayer        ELF symbol g_world resolved from the binary
    ReadProcessMemory               process_vm_readv  (fallback: /proc/<pid>/mem)
    WriteProcessMemory              process_vm_writev (fallback: /proc/<pid>/mem)

The target is target_sim, a program in this folder that you start yourself. The
write endpoint deliberately accepts nothing but "set the health field of entity
N", so this stays a lesson and does not turn into a general memory editor.

Usage:
    python3 inspector.py                 # web interface on http://localhost:7100
    python3 inspector.py --once          # one snapshot to stdout, no server
    python3 inspector.py --pid 1234      # attach to a specific pid
"""

import argparse
import ctypes
import json
import os
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TARGET_NAME = "target_sim"
SYMBOL = "g_world"

# ---------------------------------------------------------------------------
# Struct layout of the target, mirrored by hand.
# This is the Linux version of "#define m_iHealth 0x100": the numbers below are
# the contract between the two programs. Change the C structs and these break.
# ---------------------------------------------------------------------------
WORLD_MAGIC = 0x4C41424F
SIZEOF_WORLD = 800
OFF_MAGIC = 0x00
OFF_COUNT = 0x04
OFF_TICK = 0x08
OFF_CAMERA = 0x10
OFF_YAW = 0x1C
OFF_ENTITIES = 0x20

SIZEOF_ENTITY = 0x40
E_ID = 0x00
E_HEALTH = 0x04
E_TEAM = 0x08
E_ALIVE = 0x0C
E_ORIGIN = 0x10
E_VELOCITY = 0x1C
E_NAME = 0x28
E_NAME_LEN = 24

MAX_ENTITIES = 12


# ---------------------------------------------------------------------------
# Raw memory access
# ---------------------------------------------------------------------------
class IoVec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]


_libc = ctypes.CDLL("libc.so.6", use_errno=True)
_libc.process_vm_readv.argtypes = [
    ctypes.c_int, ctypes.POINTER(IoVec), ctypes.c_ulong,
    ctypes.POINTER(IoVec), ctypes.c_ulong, ctypes.c_ulong,
]
_libc.process_vm_readv.restype = ctypes.c_ssize_t
_libc.process_vm_writev.argtypes = _libc.process_vm_readv.argtypes
_libc.process_vm_writev.restype = ctypes.c_ssize_t


def read_mem(pid: int, address: int, size: int) -> bytes:
    """process_vm_readv, with /proc/<pid>/mem as a fallback."""
    buf = ctypes.create_string_buffer(size)
    local = IoVec(ctypes.cast(buf, ctypes.c_void_p), size)
    remote = IoVec(ctypes.c_void_p(address), size)
    n = _libc.process_vm_readv(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    if n == size:
        return buf.raw
    if n < 0:
        err = ctypes.get_errno()
        try:
            with open(f"/proc/{pid}/mem", "rb", 0) as mem:
                mem.seek(address)
                data = mem.read(size)
            if len(data) == size:
                return data
        except OSError:
            pass
        raise OSError(err, f"process_vm_readv failed at {address:#x}: {os.strerror(err)}")
    raise OSError(0, f"short read at {address:#x}: {n} of {size} bytes")


def write_mem(pid: int, address: int, data: bytes) -> int:
    """process_vm_writev, with /proc/<pid>/mem as a fallback."""
    buf = ctypes.create_string_buffer(data, len(data))
    local = IoVec(ctypes.cast(buf, ctypes.c_void_p), len(data))
    remote = IoVec(ctypes.c_void_p(address), len(data))
    n = _libc.process_vm_writev(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    if n == len(data):
        return n
    err = ctypes.get_errno()
    try:
        with open(f"/proc/{pid}/mem", "rb+", 0) as mem:
            mem.seek(address)
            return mem.write(data)
    except OSError:
        raise OSError(err, f"process_vm_writev failed at {address:#x}: {os.strerror(err)}")


# ---------------------------------------------------------------------------
# Finding the process, its module base and the symbol inside it
# ---------------------------------------------------------------------------
def find_pid(name: str = TARGET_NAME) -> int:
    """The /proc equivalent of FindWindowA: look the process up by name."""
    me = os.getpid()
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        pid = int(entry)
        if pid == me:
            continue
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() == name:
                    return pid
        except OSError:
            continue
    raise LookupError(f"no process named {name!r} is running")


def module_base(pid: int, path: str) -> int:
    """Lowest mapping of `path` in /proc/<pid>/maps: the module base address."""
    base = None
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            parts = line.split(maxsplit=5)
            if len(parts) < 6:
                continue
            if parts[5].strip() != path:
                continue
            start = int(parts[0].split("-")[0], 16)
            if base is None or start < base:
                base = start
    if base is None:
        raise LookupError(f"{path} is not mapped into pid {pid}")
    return base


def elf_symbol(path: str, symbol: str):
    """Return (static offset of `symbol`, is_pie) by reading the ELF symbol table.

    A debugger does this instead of hard coding addresses. The repository's
    Windows projects hard code them, which is why every game patch broke them.
    """
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{path} is not an ELF file")
    if data[4] != 2:
        raise ValueError("only 64-bit ELF binaries are supported by this lab")

    e_type = struct.unpack_from("<H", data, 0x10)[0]
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from("<I", data, off + 4)
        sh_offset, sh_size = struct.unpack_from("<QQ", data, off + 0x18)
        sh_link, = struct.unpack_from("<I", data, off + 0x28)
        sh_entsize, = struct.unpack_from("<Q", data, off + 0x38)
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    want = symbol.encode()
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type not in (2, 11) or sh_entsize == 0:  # SHT_SYMTAB, SHT_DYNSYM
            continue
        _, str_off, str_size, _, _ = sections[sh_link]
        strtab = data[str_off:str_off + str_size]
        for pos in range(sh_offset, sh_offset + sh_size, sh_entsize):
            st_name, = struct.unpack_from("<I", data, pos)
            if st_name == 0:
                continue
            end = strtab.find(b"\0", st_name)
            if strtab[st_name:end] != want:
                continue
            st_value, = struct.unpack_from("<Q", data, pos + 8)
            return st_value, e_type == 3  # ET_DYN means position independent
    raise LookupError(f"symbol {symbol!r} not found in {path} (is the binary stripped?)")


# ---------------------------------------------------------------------------
# Session: resolve once, then snapshot as often as you like
# ---------------------------------------------------------------------------
class Session:
    def __init__(self, pid=None):
        self.wanted_pid = pid
        self.pid = None
        self.exe = None
        self.base = 0
        self.pie = False
        self.sym_offset = 0
        self.pointer_addr = 0

    def attach(self):
        pid = self.wanted_pid if self.wanted_pid else find_pid()
        exe = os.path.realpath(f"/proc/{pid}/exe")
        sym_offset, pie = elf_symbol(exe, SYMBOL)
        base = module_base(pid, exe)
        self.pid, self.exe, self.base, self.pie = pid, exe, base, pie
        self.sym_offset = sym_offset
        # Position independent binary: runtime address = module base + offset.
        # Fixed load address: the symbol value is already absolute.
        self.pointer_addr = base + sym_offset if pie else sym_offset

    def ensure(self):
        if self.pid is None or not os.path.exists(f"/proc/{self.pid}"):
            self.attach()

    def snapshot(self):
        self.ensure()
        started = time.perf_counter()

        # Step 1: read the global pointer, step 2: read what it points at.
        world_addr, = struct.unpack("<Q", read_mem(self.pid, self.pointer_addr, 8))
        if world_addr == 0:
            raise RuntimeError("g_world is still null, the target is starting up")
        blob = read_mem(self.pid, world_addr, SIZEOF_WORLD)
        read_us = int((time.perf_counter() - started) * 1_000_000)

        magic, = struct.unpack_from("<I", blob, OFF_MAGIC)
        count, = struct.unpack_from("<i", blob, OFF_COUNT)
        tick, = struct.unpack_from("<Q", blob, OFF_TICK)
        cam = struct.unpack_from("<fff", blob, OFF_CAMERA)
        yaw, = struct.unpack_from("<f", blob, OFF_YAW)
        count = max(0, min(count, MAX_ENTITIES))

        entities = []
        for i in range(count):
            off = OFF_ENTITIES + i * SIZEOF_ENTITY
            name = blob[off + E_NAME:off + E_NAME + E_NAME_LEN].split(b"\0")[0]
            entities.append({
                "index": i,
                "address": hex(world_addr + off),
                "id": struct.unpack_from("<i", blob, off + E_ID)[0],
                "health": struct.unpack_from("<i", blob, off + E_HEALTH)[0],
                "team": struct.unpack_from("<i", blob, off + E_TEAM)[0],
                "alive": bool(struct.unpack_from("<i", blob, off + E_ALIVE)[0]),
                "origin": dict(zip("xyz", struct.unpack_from("<fff", blob, off + E_ORIGIN))),
                "name": name.decode("ascii", "replace"),
            })

        return {
            "ok": True,
            "target": {
                "pid": self.pid,
                "exe": self.exe,
                "pie": self.pie,
                "module_base": hex(self.base),
                "symbol": SYMBOL,
                "symbol_offset": hex(self.sym_offset),
                "pointer_addr": hex(self.pointer_addr),
                "world_addr": hex(world_addr),
                "magic_ok": magic == WORLD_MAGIC,
            },
            "world": {
                "tick": tick,
                "camera": dict(zip("xyz", cam)),
                "yaw": yaw,
                "entity_count": count,
            },
            "entities": entities,
            "hex": {
                "header": hexdump(blob[:OFF_ENTITIES], world_addr),
                "entity0": hexdump(blob[OFF_ENTITIES:OFF_ENTITIES + SIZEOF_ENTITY],
                                   world_addr + OFF_ENTITIES),
            },
            "read_us": read_us,
        }

    def write_health(self, index: int, value: int):
        self.ensure()
        if not 0 <= index < MAX_ENTITIES:
            raise ValueError("entity index out of range")
        if not 1 <= value <= 100:
            raise ValueError("health must be between 1 and 100")
        world_addr, = struct.unpack("<Q", read_mem(self.pid, self.pointer_addr, 8))
        addr = world_addr + OFF_ENTITIES + index * SIZEOF_ENTITY + E_HEALTH
        write_mem(self.pid, addr, struct.pack("<i", value))
        return {"ok": True, "address": hex(addr), "value": value}


def hexdump(data: bytes, base_addr: int):
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join(f"{b:02x}" for b in chunk).ljust(47)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base_addr + off:012x}  {hexpart}  |{text}|")
    return lines


# ---------------------------------------------------------------------------
# Web interface
# ---------------------------------------------------------------------------
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")
CONTENT_TYPES = {".html": "text/html; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}


class Handler(BaseHTTPRequestHandler):
    server_version = "memory-lab"
    session: Session = None

    def _json(self, payload, status=200):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _static(self, name):
        path = os.path.normpath(os.path.join(STATIC_DIR, name))
        if not path.startswith(STATIC_DIR) or not os.path.isfile(path):
            self.send_error(404)
            return
        with open(path, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type",
                         CONTENT_TYPES.get(os.path.splitext(path)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/api/state":
            try:
                self._json(self.session.snapshot())
            except Exception as exc:
                self.session.pid = None
                self._json({"ok": False, "error": f"{type(exc).__name__}: {exc}"})
        elif path in ("/", "/index.html"):
            self._static("index.html")
        else:
            self._static(path.lstrip("/"))

    def do_POST(self):
        if self.path.split("?")[0] != "/api/write":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length) or b"{}")
            result = self.session.write_health(int(payload["index"]), int(payload["value"]))
            self._json(result)
        except Exception as exc:
            self._json({"ok": False, "error": f"{type(exc).__name__}: {exc}"}, status=400)

    def log_message(self, fmt, *args):  # keep the console readable
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, help="attach to this pid instead of searching")
    parser.add_argument("--port", type=int, default=7100, help="web interface port (default 7100)")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    parser.add_argument("--once", action="store_true", help="print one snapshot and exit")
    args = parser.parse_args()

    session = Session(args.pid)
    if args.once:
        try:
            snap = session.snapshot()
        except Exception as exc:
            print(f"error: {exc}", file=sys.stderr)
            print(f"hint: start the target first ('make run-target')", file=sys.stderr)
            return 1
        t = snap["target"]
        print(f"pid           {t['pid']}  ({t['exe']})")
        print(f"module base   {t['module_base']}  pie={t['pie']}")
        print(f"{t['symbol']:<13} {t['symbol_offset']} -> pointer at {t['pointer_addr']}")
        print(f"world         {t['world_addr']}  magic_ok={t['magic_ok']}  read in {snap['read_us']} us")
        print()
        print(f"{'idx':>3} {'id':>5} {'name':<10} {'team':>4} {'hp':>4}   origin")
        for e in snap["entities"]:
            o = e["origin"]
            print(f"{e['index']:>3} {e['id']:>5} {e['name']:<10} {e['team']:>4} {e['health']:>4}   "
                  f"({o['x']:8.1f}, {o['y']:8.1f}, {o['z']:6.1f})")
        return 0

    Handler.session = session
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"memory lab: serving http://{args.host}:{args.port}")
    print(f"memory lab: target process '{TARGET_NAME}', symbol '{SYMBOL}'")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nmemory lab: stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
