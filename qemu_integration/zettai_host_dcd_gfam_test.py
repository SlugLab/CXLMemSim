#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import sys


REQ_STRUCT = struct.Struct("<BQQQQQ64s")
RESP_STRUCT = struct.Struct("<BQQ64s")

OP_DCD_QUERY = 10
OP_GFAM_QUERY = 13


class QMP:
    def __init__(self, path):
        self.path = path
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.file = None

    def __enter__(self):
        self.sock.connect(self.path)
        self.file = self.sock.makefile("rwb", buffering=0)
        self._read_message()
        self.execute("qmp_capabilities")
        return self

    def __exit__(self, exc_type, exc, tb):
        self.file.close()
        self.sock.close()

    def _read_message(self):
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line.decode("utf-8"))
            if "event" in msg:
                continue
            return msg

    def execute(self, command, arguments=None):
        req = {"execute": command}
        if arguments:
            req["arguments"] = arguments
        self.file.write((json.dumps(req) + "\n").encode("utf-8"))
        msg = self._read_message()
        if "error" in msg:
            err = msg["error"]
            raise RuntimeError(f"{command}: {err.get('class')}: {err.get('desc')}")
        return msg.get("return")


def query_memsim(host, port):
    def request(op):
        payload = REQ_STRUCT.pack(op, 0, 0, 0, 0, 0, b"\0" * 64)
        with socket.create_connection((host, port), timeout=5) as sock:
            sock.sendall(payload)
            data = b""
            while len(data) < RESP_STRUCT.size:
                chunk = sock.recv(RESP_STRUCT.size - len(data))
                if not chunk:
                    raise RuntimeError("CXLMemSim server closed the connection")
                data += chunk
        status, latency, old_value, raw = RESP_STRUCT.unpack(data)
        values = [struct.unpack_from("<Q", raw, offset)[0]
                  for offset in range(0, 56, 8)]
        return status, latency, old_value, values

    d_status, _, allocated, dcd = request(OP_DCD_QUERY)
    g_status, _, hosts, gfam = request(OP_GFAM_QUERY)

    print("DCD:")
    print(f"  status={d_status} allocated={allocated} total={dcd[0]} "
          f"free={dcd[1]} active_extents={dcd[2]} failed_requests={dcd[3]}")
    print("GFAM:")
    print(f"  status={g_status} hosts={hosts} mappings={gfam[0]} "
          f"shared={gfam[1]} reads={gfam[2]} writes={gfam[3]} "
          f"atomics={gfam[4]} denied={gfam[5]} avg_latency_ns={gfam[6]}")


def main():
    parser = argparse.ArgumentParser(
        description="Host-side Zettai VCS + DCD/GFAM test helper")
    parser.add_argument("--qmp", default="/tmp/zettai-qmp.sock",
                        help="QMP unix socket from the QEMU launch script")
    parser.add_argument("--server-host", default="127.0.0.1")
    parser.add_argument("--server-port", type=int, default=9999)
    parser.add_argument("--zettai-path", default="zettai0",
                        help="Zettai object id or QOM path")
    parser.add_argument("--dcd-path", default="/machine/peripheral/cxl-dcd0",
                        help="Bound Type 3 DCD QOM path")
    parser.add_argument("--vcs-id", type=int, default=0)
    parser.add_argument("--vppb-id", type=int, default=0)
    parser.add_argument("--dsp-ppb-id", type=int, default=0)
    parser.add_argument("--host-id", type=int, default=0)
    parser.add_argument("--region", type=int, default=0)
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--length", type=int, default=256 * 1024 * 1024,
                        help="DCD extent length in bytes")
    parser.add_argument("--bind", action="store_true",
                        help="Bind the hidden DCD endpoint to the vPPB")
    parser.add_argument("--add", action="store_true",
                        help="Add DCD dynamic capacity through QMP")
    parser.add_argument("--query", action="store_true",
                        help="Query CXLMemSim DCD/GFAM counters")
    parser.add_argument("--ignore-bind-error", action="store_true",
                        help="Continue if bind fails, useful when already bound")
    args = parser.parse_args()

    if not (args.bind or args.add or args.query):
        args.bind = True
        args.add = True
        args.query = True

    if args.bind or args.add:
        with QMP(args.qmp) as qmp:
            if args.bind:
                bind_args = {
                    "path": args.zettai_path,
                    "vcs-id": args.vcs_id,
                    "vppb-id": args.vppb_id,
                    "dsp-ppb-id": args.dsp_ppb_id,
                }
                try:
                    qmp.execute("zettai-bind-vppb", bind_args)
                    print("Bound Zettai vPPB")
                except RuntimeError as err:
                    if not args.ignore_bind_error:
                        raise
                    print(f"Ignoring bind error: {err}", file=sys.stderr)

            if args.add:
                qmp.execute("cxl-add-dynamic-capacity", {
                    "path": args.dcd_path,
                    "host-id": args.host_id,
                    "selection-policy": "prescriptive",
                    "region": args.region,
                    "extents": [{
                        "offset": args.offset,
                        "len": args.length,
                    }],
                })
                print(f"Added DCD capacity: offset={args.offset} "
                      f"length={args.length}")

    if args.query:
        query_memsim(args.server_host, args.server_port)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
