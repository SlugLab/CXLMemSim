#!/usr/bin/env python3
"""Run a QEMU qtest experiment for CXL Type2 near-switch offloads."""

from __future__ import annotations

import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


OP_READ = 0
OP_WRITE = 1
OP_SWITCH_QUERY = 24

REQ = struct.Struct("<BQQQQQ64s")
RESP = struct.Struct("<BQQ64s")

CXL_GPU_MAGIC = 0x43584C32
CXL_GPU_CAP_SWITCH_CORES = 1 << 8
CXL_GPU_SUCCESS = 0
CXL_GPU_CMD_STATUS_COMPLETE = 3

CXL_GPU_REG_MAGIC = 0x0000
CXL_GPU_REG_CAPS = 0x000C
CXL_GPU_REG_CMD = 0x0010
CXL_GPU_REG_CMD_STATUS = 0x0014
CXL_GPU_REG_CMD_RESULT = 0x0018
CXL_GPU_REG_PARAM0 = 0x0040
CXL_GPU_REG_RESULT0 = 0x0080
CXL_GPU_DATA_OFFSET = 0x1000

CXL_GPU_CMD_SWITCH_MEMCPY = 0xE0
CXL_GPU_CMD_SWITCH_MEMSET = 0xE1
CXL_GPU_CMD_SWITCH_REDUCE_ADD64 = 0xE2
CXL_GPU_CMD_SWITCH_DOT_I32 = 0xE3
CXL_GPU_CMD_SWITCH_MATMUL_I32 = 0xE4
CXL_GPU_CMD_SWITCH_GET_STATS = 0xE5
CXL_GPU_CMD_SWITCH_HWJIT = 0xE6
CXL_GPU_CMD_SWITCH_HWJIT_STATS = 0xE7

DAMER_HWJIT_QUANTIZE = 1 << 0
DAMER_HWJIT_COMPRESS = 1 << 1
DAMER_HWJIT_CHECKSUM = 1 << 2
DAMER_HWJIT_FILTER = 1 << 3
DAMER_HWJIT_REDUCE = 1 << 4
DAMER_HWJIT_SCATTER_GATHER = 1 << 5
DAMER_HWJIT_REPLICATE = 1 << 6
DAMER_HWJIT_PERSIST = 1 << 7
DAMER_QWEN27B_KV_PACK = 1
DAMER_QWEN27B_PREFILL_ACTIVATION_SPILL = 2
DAMER_QWEN27B_DECODE_KV_FETCH = 3
DAMER_QWEN27B_ATTENTION_MASK_FILTER = 4
DAMER_QWEN27B_TP_LOGITS_REDUCE = 5

TYPE2_VENDOR_ID = 0x8086
TYPE2_DEVICE_ID = 0x0D92


def recvall(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        data = sock.recv(size - len(chunks))
        if not data:
            raise RuntimeError(f"connection closed after {len(chunks)} of {size} bytes")
        chunks.extend(data)
    return bytes(chunks)


def wait_for_port(host: str, port: int, timeout_s: float = 10.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for {host}:{port}")


def expect(name: str, actual, expected) -> None:
    if actual != expected:
        raise RuntimeError(f"{name}: expected {expected!r}, got {actual!r}")
    print(f"PASS {name}: {actual}")


class MemSimClient:
    def __init__(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port), timeout=5)
        self.ts = 1000

    def close(self) -> None:
        self.sock.close()

    def request(self, op: int, addr: int = 0, size: int = 0, value: int = 0,
                expected: int = 0, data: bytes = b"") -> tuple[int, int, bytes]:
        self.ts += 100
        payload = data[:64].ljust(64, b"\0")
        self.sock.sendall(REQ.pack(op, addr, size, self.ts, value, expected, payload))
        status, latency_ns, old_value, resp_data = RESP.unpack(recvall(self.sock, RESP.size))
        if status != 0:
            raise RuntimeError(f"MemSim op {op} failed: status={status}")
        return latency_ns, old_value, resp_data

    def write64(self, addr: int, data: bytes) -> None:
        self.request(OP_WRITE, addr=addr, size=64, data=data[:64].ljust(64, b"\0"))

    def read(self, addr: int, size: int) -> bytes:
        _, _, data = self.request(OP_READ, addr=addr, size=size)
        return data[:size]

    def stats(self) -> dict[str, int]:
        _, enabled, data = self.request(OP_SWITCH_QUERY)
        values = struct.unpack("<8Q", data)
        return {
            "enabled": enabled,
            "general_ops": values[0],
            "ai_ops": values[1],
            "general_bytes": values[2],
            "ai_bytes": values[3],
            "queued_ns": values[4],
            "service_ns": values[5],
            "general_cores": values[6],
            "ai_cores": values[7],
        }


def pack_hwjit_control(ttl: int = 4, max_ops: int = 1, flags: int = 0) -> int:
    return (flags << 32) | ((max_ops & 0xFFFF) << 16) | (ttl & 0xFFFF)


def hwjit_quantize(payload: bytes) -> bytes:
    out = bytearray()
    for idx in range(0, len(payload), 2):
        hi = payload[idx + 1] if idx + 1 < len(payload) else 0
        out.append((payload[idx] + hi) // 2)
    return bytes(out)


class QTest:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buf = b""

    def close(self) -> None:
        self.sock.close()

    def cmd(self, text: str) -> list[str]:
        self.sock.sendall((text + "\n").encode("ascii"))
        while True:
            while b"\n" not in self.buf:
                data = self.sock.recv(4096)
                if not data:
                    raise RuntimeError("qtest socket closed")
                self.buf += data
            line, self.buf = self.buf.split(b"\n", 1)
            decoded = line.decode("ascii")
            if decoded.startswith("IRQ "):
                continue
            if not decoded.startswith("OK"):
                raise RuntimeError(f"qtest command {text!r} returned {decoded!r}")
            return decoded.split()

    def outl(self, port: int, value: int) -> None:
        self.cmd(f"outl 0x{port:x} 0x{value & 0xFFFFFFFF:x}")

    def inl(self, port: int) -> int:
        return int(self.cmd(f"inl 0x{port:x}")[1], 0)

    def writel(self, addr: int, value: int) -> None:
        self.cmd(f"writel 0x{addr:x} 0x{value & 0xFFFFFFFF:x}")

    def writeq(self, addr: int, value: int) -> None:
        self.cmd(f"writeq 0x{addr:x} 0x{value & 0xFFFFFFFFFFFFFFFF:x}")

    def readl(self, addr: int) -> int:
        return int(self.cmd(f"readl 0x{addr:x}")[1], 0)

    def readq(self, addr: int) -> int:
        return int(self.cmd(f"readq 0x{addr:x}")[1], 0)


def pci_config_addr(bus: int, dev: int, fn: int, offset: int) -> int:
    return 0x80000000 | (bus << 16) | (dev << 11) | (fn << 8) | (offset & 0xFC)


def pci_read(qt: QTest, bus: int, dev: int, fn: int, offset: int) -> int:
    qt.outl(0xCF8, pci_config_addr(bus, dev, fn, offset))
    return qt.inl(0xCFC)


def pci_write(qt: QTest, bus: int, dev: int, fn: int, offset: int, value: int) -> None:
    qt.outl(0xCF8, pci_config_addr(bus, dev, fn, offset))
    qt.outl(0xCFC, value)


def scan_pci(qt: QTest, max_bus: int = 31) -> list[tuple[int, int, int, int, int, int]]:
    devices = []
    for bus in range(max_bus + 1):
        for dev in range(32):
            for fn in range(8):
                id_dword = pci_read(qt, bus, dev, fn, 0)
                vendor = id_dword & 0xFFFF
                device = (id_dword >> 16) & 0xFFFF
                if vendor == 0xFFFF:
                    continue
                header = (pci_read(qt, bus, dev, fn, 0x0C) >> 16) & 0xFF
                class_code = pci_read(qt, bus, dev, fn, 0x08) >> 8
                devices.append((bus, dev, fn, vendor, device, header, class_code))
    return devices


def bridge_prefetch_window(base: int, size: int) -> tuple[int, int, int]:
    limit = base + size - 1
    low = (((limit >> 16) & 0xFFF0) << 16) | ((base >> 16) & 0xFFF0) | 0x1
    return low, (base >> 32) & 0xFFFFFFFF, (limit >> 32) & 0xFFFFFFFF


def configure_pci_bridge(qt: QTest, bus: int, dev: int, fn: int,
                         primary: int, secondary: int, subordinate: int,
                         window_base: int, window_size: int) -> None:
    """Assign buses and a 64-bit prefetch window to an unconfigured bridge."""
    pci_write(qt, bus, dev, fn, 0x18,
              (subordinate << 16) | (secondary << 8) | primary)
    low, high_base, high_limit = bridge_prefetch_window(window_base, window_size)
    pci_write(qt, bus, dev, fn, 0x24, low)
    pci_write(qt, bus, dev, fn, 0x28, high_base)
    pci_write(qt, bus, dev, fn, 0x2C, high_limit)
    command = pci_read(qt, bus, dev, fn, 0x04)
    pci_write(qt, bus, dev, fn, 0x04, command | 0x6)


def map_type2_bar2(qt: QTest, bar2_base: int, bar2_size: int) -> int:
    devices = scan_pci(qt)
    root_ports = [d for d in devices if d[0] == 0x0C and d[6] == 0x060400]
    if not root_ports:
        raise RuntimeError("CXL root port not found on bus 0x0c")

    for bus, dev, fn, *_ in root_ports:
        configure_pci_bridge(qt, bus, dev, fn, 0x0C, 0x0D, 0x0D,
                             bar2_base, bar2_size)

    devices = scan_pci(qt)
    matches = [d for d in devices if d[3] == TYPE2_VENDOR_ID and d[4] == TYPE2_DEVICE_ID]
    if not matches:
        raise RuntimeError("CXL Type2 endpoint not found after root-port setup")

    bus, dev, fn, *_ = matches[0]
    pci_write(qt, bus, dev, fn, 0x18, bar2_base & 0xFFFFFFFF)
    pci_write(qt, bus, dev, fn, 0x1C, (bar2_base >> 32) & 0xFFFFFFFF)
    command = pci_read(qt, bus, dev, fn, 0x04)
    pci_write(qt, bus, dev, fn, 0x04, command | 0x6)

    magic = qt.readl(bar2_base + CXL_GPU_REG_MAGIC)
    caps = qt.readl(bar2_base + CXL_GPU_REG_CAPS)
    expect("qemu_bar2_magic", f"0x{magic:08x}", f"0x{CXL_GPU_MAGIC:08x}")
    if (caps & CXL_GPU_CAP_SWITCH_CORES) == 0:
        raise RuntimeError(f"BAR2 caps missing switch-core bit: 0x{caps:x}")
    print(f"PASS qemu_bar2_caps: 0x{caps:x}")
    return bar2_base


def map_type2_bar2_through_switch(qt: QTest, bar2_base: int,
                                  bar2_size: int) -> int:
    """Enumerate root port -> CXL upstream -> downstream -> Type2.

    Qtest does not run firmware, so the three bridge levels have to be given
    bus numbers and prefetchable windows explicitly before BAR2 can be used.
    """
    devices = scan_pci(qt)
    root_ports = [d for d in devices if d[0] == 0x0C and d[6] == 0x060400]
    if len(root_ports) != 1:
        raise RuntimeError(f"expected one CXL root port on bus 0x0c, got {root_ports}")

    bus, dev, fn, *_ = root_ports[0]
    configure_pci_bridge(qt, bus, dev, fn, 0x0C, 0x0D, 0x0F,
                         bar2_base, bar2_size)

    upstream = [d for d in scan_pci(qt) if d[0] == 0x0D and d[6] == 0x060400]
    if len(upstream) != 1:
        raise RuntimeError(f"expected one CXL upstream port on bus 0x0d, got {upstream}")
    bus, dev, fn, *_ = upstream[0]
    configure_pci_bridge(qt, bus, dev, fn, 0x0D, 0x0E, 0x0F,
                         bar2_base, bar2_size)

    downstream = [d for d in scan_pci(qt) if d[0] == 0x0E and d[6] == 0x060400]
    if len(downstream) != 1:
        raise RuntimeError(f"expected one CXL downstream port on bus 0x0e, got {downstream}")
    bus, dev, fn, *_ = downstream[0]
    configure_pci_bridge(qt, bus, dev, fn, 0x0E, 0x0F, 0x0F,
                         bar2_base, bar2_size)

    matches = [d for d in scan_pci(qt)
               if d[3] == TYPE2_VENDOR_ID and d[4] == TYPE2_DEVICE_ID]
    if len(matches) != 1:
        raise RuntimeError(f"expected one CXL Type2 endpoint behind switch, got {matches}")

    bus, dev, fn, *_ = matches[0]
    pci_write(qt, bus, dev, fn, 0x18, bar2_base & 0xFFFFFFFF)
    pci_write(qt, bus, dev, fn, 0x1C, (bar2_base >> 32) & 0xFFFFFFFF)
    command = pci_read(qt, bus, dev, fn, 0x04)
    pci_write(qt, bus, dev, fn, 0x04, command | 0x6)

    magic = qt.readl(bar2_base + CXL_GPU_REG_MAGIC)
    caps = qt.readl(bar2_base + CXL_GPU_REG_CAPS)
    expect("qemu_switch_bar2_magic", f"0x{magic:08x}", f"0x{CXL_GPU_MAGIC:08x}")
    if (caps & CXL_GPU_CAP_SWITCH_CORES) == 0:
        raise RuntimeError(f"BAR2 caps missing switch-core bit: 0x{caps:x}")
    print(f"PASS qemu_switch_bar2_caps: 0x{caps:x}")
    return bar2_base


def qemu_gpu_cmd(qt: QTest, bar2: int, cmd: int, params: list[int]) -> tuple[int, int]:
    for idx in range(8):
        value = params[idx] if idx < len(params) else 0
        qt.writeq(bar2 + CXL_GPU_REG_PARAM0 + idx * 8, value)
    qt.writel(bar2 + CXL_GPU_REG_CMD, cmd)

    status = qt.readl(bar2 + CXL_GPU_REG_CMD_STATUS)
    result = qt.readl(bar2 + CXL_GPU_REG_CMD_RESULT)
    if status != CXL_GPU_CMD_STATUS_COMPLETE or result != CXL_GPU_SUCCESS:
        raise RuntimeError(f"QEMU GPU cmd 0x{cmd:x} failed: status={status} result={result}")
    return qt.readq(bar2 + CXL_GPU_REG_RESULT0), qt.readq(bar2 + CXL_GPU_REG_RESULT0 + 8)


def listen_unix(path: Path) -> socket.socket:
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(path))
    listener.listen(1)
    return listener


def launch_qemu(args, qtest_path: Path, log_path: Path) -> tuple[subprocess.Popen, QTest, object]:
    listener = listen_unix(qtest_path)
    log = open(log_path, "wb")
    proc = None
    try:
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=log)
        listener.settimeout(10)
        conn, _ = listener.accept()
    except Exception:
        stop_process(proc)
        log.close()
        raise
    finally:
        listener.close()
    qt = QTest(conn)
    endianness = qt.cmd("endianness")
    expect("qtest_endianness", endianness[1], "little")
    return proc, qt, log


def stop_process(proc: subprocess.Popen | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=10125)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--run-dir", default=str(repo / "build" / "qtest-switch-offload"))
    parser.add_argument("--server", default=str(repo / "build" / "cxlmemsim_server"))
    parser.add_argument("--qemu", default=str(repo / "lib" / "qemu" / "build" / "qemu-system-x86_64"))
    parser.add_argument("--bar2-base", type=lambda v: int(v, 0), default=0x80000000)
    parser.add_argument("--through-switch", action="store_true",
                        help="place Type2 behind QEMU CXL upstream/downstream switch ports")
    args = parser.parse_args()

    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    server_log_path = run_dir / "cxlmemsim-server.log"
    qemu_log_path = run_dir / "qemu-qtest.log"

    server_log = open(server_log_path, "wb")
    server_proc = subprocess.Popen([
        args.server,
        "--comm-mode=tcp",
        f"--port={args.port}",
        "--capacity=64",
        "--default_latency=100",
        "--enable-switch-cores",
        "--switch-general-cores=2",
        "--switch-ai-cores=1",
        "--switch-general-latency=40",
        "--switch-ai-latency=30",
        "--switch-hwjit-lanes=1",
        "--switch-hwjit-latency=8",
        "--switch-hwjit-state-latency=4",
        "--switch-general-bandwidth=64",
        "--switch-ai-ops-per-ns=128",
        "--switch-hwjit-bandwidth=256",
        "--switch-hwjit-ops-per-ns=512",
        "--verbose=1",
    ], stdout=server_log, stderr=subprocess.STDOUT)

    qemu_proc = None
    qemu_log = None
    qt = None
    client = None
    try:
        wait_for_port(args.host, args.port)
        client = MemSimClient(args.host, args.port)

        reduce_addr = 0
        a_addr = 64
        b_addr = 128
        copy_dst = 192
        memset_dst = 224
        matmul_dst = 256
        hwjit_src = 320
        hwjit_dst = 384

        client.write64(reduce_addr, struct.pack("<4Q", 1, 2, 3, 4))
        client.write64(a_addr, struct.pack("<4i", 1, 2, 3, 4))
        client.write64(b_addr, struct.pack("<4i", 5, 6, 7, 8))
        client.write64(copy_dst, b"\0" * 64)
        client.write64(matmul_dst, b"\0" * 64)
        hwjit_payload = bytes(range(64))
        client.write64(hwjit_src, hwjit_payload)
        client.write64(hwjit_dst, b"\0" * 64)

        before = client.stats()
        expect("switch_enabled", before["enabled"], 1)

        qtest_dir = Path(tempfile.mkdtemp(prefix="qtest-", dir=run_dir))
        qtest_path = qtest_dir / "qtest.sock"
        topology_args = [
            "-device", "cxl-rp,port=0,bus=cxl.0,id=type2_rp,chassis=0,slot=2",
        ]
        type2_bus = "type2_rp"
        if args.through_switch:
            topology_args += [
                "-device", "cxl-upstream,port=0,sn=1234,bus=type2_rp,id=type2_us",
                "-device", "cxl-downstream,port=0,bus=type2_us,id=type2_ds,slot=3",
            ]
            type2_bus = "type2_ds"

        qemu_args = [
            args.qemu,
            "-qtest", f"unix:{qtest_path}",
            "-qtest-log", "/dev/null",
            "-display", "none",
            "-audio", "none",
            "-machine", "q35,cxl=on,cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=1G",
            "-m", "1G",
            "-smp", "1",
            "-nodefaults",
            "-accel", "qtest",
            "-device", "pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0",
            *topology_args,
            "-device",
            (
                f"cxl-type2,bus={type2_bus},id=cxl-type2-qtest,sn=201,gpu-mode=0,"
                "cache-size=16M,mem-size=64M,cxlmemsim-addr=127.0.0.1,"
                f"cxlmemsim-port={args.port},coherency-enabled=true,dcd=on,"
                "dcd-granularity=1M,dcd-initial-size=64M,gfam=on,gfam-hosts=4,"
                "gfam-host-id=0,mhsld=on,mhsld-heads=4,mhsld-head-id=0"
            ),
        ]
        qemu_proc, qt, qemu_log = launch_qemu(qemu_args, qtest_path, qemu_log_path)
        if args.through_switch:
            bar2 = map_type2_bar2_through_switch(
                qt, args.bar2_base, 16 * 1024 * 1024)
        else:
            bar2 = map_type2_bar2(qt, args.bar2_base, 16 * 1024 * 1024)

        result, latency = qemu_gpu_cmd(qt, bar2, CXL_GPU_CMD_SWITCH_REDUCE_ADD64, [reduce_addr, 32])
        expect("qemu_reduce_add64", result, 10)
        print(f"LATENCY qemu_reduce_add64_ns={latency}")

        result, latency = qemu_gpu_cmd(qt, bar2, CXL_GPU_CMD_SWITCH_DOT_I32, [a_addr, b_addr, 4])
        expect("qemu_dot_i32", result, 70)
        print(f"LATENCY qemu_dot_i32_ns={latency}")

        result, latency = qemu_gpu_cmd(qt, bar2, CXL_GPU_CMD_SWITCH_MEMCPY, [copy_dst, a_addr, 16])
        expect("qemu_memcpy_bytes", result, 16)
        expect("qemu_memcpy_payload", list(struct.unpack("<4i", client.read(copy_dst, 16))), [1, 2, 3, 4])
        print(f"LATENCY qemu_memcpy_ns={latency}")

        result, latency = qemu_gpu_cmd(qt, bar2, CXL_GPU_CMD_SWITCH_MEMSET, [memset_dst, 0xAB, 16])
        expect("qemu_memset_bytes", result, 16)
        expect("qemu_memset_payload", client.read(memset_dst, 16), b"\xAB" * 16)
        print(f"LATENCY qemu_memset_ns={latency}")

        result, latency = qemu_gpu_cmd(qt, bar2, CXL_GPU_CMD_SWITCH_MATMUL_I32,
                                       [a_addr, b_addr, matmul_dst, 2, 2, 2, 0])
        expect("qemu_matmul_elems", result, 4)
        expect("qemu_matmul_payload", list(struct.unpack("<4i", client.read(matmul_dst, 16))),
               [19, 22, 43, 50])
        print(f"LATENCY qemu_matmul_ns={latency}")

        result, latency = qemu_gpu_cmd(
            qt,
            bar2,
            CXL_GPU_CMD_SWITCH_HWJIT,
            [
                hwjit_src,
                hwjit_dst,
                len(hwjit_payload),
                DAMER_QWEN27B_KV_PACK,
                DAMER_HWJIT_QUANTIZE,
                len(hwjit_payload) // 64,
                pack_hwjit_control(),
                0,
            ],
        )
        expected_hwjit = hwjit_quantize(hwjit_payload)
        expect("qemu_hwjit_kv_pack_bytes", result, len(expected_hwjit))
        expect("qemu_hwjit_kv_pack_payload", client.read(hwjit_dst, len(expected_hwjit)),
               expected_hwjit)
        print(f"LATENCY qemu_hwjit_kv_pack_ns={latency}")

        qemu_gpu_cmd(qt, bar2, CXL_GPU_CMD_SWITCH_GET_STATS, [])
        stats_words = [qt.readq(bar2 + CXL_GPU_DATA_OFFSET + i * 8) for i in range(8)]
        after = {
            "general_ops": stats_words[0],
            "ai_ops": stats_words[1],
            "general_bytes": stats_words[2],
            "ai_bytes": stats_words[3],
            "queued_ns": stats_words[4],
            "service_ns": stats_words[5],
            "general_cores": stats_words[6],
            "ai_cores": stats_words[7],
        }
        expect("qemu_stats_general_ops_delta", after["general_ops"] - before["general_ops"], 3)
        expect("qemu_stats_ai_ops_delta", after["ai_ops"] - before["ai_ops"], 2)
        expect("qemu_stats_general_bytes_delta", after["general_bytes"] - before["general_bytes"], 64)
        expect("qemu_stats_ai_bytes_delta", after["ai_bytes"] - before["ai_bytes"], 80)
        qemu_gpu_cmd(qt, bar2, CXL_GPU_CMD_SWITCH_HWJIT_STATS, [])
        hwjit_words = [qt.readq(bar2 + CXL_GPU_DATA_OFFSET + i * 8) for i in range(8)]
        expect("qemu_stats_hwjit_ops", hwjit_words[0], 1)
        expect("qemu_stats_hwjit_commands", hwjit_words[1], 1)
        expect("qemu_stats_hwjit_bytes", hwjit_words[2], len(hwjit_payload))
        print(
            "STATS "
            f"qemu_queued_ns_delta={after['queued_ns'] - before['queued_ns']} "
            f"qemu_service_ns_delta={after['service_ns'] - before['service_ns']} "
            f"qemu_hwjit_service_ns={hwjit_words[5]}"
        )
        print(f"LOG server={server_log_path}")
        print(f"LOG qemu={qemu_log_path}")
        return 0
    finally:
        if qt:
            qt.close()
        if client:
            client.close()
        stop_process(qemu_proc)
        if qemu_log is not None:
            qemu_log.close()
        stop_process(server_proc)
        server_log.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        raise
