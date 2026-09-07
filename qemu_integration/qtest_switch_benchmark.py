#!/usr/bin/env python3
"""Benchmark QEMU Type2 near-switch offloads with Damer-inspired cases."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import qtest_switch_offload as qtest


STAT_KEYS = (
    "general_ops",
    "ai_ops",
    "general_bytes",
    "ai_bytes",
    "hw_jit_ops",
    "hw_jit_commands",
    "hw_jit_bytes",
    "hw_jit_output_bytes",
    "hw_jit_work_items",
    "hw_jit_service_ns",
    "queued_ns",
    "service_ns",
)


@dataclass(frozen=True)
class CaseSpec:
    name: str
    usecase: str
    damer_key: str
    func: Callable[["BenchmarkContext"], dict[str, object]]


class BenchmarkContext:
    def __init__(self, client: qtest.MemSimClient, qt: qtest.QTest, bar2: int,
                 quick: bool, profile: dict[str, object | None] | None = None) -> None:
        self.client = client
        self.qt = qt
        self.bar2 = bar2
        self.quick = quick
        self.profile = profile or {}
        self.next_addr = 0x10000
        self.commands: list[dict[str, object]] = []

    @staticmethod
    def align_up(value: int, alignment: int = 64) -> int:
        return ((value + alignment - 1) // alignment) * alignment

    def alloc(self, size: int, alignment: int = 64) -> int:
        addr = self.align_up(self.next_addr, alignment)
        self.next_addr = addr + self.align_up(size, 64)
        if self.next_addr >= 64 * 1024 * 1024:
            raise RuntimeError("benchmark address allocator exceeded 64 MiB memory")
        return addr

    def write_bytes(self, addr: int, payload: bytes) -> None:
        for offset in range(0, len(payload), 64):
            self.client.write64(addr + offset, payload[offset:offset + 64])

    def read_bytes(self, addr: int, size: int) -> bytes:
        chunks = []
        for offset in range(0, size, 64):
            chunk_size = min(64, size - offset)
            chunks.append(self.client.read(addr + offset, chunk_size))
        return b"".join(chunks)

    def read_bytes_with_latency(self, addr: int, size: int) -> tuple[bytes, int]:
        chunks = []
        latency_ns = 0
        for offset in range(0, size, 64):
            chunk_size = min(64, size - offset)
            latency, _, data = self.client.request(qtest.OP_READ, addr=addr + offset,
                                                   size=chunk_size)
            chunks.append(data[:chunk_size])
            latency_ns += latency
        return b"".join(chunks), latency_ns

    def write_bytes_with_latency(self, addr: int, payload: bytes) -> int:
        latency_ns = 0
        for offset in range(0, len(payload), 64):
            chunk = payload[offset:offset + 64]
            latency, _, _ = self.client.request(qtest.OP_WRITE, addr=addr + offset,
                                                size=len(chunk), data=chunk)
            latency_ns += latency
        return latency_ns

    def offload(self, label: str, cmd: int, params: list[int]) -> tuple[int, int]:
        result, latency_ns = qtest.qemu_gpu_cmd(self.qt, self.bar2, cmd, params)
        self.commands.append({
            "label": label,
            "cmd": f"0x{cmd:x}",
            "params": params,
            "result": result,
            "latency_ns": latency_ns,
        })
        return result, latency_ns

    def profile_int(self, name: str, fallback: int) -> int:
        value = self.profile.get(name)
        return fallback if value is None else int(value)

    def profile_str(self, name: str, fallback: str) -> str:
        value = self.profile.get(name)
        return fallback if value is None else str(value)


def pack_i32(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}i", *values)


def unpack_i32(payload: bytes) -> list[int]:
    if not payload:
        return []
    return list(struct.unpack(f"<{len(payload) // 4}i", payload))


def pack_u64(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}Q", *values)


def deterministic_bytes(size: int, seed: int) -> bytes:
    return bytes(((idx * 17 + seed) & 0xFF) for idx in range(size))


def ceil_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def checksum64(payload: bytes) -> int:
    acc = 0xCBF29CE484222325
    for byte in payload:
        acc ^= byte
        acc = (acc * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return acc


def hwjit_transform_payload(payload: bytes, transform_mask: int) -> tuple[bytes, int]:
    current = payload
    scalar = len(current)

    if transform_mask & qtest.DAMER_HWJIT_QUANTIZE:
        current = bytes(
            (current[idx] + (current[idx + 1] if idx + 1 < len(current) else 0)) // 2
            for idx in range(0, len(current), 2)
        )

    if transform_mask & qtest.DAMER_HWJIT_COMPRESS:
        current = current[::2]

    if transform_mask & qtest.DAMER_HWJIT_FILTER:
        filtered = bytes(byte for byte in current if (byte & 1) == 0)
        current = filtered or current[:1]

    if transform_mask & qtest.DAMER_HWJIT_SCATTER_GATHER:
        chunks = []
        for offset in range(0, len(current), 64):
            chunks.append(current[offset:offset + 64][::-1])
        current = b"".join(chunks)

    if transform_mask & qtest.DAMER_HWJIT_REPLICATE:
        current = bytes(
            current[idx ^ 1] if (idx ^ 1) < len(current) else current[idx]
            for idx in range(len(current))
        )

    if transform_mask & qtest.DAMER_HWJIT_REDUCE:
        total = 0
        for offset in range(0, len(current), 8):
            total = (total + int.from_bytes(current[offset:offset + 8].ljust(8, b"\0"), "little")) & 0xFFFFFFFFFFFFFFFF
        current = total.to_bytes(8, "little")
        scalar = total
    elif transform_mask & qtest.DAMER_HWJIT_CHECKSUM:
        scalar = checksum64(current)
    else:
        scalar = len(current)

    return current, scalar


def run_host_transform_baseline(ctx: BenchmarkContext, src: int, dst: int, size: int,
                                transform_mask: int) -> dict[str, object]:
    payload, read_latency_ns = ctx.read_bytes_with_latency(src, size)
    output, scalar = hwjit_transform_payload(payload, transform_mask)
    write_latency_ns = ctx.write_bytes_with_latency(dst, output)
    cpu_latency_ns = ceil_div(max(size, len(output)), 32)
    return {
        "output": output,
        "scalar": scalar,
        "latency_ns": read_latency_ns + write_latency_ns + cpu_latency_ns,
        "read_latency_ns": read_latency_ns,
        "write_latency_ns": write_latency_ns,
        "cpu_latency_ns": cpu_latency_ns,
    }


def run_hwjit_switchlet(ctx: BenchmarkContext, label: str, src: int, dst: int,
                        size: int, switchlet_id: int, transform_mask: int,
                        tile_count: int | None = None) -> tuple[int, int]:
    tiles = tile_count if tile_count is not None else max(1, ceil_div(size, 64))
    return ctx.offload(
        label,
        qtest.CXL_GPU_CMD_SWITCH_HWJIT,
        [
            src,
            dst,
            size,
            switchlet_id,
            transform_mask,
            tiles,
            qtest.pack_hwjit_control(),
            0,
        ],
    )


def matmul_expected(a: list[int], b: list[int], m: int, n: int, k: int) -> list[int]:
    out = []
    for row in range(m):
        for col in range(n):
            acc = 0
            for idx in range(k):
                acc += a[row * k + idx] * b[idx * n + col]
            out.append(acc)
    return out


def stats_from_bar(ctx: BenchmarkContext) -> dict[str, int]:
    enabled, _ = qtest.qemu_gpu_cmd(
        ctx.qt, ctx.bar2, qtest.CXL_GPU_CMD_SWITCH_GET_STATS, [])
    words = [
        ctx.qt.readq(ctx.bar2 + qtest.CXL_GPU_DATA_OFFSET + idx * 8)
        for idx in range(8)
    ]
    stats = {
        "enabled": enabled,
        "general_ops": words[0],
        "ai_ops": words[1],
        "general_bytes": words[2],
        "ai_bytes": words[3],
        "queued_ns": words[4],
        "service_ns": words[5],
        "general_cores": words[6],
        "ai_cores": words[7],
    }
    qtest.qemu_gpu_cmd(ctx.qt, ctx.bar2, qtest.CXL_GPU_CMD_SWITCH_HWJIT_STATS, [])
    hwjit_words = [
        ctx.qt.readq(ctx.bar2 + qtest.CXL_GPU_DATA_OFFSET + idx * 8)
        for idx in range(8)
    ]
    stats.update({
        "hw_jit_ops": hwjit_words[0],
        "hw_jit_commands": hwjit_words[1],
        "hw_jit_bytes": hwjit_words[2],
        "hw_jit_output_bytes": hwjit_words[3],
        "hw_jit_work_items": hwjit_words[4],
        "hw_jit_service_ns": hwjit_words[5],
        "hw_jit_lanes": hwjit_words[6],
        "hw_jit_switchlets": hwjit_words[7],
    })
    return stats


def run_matmul(ctx: BenchmarkContext, label: str, m: int, n: int, k: int,
               seed: int) -> dict[str, object]:
    a_vals = [((idx + seed) % 11) + 1 for idx in range(m * k)]
    b_vals = [((idx * 3 + seed) % 7) + 1 for idx in range(k * n)]
    c_size = m * n * 4
    a_addr = ctx.alloc(len(a_vals) * 4)
    b_addr = ctx.alloc(len(b_vals) * 4)
    c_addr = ctx.alloc(c_size)
    ctx.write_bytes(a_addr, pack_i32(a_vals))
    ctx.write_bytes(b_addr, pack_i32(b_vals))
    ctx.write_bytes(c_addr, b"\0" * c_size)

    result, _ = ctx.offload(
        label,
        qtest.CXL_GPU_CMD_SWITCH_MATMUL_I32,
        [a_addr, b_addr, c_addr, m, n, k, 0],
    )
    expected = matmul_expected(a_vals, b_vals, m, n, k)
    actual = unpack_i32(ctx.read_bytes(c_addr, c_size))
    if result != m * n or actual != expected:
        raise RuntimeError(f"{label}: matmul verification failed")
    return {
        "bytes": (len(a_vals) + len(b_vals) + len(expected)) * 4,
        "work_items": m * n * k,
        "checksum": sum(actual),
        "addr": c_addr,
        "values": actual,
    }


def run_ternary_matmul(ctx: BenchmarkContext, label: str, m: int, n: int, k: int,
                       seed: int) -> dict[str, object]:
    a_vals = [((idx + seed) % 3) - 1 for idx in range(m * k)]
    b_vals = [((idx * 5 + seed) % 3) - 1 for idx in range(k * n)]
    c_size = m * n * 4
    a_addr = ctx.alloc(len(a_vals) * 4)
    b_addr = ctx.alloc(len(b_vals) * 4)
    c_addr = ctx.alloc(c_size)
    ctx.write_bytes(a_addr, pack_i32(a_vals))
    ctx.write_bytes(b_addr, pack_i32(b_vals))
    ctx.write_bytes(c_addr, b"\0" * c_size)

    result, _ = ctx.offload(
        label,
        qtest.CXL_GPU_CMD_SWITCH_MATMUL_I32,
        [a_addr, b_addr, c_addr, m, n, k, 0],
    )
    expected = matmul_expected(a_vals, b_vals, m, n, k)
    actual = unpack_i32(ctx.read_bytes(c_addr, c_size))
    if result != m * n or actual != expected:
        raise RuntimeError(f"{label}: ternary matmul verification failed")
    return {
        "bytes": (len(a_vals) + len(b_vals) + len(expected)) * 4,
        "work_items": m * n * k,
        "checksum": sum(abs(value) for value in actual),
        "addr": c_addr,
        "values": actual,
    }


def nccl_hook_allgather(ctx: BenchmarkContext, label: str, ranks: int,
                        shard_bytes: int, seed: int, mode: str) -> dict[str, object]:
    if mode == "full":
        dst_ranks = ranks
    elif mode == "single-dst":
        dst_ranks = 1
    else:
        raise RuntimeError(f"unknown all-gather ablation mode {mode!r}")

    src_addrs = []
    src_payloads = []
    for rank in range(ranks):
        payload = deterministic_bytes(shard_bytes, seed + rank * 29)
        addr = ctx.alloc(shard_bytes)
        ctx.write_bytes(addr, payload)
        src_addrs.append(addr)
        src_payloads.append(payload)

    expected = b"".join(src_payloads)
    checksum = 0
    for dst_rank in range(dst_ranks):
        dst_base = ctx.alloc(ranks * shard_bytes)
        ctx.write_bytes(dst_base, b"\0" * ranks * shard_bytes)
        for src_rank, src_addr in enumerate(src_addrs):
            ctx.offload(
                f"{label}_r{src_rank}_to_r{dst_rank}",
                qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [dst_base + src_rank * shard_bytes, src_addr, shard_bytes],
            )
        actual = ctx.read_bytes(dst_base, ranks * shard_bytes)
        if actual != expected:
            raise RuntimeError(f"{label}: all-gather verification failed")
        checksum += sum(actual)

    moved = dst_ranks * ranks * shard_bytes
    return {
        "bytes": moved,
        "work_items": moved // 64,
        "checksum": checksum,
        "collectives": 1,
        "fanout_copies": dst_ranks * ranks,
    }


def nccl_hook_allreduce_checksum(ctx: BenchmarkContext, label: str, ranks: int,
                                 count_u64: int, seed: int, mode: str) -> dict[str, object]:
    if mode not in {"full", "reduce-only"}:
        raise RuntimeError(f"unknown all-reduce ablation mode {mode!r}")

    values = []
    for rank in range(ranks):
        values.extend(((idx + rank + seed) % 23) + 1 for idx in range(count_u64))
    payload = pack_u64(values)
    input_addr = ctx.alloc(len(payload))
    ctx.write_bytes(input_addr, payload)

    result, _ = ctx.offload(
        f"{label}_reduce",
        qtest.CXL_GPU_CMD_SWITCH_REDUCE_ADD64,
        [input_addr, len(payload)],
    )
    expected = sum(values)
    if result != expected:
        raise RuntimeError(f"{label}: all-reduce expected {expected}, got {result}")

    broadcast_copies = 0
    if mode == "full":
        # The qtest command returns scalar reductions in BAR registers. Stage
        # that scalar once, then model the NCCL hook broadcast through switch
        # copies. reduce-only mode intentionally skips this staging/fanout path.
        scratch = ctx.alloc(64)
        ctx.write_bytes(scratch, struct.pack("<Q", result).ljust(64, b"\0"))
        for rank in range(ranks):
            dst = ctx.alloc(64)
            ctx.write_bytes(dst, b"\0" * 64)
            ctx.offload(
                f"{label}_broadcast_r{rank}",
                qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [dst, scratch, 64],
            )
            actual = struct.unpack("<Q", ctx.read_bytes(dst, 8))[0]
            if actual != result:
                raise RuntimeError(f"{label}: broadcast verification failed")
        broadcast_copies = ranks

    return {
        "bytes": len(payload) + broadcast_copies * 64,
        "work_items": ranks * count_u64,
        "checksum": result,
        "collectives": 1,
        "broadcast_copies": broadcast_copies,
    }


def case_general_memcpy(ctx: BenchmarkContext) -> dict[str, object]:
    size = 1024 if ctx.quick else 4096
    src = ctx.alloc(size)
    dst = ctx.alloc(size)
    payload = deterministic_bytes(size, 13)
    ctx.write_bytes(src, payload)
    ctx.write_bytes(dst, b"\0" * size)
    result, _ = ctx.offload("memcpy", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                            [dst, src, size])
    if result != size or ctx.read_bytes(dst, size) != payload:
        raise RuntimeError("general_memcpy: payload mismatch")
    return {"bytes": size, "work_items": size // 64, "result": f"bytes={result}"}


def case_general_memset(ctx: BenchmarkContext) -> dict[str, object]:
    size = 1024 if ctx.quick else 4096
    dst = ctx.alloc(size)
    pattern = 0x5A
    ctx.write_bytes(dst, b"\0" * size)
    result, _ = ctx.offload("memset", qtest.CXL_GPU_CMD_SWITCH_MEMSET,
                            [dst, pattern, size])
    if result != size or ctx.read_bytes(dst, size) != bytes([pattern]) * size:
        raise RuntimeError("general_memset: payload mismatch")
    return {"bytes": size, "work_items": size // 64, "result": f"bytes={result}"}


def case_general_reduce(ctx: BenchmarkContext) -> dict[str, object]:
    count = 128 if ctx.quick else 512
    values = [(idx % 31) + 1 for idx in range(count)]
    size = count * 8
    src = ctx.alloc(size)
    ctx.write_bytes(src, pack_u64(values))
    result, _ = ctx.offload("reduce_add64",
                            qtest.CXL_GPU_CMD_SWITCH_REDUCE_ADD64,
                            [src, size])
    expected = sum(values)
    if result != expected:
        raise RuntimeError(f"general_reduce: expected {expected}, got {result}")
    return {"bytes": size, "work_items": count, "result": f"sum={result}"}


def case_ai_dot(ctx: BenchmarkContext) -> dict[str, object]:
    count = 64 if ctx.quick else 256
    a_vals = [(idx % 13) + 1 for idx in range(count)]
    b_vals = [(idx % 7) + 1 for idx in range(count)]
    a_addr = ctx.alloc(count * 4)
    b_addr = ctx.alloc(count * 4)
    ctx.write_bytes(a_addr, pack_i32(a_vals))
    ctx.write_bytes(b_addr, pack_i32(b_vals))
    result, _ = ctx.offload("dot_i32", qtest.CXL_GPU_CMD_SWITCH_DOT_I32,
                            [a_addr, b_addr, count])
    expected = sum(a * b for a, b in zip(a_vals, b_vals))
    if result != expected:
        raise RuntimeError(f"ai_dot: expected {expected}, got {result}")
    return {
        "bytes": count * 8,
        "work_items": count,
        "result": f"dot={result}",
    }


def case_ai_gemm(ctx: BenchmarkContext) -> dict[str, object]:
    dim = 4 if ctx.quick else 8
    mm = run_matmul(ctx, "gemm_i32", dim, dim, dim, 5)
    return {
        "bytes": mm["bytes"],
        "work_items": mm["work_items"],
        "result": f"checksum={mm['checksum']}",
    }


def case_ternary_matmul_pipeline(ctx: BenchmarkContext) -> dict[str, object]:
    dim = 4 if ctx.quick else 8
    matrix_bytes = dim * dim * 4
    a_host = ctx.alloc(matrix_bytes)
    b_host = ctx.alloc(matrix_bytes)
    a_scratch = ctx.alloc(matrix_bytes)
    b_scratch = ctx.alloc(matrix_bytes)
    c_scratch = ctx.alloc(matrix_bytes)
    c_host = ctx.alloc(matrix_bytes)

    a_vals = [((idx + 2) % 5) + 1 for idx in range(dim * dim)]
    b_vals = [((idx * 2 + 3) % 7) + 1 for idx in range(dim * dim)]
    expected = matmul_expected(a_vals, b_vals, dim, dim, dim)

    ctx.write_bytes(a_host, pack_i32(a_vals))
    ctx.write_bytes(b_host, pack_i32(b_vals))
    for addr in (a_scratch, b_scratch, c_scratch, c_host):
        ctx.write_bytes(addr, b"\0" * matrix_bytes)

    ctx.offload("tmatmul_import_a", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [a_scratch, a_host, matrix_bytes])
    ctx.offload("tmatmul_import_b", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [b_scratch, b_host, matrix_bytes])
    result, _ = ctx.offload(
        "tmatmul_go",
        qtest.CXL_GPU_CMD_SWITCH_MATMUL_I32,
        [a_scratch, b_scratch, c_scratch, dim, dim, dim, 0],
    )
    ctx.offload("tmatmul_export", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [c_host, c_scratch, matrix_bytes])
    actual = unpack_i32(ctx.read_bytes(c_host, matrix_bytes))
    if result != dim * dim or actual != expected:
        raise RuntimeError("ternary_matmul_pipeline: verification failed")
    return {
        "bytes": matrix_bytes * 3 + matrix_bytes * 3,
        "work_items": dim * dim * dim,
        "result": f"checksum={sum(actual)}",
    }


def case_qwen_decode(ctx: BenchmarkContext) -> dict[str, object]:
    dim = 4 if ctx.quick else 8
    hidden_count = 64 if ctx.quick else 128
    hidden_vals = [(idx % 17) + 1 for idx in range(hidden_count)]
    hidden = ctx.alloc(hidden_count * 8)
    rope_scratch = ctx.alloc(hidden_count * 4)
    ctx.write_bytes(hidden, pack_u64(hidden_vals))
    ctx.write_bytes(rope_scratch, b"\0" * (hidden_count * 4))

    ctx.offload("decode_rope_scratch", qtest.CXL_GPU_CMD_SWITCH_MEMSET,
                [rope_scratch, 0, hidden_count * 4])
    rms, _ = ctx.offload("decode_rms_norm_reduce",
                         qtest.CXL_GPU_CMD_SWITCH_REDUCE_ADD64,
                         [hidden, hidden_count * 8])
    qkv = run_matmul(ctx, "decode_gemm_qkv", dim, dim, dim, 7)

    dot_count = 64 if ctx.quick else 128
    a_vals = [(idx % 9) + 1 for idx in range(dot_count)]
    b_vals = [((idx * 5) % 11) + 1 for idx in range(dot_count)]
    a_addr = ctx.alloc(dot_count * 4)
    b_addr = ctx.alloc(dot_count * 4)
    ctx.write_bytes(a_addr, pack_i32(a_vals))
    ctx.write_bytes(b_addr, pack_i32(b_vals))
    attention, _ = ctx.offload("decode_attention_dot",
                               qtest.CXL_GPU_CMD_SWITCH_DOT_I32,
                               [a_addr, b_addr, dot_count])
    expected_attention = sum(a * b for a, b in zip(a_vals, b_vals))
    if attention != expected_attention:
        raise RuntimeError("qwen_decode: attention dot mismatch")

    mlp = run_matmul(ctx, "decode_gemm_mlp_down", dim, dim, dim, 11)
    if rms != sum(hidden_vals):
        raise RuntimeError("qwen_decode: reduce mismatch")
    return {
        "bytes": hidden_count * 12 + qkv["bytes"] + dot_count * 8 + mlp["bytes"],
        "work_items": hidden_count + qkv["work_items"] + dot_count + mlp["work_items"],
        "result": f"rms={rms};attention={attention};checksum={qkv['checksum'] + mlp['checksum']}",
    }


def case_qwen_prefill(ctx: BenchmarkContext) -> dict[str, object]:
    dim = 4 if ctx.quick else 8
    kv_bytes = 512 if ctx.quick else 2048
    kv_host = ctx.alloc(kv_bytes)
    kv_scratch = ctx.alloc(kv_bytes)
    kv_out = ctx.alloc(kv_bytes)
    ctx.write_bytes(kv_host, deterministic_bytes(kv_bytes, 19))
    ctx.write_bytes(kv_scratch, b"\0" * kv_bytes)
    ctx.write_bytes(kv_out, b"\0" * kv_bytes)

    ctx.offload("prefill_load_kv_cache", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [kv_scratch, kv_host, kv_bytes])
    qkv = run_matmul(ctx, "prefill_gemm_qkv", dim, dim, dim, 13)
    scores = run_matmul(ctx, "prefill_attention_scores", dim, dim, dim, 17)

    score_u64 = [abs(value) for value in scores["values"]]
    score_addr = ctx.alloc(len(score_u64) * 8)
    ctx.write_bytes(score_addr, pack_u64(score_u64))
    softmax_den, _ = ctx.offload("prefill_softmax_reduce",
                                 qtest.CXL_GPU_CMD_SWITCH_REDUCE_ADD64,
                                 [score_addr, len(score_u64) * 8])
    if softmax_den != sum(score_u64):
        raise RuntimeError("qwen_prefill: softmax reduce mismatch")

    value = run_matmul(ctx, "prefill_gemm_attention_value", dim, dim, dim, 23)
    ctx.offload("prefill_store_kv_cache", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [kv_out, kv_scratch, kv_bytes])
    if ctx.read_bytes(kv_out, kv_bytes) != ctx.read_bytes(kv_host, kv_bytes):
        raise RuntimeError("qwen_prefill: KV cache store mismatch")
    return {
        "bytes": kv_bytes * 2 + qkv["bytes"] + scores["bytes"] +
        len(score_u64) * 8 + value["bytes"],
        "work_items": qkv["work_items"] + scores["work_items"] +
        len(score_u64) + value["work_items"],
        "result": f"softmax_den={softmax_den};checksum={qkv['checksum'] + scores['checksum'] + value['checksum']}",
    }


def case_hwjit_qwen27b_kv_pack(ctx: BenchmarkContext) -> dict[str, object]:
    size = ctx.profile_int("hwjit_kv_bytes", 1024 if ctx.quick else 8192)
    if size < 64 or size % 64 != 0:
        raise RuntimeError("HW-JIT KV pack size must be a positive 64-byte multiple")

    src = ctx.alloc(size)
    host_dst = ctx.alloc(size)
    hwjit_dst = ctx.alloc(size)
    payload = deterministic_bytes(size, 83)
    ctx.write_bytes(src, payload)
    ctx.write_bytes(host_dst, b"\0" * size)
    ctx.write_bytes(hwjit_dst, b"\0" * size)

    baseline = run_host_transform_baseline(
        ctx, src, host_dst, size, qtest.DAMER_HWJIT_QUANTIZE)
    result, hwjit_latency_ns = run_hwjit_switchlet(
        ctx,
        "hwjit_qwen27b_kv_pack",
        src,
        hwjit_dst,
        size,
        qtest.DAMER_QWEN27B_KV_PACK,
        qtest.DAMER_HWJIT_QUANTIZE,
    )
    expected = baseline["output"]
    actual = ctx.read_bytes(hwjit_dst, len(expected))
    if actual != expected or result != baseline["scalar"]:
        raise RuntimeError("hwjit_qwen27b_kv_pack: fused output mismatch")

    speedup = baseline["latency_ns"] / hwjit_latency_ns
    return {
        "bytes": size + len(expected),
        "work_items": size // 2,
        "baseline_latency_ns": baseline["latency_ns"],
        "hwjit_latency_ns": hwjit_latency_ns,
        "speedup": speedup,
        "baseline_method": "host_cxl_read_quantize_write",
        "hwjit_policy": "qwen27b_kv_pack",
        "result": (
            f"out_bytes={len(expected)};baseline_ns={baseline['latency_ns']};"
            f"hwjit_ns={hwjit_latency_ns};speedup={speedup:.2f}x"
        ),
    }


def case_hwjit_qwen27b_prefill_attention_ffn_e2e(ctx: BenchmarkContext) -> dict[str, object]:
    edge_specs = [
        ("prefill_activation_spill", qtest.DAMER_QWEN27B_PREFILL_ACTIVATION_SPILL,
         qtest.DAMER_HWJIT_COMPRESS, 1536 if ctx.quick else 12288, 97),
        ("decode_kv_fetch", qtest.DAMER_QWEN27B_DECODE_KV_FETCH,
         qtest.DAMER_HWJIT_CHECKSUM, 1024 if ctx.quick else 8192, 113),
        ("attention_mask_filter", qtest.DAMER_QWEN27B_ATTENTION_MASK_FILTER,
         qtest.DAMER_HWJIT_FILTER, 1024 if ctx.quick else 8192, 131),
        ("tp_logits_reduce", qtest.DAMER_QWEN27B_TP_LOGITS_REDUCE,
         qtest.DAMER_HWJIT_REDUCE, 1024 if ctx.quick else 8192, 149),
    ]

    baseline_latency_ns = 0
    hwjit_latency_ns = 0
    total_bytes = 0
    total_work = 0
    scalar_acc = 0

    for name, switchlet_id, transform_mask, size, seed in edge_specs:
        src = ctx.alloc(size)
        host_dst = ctx.alloc(size)
        hwjit_dst = ctx.alloc(size)
        payload = deterministic_bytes(size, seed)
        if transform_mask & qtest.DAMER_HWJIT_REDUCE:
            values = [((idx + seed) % 29) + 1 for idx in range(size // 8)]
            payload = pack_u64(values)
            size = len(payload)
        ctx.write_bytes(src, payload)
        ctx.write_bytes(host_dst, b"\0" * size)
        ctx.write_bytes(hwjit_dst, b"\0" * size)

        baseline = run_host_transform_baseline(ctx, src, host_dst, size,
                                               transform_mask)
        result, latency_ns = run_hwjit_switchlet(
            ctx,
            f"hwjit_qwen27b_{name}",
            src,
            hwjit_dst,
            size,
            switchlet_id,
            transform_mask,
        )
        expected = baseline["output"]
        actual = ctx.read_bytes(hwjit_dst, len(expected))
        if actual != expected or result != baseline["scalar"]:
            raise RuntimeError(f"hwjit_qwen27b_prefill_attention_ffn_e2e: {name} mismatch")

        baseline_latency_ns += int(baseline["latency_ns"])
        hwjit_latency_ns += latency_ns
        total_bytes += size + len(expected)
        total_work += max(size // 64, len(expected))
        scalar_acc = (scalar_acc + int(result)) & 0xFFFFFFFFFFFFFFFF

    speedup = baseline_latency_ns / hwjit_latency_ns
    return {
        "bytes": total_bytes,
        "work_items": total_work,
        "baseline_latency_ns": baseline_latency_ns,
        "hwjit_latency_ns": hwjit_latency_ns,
        "speedup": speedup,
        "baseline_method": "host_cxl_read_transform_write",
        "hwjit_policy": "qwen27b_prefill_attention_ffn_dataflow",
        "result": (
            f"edges={len(edge_specs)};baseline_ns={baseline_latency_ns};"
            f"hwjit_ns={hwjit_latency_ns};speedup={speedup:.2f}x;"
            f"scalar_acc={scalar_acc}"
        ),
    }


def case_graph_bfs_frontier(ctx: BenchmarkContext) -> dict[str, object]:
    frontier_bytes = 1024 if ctx.quick else 4096
    bitmap_bytes = 1024 if ctx.quick else 4096
    degree_count = 128 if ctx.quick else 512

    frontier_host = ctx.alloc(frontier_bytes)
    frontier_scratch = ctx.alloc(frontier_bytes)
    frontier_out = ctx.alloc(frontier_bytes)
    visited_bitmap = ctx.alloc(bitmap_bytes)
    degree_addr = ctx.alloc(degree_count * 8)

    frontier = deterministic_bytes(frontier_bytes, 31)
    degrees = [((idx * 7) % 29) + 1 for idx in range(degree_count)]
    ctx.write_bytes(frontier_host, frontier)
    ctx.write_bytes(frontier_scratch, b"\0" * frontier_bytes)
    ctx.write_bytes(frontier_out, b"\0" * frontier_bytes)
    ctx.write_bytes(visited_bitmap, b"\xff" * bitmap_bytes)
    ctx.write_bytes(degree_addr, pack_u64(degrees))

    ctx.offload("bfs_import_frontier", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [frontier_scratch, frontier_host, frontier_bytes])
    ctx.offload("bfs_clear_next_bitmap", qtest.CXL_GPU_CMD_SWITCH_MEMSET,
                [visited_bitmap, 0, bitmap_bytes])
    edge_total, _ = ctx.offload("bfs_reduce_frontier_degrees",
                                qtest.CXL_GPU_CMD_SWITCH_REDUCE_ADD64,
                                [degree_addr, degree_count * 8])
    ctx.offload("bfs_export_frontier", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [frontier_out, frontier_scratch, frontier_bytes])

    if ctx.read_bytes(frontier_out, frontier_bytes) != frontier:
        raise RuntimeError("graph_bfs_frontier: frontier export mismatch")
    if ctx.read_bytes(visited_bitmap, bitmap_bytes) != b"\0" * bitmap_bytes:
        raise RuntimeError("graph_bfs_frontier: bitmap clear mismatch")
    if edge_total != sum(degrees):
        raise RuntimeError("graph_bfs_frontier: degree reduce mismatch")

    return {
        "bytes": frontier_bytes * 2 + bitmap_bytes + degree_count * 8,
        "work_items": frontier_bytes // 32 + bitmap_bytes // 64 + degree_count,
        "result": f"edges={edge_total}",
    }


def case_hash_join_probe(ctx: BenchmarkContext) -> dict[str, object]:
    bucket_bytes = 1024 if ctx.quick else 4096
    hash_count = 128 if ctx.quick else 512
    predicate_count = 64 if ctx.quick else 256

    build_host = ctx.alloc(bucket_bytes)
    build_scratch = ctx.alloc(bucket_bytes)
    output = ctx.alloc(bucket_bytes)
    hash_addr = ctx.alloc(hash_count * 8)
    pred_a_addr = ctx.alloc(predicate_count * 4)
    pred_b_addr = ctx.alloc(predicate_count * 4)

    build = deterministic_bytes(bucket_bytes, 47)
    hashes = [((idx * 13) % 97) + 1 for idx in range(hash_count)]
    pred_a = [((idx + 3) % 11) - 5 for idx in range(predicate_count)]
    pred_b = [((idx * 5 + 7) % 13) - 6 for idx in range(predicate_count)]

    ctx.write_bytes(build_host, build)
    ctx.write_bytes(build_scratch, b"\0" * bucket_bytes)
    ctx.write_bytes(output, b"\xff" * bucket_bytes)
    ctx.write_bytes(hash_addr, pack_u64(hashes))
    ctx.write_bytes(pred_a_addr, pack_i32(pred_a))
    ctx.write_bytes(pred_b_addr, pack_i32(pred_b))

    ctx.offload("join_import_build_buckets", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [build_scratch, build_host, bucket_bytes])
    ctx.offload("join_clear_output", qtest.CXL_GPU_CMD_SWITCH_MEMSET,
                [output, 0, bucket_bytes])
    hash_total, _ = ctx.offload("join_reduce_hashes",
                                qtest.CXL_GPU_CMD_SWITCH_REDUCE_ADD64,
                                [hash_addr, hash_count * 8])
    predicate_raw, _ = ctx.offload("join_predicate_dot",
                                   qtest.CXL_GPU_CMD_SWITCH_DOT_I32,
                                   [pred_a_addr, pred_b_addr,
                                    predicate_count])
    # BAR2 result registers are unsigned 64-bit values.  DOT_I32 is signed,
    # so decode its two's-complement result before comparing negative scores.
    predicate_score = (predicate_raw if predicate_raw < (1 << 63)
                       else predicate_raw - (1 << 64))
    ctx.offload("join_materialize", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [output, build_scratch, bucket_bytes])

    if ctx.read_bytes(output, bucket_bytes) != build:
        raise RuntimeError("hash_join_probe: materialized output mismatch")
    if hash_total != sum(hashes):
        raise RuntimeError("hash_join_probe: hash reduce mismatch")
    expected_score = sum(a * b for a, b in zip(pred_a, pred_b))
    if predicate_score != expected_score:
        raise RuntimeError("hash_join_probe: predicate score mismatch")

    return {
        "bytes": bucket_bytes * 3 + hash_count * 8 + predicate_count * 8,
        "work_items": bucket_bytes // 32 + hash_count + predicate_count,
        "result": f"hash={hash_total};pred={predicate_score}",
    }


def case_kv_store_batch(ctx: BenchmarkContext) -> dict[str, object]:
    value_bytes = 1024 if ctx.quick else 4096
    version_count = 128 if ctx.quick else 512

    put_host = ctx.alloc(value_bytes)
    device_log = ctx.alloc(value_bytes)
    get_host = ctx.alloc(value_bytes)
    lease_words = ctx.alloc(version_count * 8)
    tombstone = ctx.alloc(64)

    value = deterministic_bytes(value_bytes, 59)
    versions = [1_000_000 + idx * 3 for idx in range(version_count)]
    ctx.write_bytes(put_host, value)
    ctx.write_bytes(device_log, b"\0" * value_bytes)
    ctx.write_bytes(get_host, b"\0" * value_bytes)
    ctx.write_bytes(lease_words, pack_u64(versions))
    ctx.write_bytes(tombstone, b"\xff" * 64)

    ctx.offload("kv_put_log_append", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [device_log, put_host, value_bytes])
    version_sum, _ = ctx.offload("kv_reduce_version_fence",
                                 qtest.CXL_GPU_CMD_SWITCH_REDUCE_ADD64,
                                 [lease_words, version_count * 8])
    ctx.offload("kv_clear_tombstone", qtest.CXL_GPU_CMD_SWITCH_MEMSET,
                [tombstone, 0, 64])
    ctx.offload("kv_get_payload", qtest.CXL_GPU_CMD_SWITCH_MEMCPY,
                [get_host, device_log, value_bytes])

    if ctx.read_bytes(get_host, value_bytes) != value:
        raise RuntimeError("kv_store_batch: get payload mismatch")
    if ctx.read_bytes(tombstone, 64) != b"\0" * 64:
        raise RuntimeError("kv_store_batch: tombstone clear mismatch")
    if version_sum != sum(versions):
        raise RuntimeError("kv_store_batch: version reduce mismatch")

    return {
        "bytes": value_bytes * 2 + version_count * 8 + 64,
        "work_items": value_bytes // 32 + version_count + 1,
        "result": f"version_sum={version_sum}",
    }


def case_kimi26_ternary_nccl_hook_e2e(ctx: BenchmarkContext) -> dict[str, object]:
    ranks = ctx.profile_int("kimi_ranks", 2 if ctx.quick else 4)
    layers = ctx.profile_int("kimi_layers", 1 if ctx.quick else 3)
    dim = ctx.profile_int("kimi_dim", 4 if ctx.quick else 8)
    kv_shard_bytes = ctx.profile_int("kimi_kv_shard_bytes",
                                     128 if ctx.quick else 256)
    reduce_count = ctx.profile_int("kimi_reduce_count",
                                   16 if ctx.quick else 64)
    allgather_mode = ctx.profile_str("kimi_allgather_mode", "full")
    allreduce_mode = ctx.profile_str("kimi_allreduce_mode", "full")
    if ranks < 1 or layers < 1 or dim < 1 or reduce_count < 1:
        raise RuntimeError("Kimi profile dimensions must be positive")
    if kv_shard_bytes < 64 or kv_shard_bytes % 64 != 0:
        raise RuntimeError("--kimi-kv-shard-bytes must be a positive 64-byte multiple")
    if allgather_mode not in {"full", "single-dst"}:
        raise RuntimeError("--kimi-allgather-mode must be full or single-dst")
    if allreduce_mode not in {"full", "reduce-only"}:
        raise RuntimeError("--kimi-allreduce-mode must be full or reduce-only")

    totals = {
        "bytes": 0,
        "work_items": 0,
        "checksum": 0,
        "collectives": 0,
        "ternary_matmuls": 0,
        "fanout_copies": 0,
        "broadcast_copies": 0,
    }

    def add(item: dict[str, object]) -> None:
        totals["bytes"] += int(item["bytes"])
        totals["work_items"] += int(item["work_items"])
        totals["checksum"] += int(item["checksum"])
        totals["collectives"] += int(item.get("collectives", 0))
        totals["fanout_copies"] += int(item.get("fanout_copies", 0))
        totals["broadcast_copies"] += int(item.get("broadcast_copies", 0))

    for layer in range(layers):
        add(nccl_hook_allgather(
            ctx,
            f"kimi26_l{layer}_nccl_hook_kv_allgather",
            ranks,
            kv_shard_bytes,
            41 + layer * 17,
            allgather_mode,
        ))

        for phase, seed in (
            ("qkv_ternary", 101),
            ("router_ternary", 211),
            ("moe_up_ternary", 307),
            ("moe_down_ternary", 401),
        ):
            add(run_ternary_matmul(
                ctx,
                f"kimi26_l{layer}_{phase}",
                dim,
                dim,
                dim,
                seed + layer * 13,
            ))
            totals["ternary_matmuls"] += 1

        add(nccl_hook_allreduce_checksum(
            ctx,
            f"kimi26_l{layer}_nccl_hook_attention_allreduce",
            ranks,
            reduce_count,
            503 + layer * 19,
            allreduce_mode,
        ))
        add(nccl_hook_allreduce_checksum(
            ctx,
            f"kimi26_l{layer}_nccl_hook_residual_allreduce",
            ranks,
            reduce_count,
            709 + layer * 23,
            allreduce_mode,
        ))

    return {
        "bytes": totals["bytes"],
        "work_items": totals["work_items"],
        "result": (
            f"layers={layers};ranks={ranks};ternary_matmuls="
            f"{totals['ternary_matmuls']};collectives={totals['collectives']};"
            f"allgather={allgather_mode};allreduce={allreduce_mode};"
            f"fanout_copies={totals['fanout_copies']};"
            f"broadcast_copies={totals['broadcast_copies']};"
            f"checksum={totals['checksum']}"
        ),
    }


def case_specs() -> list[CaseSpec]:
    return [
        CaseSpec("general_memcpy", "general core cacheline copy", "gemm",
                 case_general_memcpy),
        CaseSpec("general_memset", "general core cacheline fill", "synthetic",
                 case_general_memset),
        CaseSpec("general_reduce_add64", "general core reduction", "qwen_decode_token",
                 case_general_reduce),
        CaseSpec("ai_dot_i32", "AI core vector dot", "gemm", case_ai_dot),
        CaseSpec("ai_gemm_i32", "AI core GEMM tile", "gemm", case_ai_gemm),
        CaseSpec("mixed_ternary_matmul_pipeline",
                 "Damer ternary_matmul import/go/export",
                 "ternary_matmul", case_ternary_matmul_pipeline),
        CaseSpec("mixed_qwen_decode_token",
                 "Damer Qwen decode tensor pipeline",
                 "qwen_decode_token", case_qwen_decode),
        CaseSpec("mixed_qwen_prefill_gemm",
                 "Damer Qwen prefill KV/GEMM pipeline",
                 "qwen_prefill_gemm", case_qwen_prefill),
        CaseSpec("hwjit_qwen27b_kv_pack",
                 "Damer Hardware-JIT Qwen27B KV pack near CXL switch",
                 "qwen27b_kv_pack", case_hwjit_qwen27b_kv_pack),
        CaseSpec("hwjit_qwen27b_prefill_attention_ffn_e2e",
                 "Damer Hardware-JIT Qwen27B prefill/attention/FFN dataflow",
                 "qwen27b_prefill_attention_ffn",
                 case_hwjit_qwen27b_prefill_attention_ffn_e2e),
        CaseSpec("mixed_graph_bfs_frontier",
                 "Graph500-style frontier expansion",
                 "synthetic", case_graph_bfs_frontier),
        CaseSpec("mixed_hash_join_probe",
                 "in-memory hash-join probe pipeline",
                 "synthetic", case_hash_join_probe),
        CaseSpec("mixed_kv_store_batch",
                 "distributed KV get/put batch",
                 "synthetic", case_kv_store_batch),
        CaseSpec("kimi26_ternary_nccl_hook_e2e",
                 "Synthetic Kimi 2.6 ternary E2E with NCCL hook collectives",
                 "ternary_matmul", case_kimi26_ternary_nccl_hook_e2e),
    ]


def load_damer_traces(damer_root: Path) -> dict[str, dict[str, object]]:
    trace_dir = damer_root / "workloads" / "concordia" / "ptxspatial"
    traces: dict[str, dict[str, object]] = {}
    if not trace_dir.exists():
        return traces
    for path in sorted(trace_dir.glob("*.ptxspatial.json")):
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        key = path.name.removesuffix(".ptxspatial.json")
        workload = data.get("workload") or key
        events = data.get("events") or []
        item = {
            "workload": workload,
            "path": str(path),
            "event_count": len(events),
            "target_ip": data.get("target_ip", ""),
            "event_classes": sorted({event.get("class", "") for event in events}),
        }
        traces[key] = item
        traces[str(workload)] = item
    return traces


def damer_trace_fields(traces: dict[str, dict[str, object]],
                       key: str) -> dict[str, object]:
    trace = traces.get(key)
    if not trace:
        return {
            "damer_workload": key,
            "damer_trace": "synthetic",
            "damer_events": 0,
        }
    return {
        "damer_workload": trace["workload"],
        "damer_trace": trace["path"],
        "damer_events": trace["event_count"],
    }


def run_case(ctx: BenchmarkContext, spec: CaseSpec, iteration: int,
             traces: dict[str, dict[str, object]]) -> dict[str, object]:
    before = stats_from_bar(ctx)
    start = len(ctx.commands)
    details = spec.func(ctx)
    commands = ctx.commands[start:]
    after = stats_from_bar(ctx)
    delta = {key: after[key] - before[key] for key in STAT_KEYS}
    row: dict[str, object] = {
        "iteration": iteration,
        "case": spec.name,
        "usecase": spec.usecase,
        "commands": len(commands),
        "command_sequence": "+".join(str(cmd["label"]) for cmd in commands),
        "bytes": int(details["bytes"]),
        "work_items": int(details["work_items"]),
        "modeled_latency_ns": sum(int(cmd["latency_ns"]) for cmd in commands),
        "result": details["result"],
        "status": "PASS",
    }
    for key in (
        "baseline_latency_ns",
        "hwjit_latency_ns",
        "speedup",
        "baseline_method",
        "hwjit_policy",
    ):
        if key in details:
            row[key] = details[key]
    row.update(delta)
    row.update({
        "general_cores": after["general_cores"],
        "ai_cores": after["ai_cores"],
        "hw_jit_lanes": after["hw_jit_lanes"],
        "hw_jit_switchlets": after["hw_jit_switchlets"],
        "command_details": commands,
    })
    row.update(damer_trace_fields(traces, spec.damer_key))
    return row


def write_outputs(rows: list[dict[str, object]], run_dir: Path,
                  prefix: str, config: dict[str, object]) -> tuple[Path, Path]:
    json_path = run_dir / f"{prefix}.json"
    csv_path = run_dir / f"{prefix}.csv"
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump({"config": config, "results": rows}, fh, indent=2)

    fieldnames = [
        "iteration",
        "case",
        "usecase",
        "damer_workload",
        "damer_events",
        "commands",
        "command_sequence",
        "bytes",
        "work_items",
        "modeled_latency_ns",
        "baseline_latency_ns",
        "hwjit_latency_ns",
        "speedup",
        "baseline_method",
        "hwjit_policy",
        "general_ops",
        "ai_ops",
        "general_bytes",
        "ai_bytes",
        "hw_jit_ops",
        "hw_jit_commands",
        "hw_jit_bytes",
        "hw_jit_output_bytes",
        "hw_jit_work_items",
        "hw_jit_service_ns",
        "queued_ns",
        "service_ns",
        "general_cores",
        "ai_cores",
        "hw_jit_lanes",
        "hw_jit_switchlets",
        "result",
        "status",
        "damer_trace",
    ]
    with open(csv_path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fieldnames})
    return json_path, csv_path


def print_summary(rows: list[dict[str, object]]) -> None:
    print("\nBENCHMARK SUMMARY")
    print("case,commands,latency_ns,baseline_ns,hwjit_ns,speedup,general_ops,ai_ops,hw_jit_ops,bytes,result")
    for row in rows:
        speedup = row.get("speedup", "")
        if isinstance(speedup, float):
            speedup = f"{speedup:.2f}"
        print(
            f"{row['case']},{row['commands']},{row['modeled_latency_ns']},"
            f"{row.get('baseline_latency_ns', '')},{row.get('hwjit_latency_ns', '')},"
            f"{speedup},{row['general_ops']},{row['ai_ops']},{row['hw_jit_ops']},"
            f"{row['bytes']},{row['result']}"
        )


def selected_cases(selection: str) -> list[CaseSpec]:
    specs = case_specs()
    if selection == "all":
        return specs
    requested = [item.strip() for item in selection.split(",") if item.strip()]
    by_name = {spec.name: spec for spec in specs}
    missing = [name for name in requested if name not in by_name]
    if missing:
        names = ", ".join(sorted(by_name))
        raise RuntimeError(f"unknown benchmark case(s) {missing}; available: {names}")
    return [by_name[name] for name in requested]


def qemu_args(args: argparse.Namespace, qtest_path: Path) -> list[str]:
    return [
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
        "-device", "cxl-rp,port=0,bus=cxl.0,id=type2_rp,chassis=0,slot=2",
        "-device", "cxl-upstream,port=0,sn=1234,bus=type2_rp,id=type2_us",
        "-device", "cxl-downstream,port=0,bus=type2_us,id=type2_ds,slot=3",
        "-device",
        (
            "cxl-type2,bus=type2_ds,id=cxl-type2-bench,sn=202,gpu-mode=0,"
            "cache-size=16M,mem-size=64M,cxlmemsim-addr=127.0.0.1,"
            f"cxlmemsim-port={args.port},coherency-enabled=true,dcd=on,"
            "dcd-granularity=1M,dcd-initial-size=64M,gfam=on,gfam-hosts=4,"
            "gfam-host-id=0,mhsld=on,mhsld-heads=4,mhsld-head-id=0"
        ),
    ]


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=10126)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--run-dir", default=str(repo / "build" / "qtest-switch-bench"))
    parser.add_argument("--server", default=str(repo / "build" / "cxlmemsim_server"))
    parser.add_argument("--qemu", default=str(repo / "lib" / "qemu" / "build" / "qemu-system-x86_64"))
    parser.add_argument("--bar2-base", type=lambda value: int(value, 0), default=0x80000000)
    parser.add_argument("--damer-root", default="/root/Damer")
    parser.add_argument("--cases", default="all",
                        help="comma-separated case names, or all")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--quick", action="store_true",
                        help="use smaller matrix/vector sizes")
    parser.add_argument("--output-prefix", default="switch_benchmark")
    parser.add_argument("--switch-general-cores", type=int, default=4)
    parser.add_argument("--switch-ai-cores", type=int, default=2)
    parser.add_argument("--switch-hwjit-lanes", type=int, default=1)
    parser.add_argument("--switch-general-latency", type=int, default=40)
    parser.add_argument("--switch-ai-latency", type=int, default=30)
    parser.add_argument("--switch-hwjit-latency", type=int, default=8)
    parser.add_argument("--switch-hwjit-state-latency", type=int, default=4)
    parser.add_argument("--switch-general-bandwidth", type=int, default=64)
    parser.add_argument("--switch-ai-ops-per-ns", type=int, default=128)
    parser.add_argument("--switch-hwjit-bandwidth", type=int, default=256)
    parser.add_argument("--switch-hwjit-ops-per-ns", type=int, default=512)
    parser.add_argument("--hwjit-kv-bytes", type=int,
                        help="bytes for the Qwen27B KV-pack HW-JIT case")
    parser.add_argument("--kimi-layers", type=int)
    parser.add_argument("--kimi-ranks", type=int)
    parser.add_argument("--kimi-dim", type=int)
    parser.add_argument("--kimi-kv-shard-bytes", type=int)
    parser.add_argument("--kimi-reduce-count", type=int)
    parser.add_argument("--kimi-allgather-mode", choices=["full", "single-dst"],
                        default="full",
                        help="Kimi collective ablation: full fanout or one destination")
    parser.add_argument("--kimi-allreduce-mode", choices=["full", "reduce-only"],
                        default="full",
                        help="Kimi collective ablation: include or omit result broadcast")
    args = parser.parse_args()

    if args.repeat < 1:
        raise RuntimeError("--repeat must be at least 1")

    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    server_log_path = run_dir / "cxlmemsim-server.log"
    qemu_log_path = run_dir / "qemu-qtest.log"
    traces = load_damer_traces(Path(args.damer_root))
    specs = selected_cases(args.cases)

    server_cmd = [
        args.server,
        "--comm-mode=tcp",
        f"--port={args.port}",
        "--capacity=64",
        "--default_latency=100",
        "--enable-switch-cores",
        f"--switch-general-cores={args.switch_general_cores}",
        f"--switch-ai-cores={args.switch_ai_cores}",
        f"--switch-hwjit-lanes={args.switch_hwjit_lanes}",
        f"--switch-general-latency={args.switch_general_latency}",
        f"--switch-ai-latency={args.switch_ai_latency}",
        f"--switch-hwjit-latency={args.switch_hwjit_latency}",
        f"--switch-hwjit-state-latency={args.switch_hwjit_state_latency}",
        f"--switch-general-bandwidth={args.switch_general_bandwidth}",
        f"--switch-ai-ops-per-ns={args.switch_ai_ops_per_ns}",
        f"--switch-hwjit-bandwidth={args.switch_hwjit_bandwidth}",
        f"--switch-hwjit-ops-per-ns={args.switch_hwjit_ops_per_ns}",
        "--verbose=1",
    ]

    server_log = open(server_log_path, "wb")
    server_proc = subprocess.Popen(server_cmd, stdout=server_log,
                                   stderr=subprocess.STDOUT)
    qemu_proc = None
    qemu_log = None
    qt = None
    client = None
    try:
        qtest.wait_for_port(args.host, args.port)
        client = qtest.MemSimClient(args.host, args.port)

        # Keep the qtest socket path short enough for sockaddr_un. Results and
        # logs still go under run_dir; only this transient socket lives in /tmp.
        qtest_dir = Path(tempfile.mkdtemp(prefix="qtest-"))
        qtest_path = qtest_dir / "qtest.sock"
        qemu_proc, qt, qemu_log = qtest.launch_qemu(
            qemu_args(args, qtest_path), qtest_path, qemu_log_path)
        bar2 = qtest.map_type2_bar2_through_switch(
            qt, args.bar2_base, 16 * 1024 * 1024)

        profile = {
            "kimi_layers": args.kimi_layers,
            "kimi_ranks": args.kimi_ranks,
            "kimi_dim": args.kimi_dim,
            "kimi_kv_shard_bytes": args.kimi_kv_shard_bytes,
            "kimi_reduce_count": args.kimi_reduce_count,
            "kimi_allgather_mode": args.kimi_allgather_mode,
            "kimi_allreduce_mode": args.kimi_allreduce_mode,
            "hwjit_kv_bytes": args.hwjit_kv_bytes,
        }
        ctx = BenchmarkContext(client, qt, bar2, args.quick, profile)
        initial = stats_from_bar(ctx)
        if initial["enabled"] != 1:
            raise RuntimeError("switch cores are not enabled")

        rows = []
        for iteration in range(args.repeat):
            for spec in specs:
                rows.append(run_case(ctx, spec, iteration, traces))

        config = {
            "quick": args.quick,
            "repeat": args.repeat,
            "cases": [spec.name for spec in specs],
            "server_cmd": server_cmd,
            "qemu": args.qemu,
            "qemu_topology": (
                "pxb-cxl -> cxl-rp -> cxl-upstream -> "
                "cxl-downstream -> cxl-type2"
            ),
            "damer_root": args.damer_root,
            "damer_hwjit_report": str(Path(args.damer_root) / "out" / "hwjit-qwen27b-final" /
                                      "cxl_switch_hwjit_sim_report.json"),
            "damer_hwjit_rtl": "fpga/damer_cxl_switch_hwjit_qwen27b_policy.sv",
            "damer_traces_loaded": len({id(value) for value in traces.values()}),
            "kimi_profile": profile,
        }
        json_path, csv_path = write_outputs(rows, run_dir, args.output_prefix, config)
        print_summary(rows)
        print(f"RESULT_JSON {json_path}")
        print(f"RESULT_CSV {csv_path}")
        print(f"LOG server={server_log_path}")
        print(f"LOG qemu={qemu_log_path}")
        return 0
    finally:
        if qt:
            qt.close()
        if client:
            client.close()
        qtest.stop_process(qemu_proc)
        if qemu_log is not None:
            qemu_log.close()
        qtest.stop_process(server_proc)
        server_log.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        raise
