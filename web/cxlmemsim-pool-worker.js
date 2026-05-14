'use strict';

/* Same MessagePort protocol as the previous flat-pool worker:
 * connect / sync-request / qemu-message / reset / get-status.
 *
 * Difference: a single WASM-compiled CXLMemSim instance services all
 * tabs. If the WASM fails to load, we keep the flat-pool fallback so
 * QEMU clients still see something coherent (just no latency model
 * and no MESI directory).
 */

const DEFAULT_POOL = 'CXLMemSim';
const DEFAULT_SIZE = 256 * 1024 * 1024;
const REQUEST_DATA_OFFSET = 64;
const RESPONSE_OFFSET = 128;
const RESPONSE_SIZE = 81;
const REQUEST_SIZE = 105;
const TYPE2_MSG_SIZE = 96;
const TYPE2_DATA_OFFSET = 26;
const INV_CAP = 16;
const STATS_SIZE = 256;
const WASM_URL = new URL('./cxlmemsim_wasm/cxlmemsim_wasm.mjs', self.location).href;

const CXL_T2_MSG_INVALIDATE = 7;

const pools = new Map();
let bridge = null;
let bridgeError = null;
let bridgeReady = null;

const events = new BroadcastChannel('cxlmemsim-events');

async function ensureBridge(capacity) {
    if (bridge) return bridge;
    if (bridgeReady) return bridgeReady;
    bridgeReady = (async () => {
        try {
            const mod = await import(WASM_URL);
            const Module = await mod.default();
            const out = Module._malloc(4);
            const rc = Module._cxlmemsim_init(capacity, 0, out);
            if (rc !== 0) {
                Module._free(out);
                throw new Error('cxlmemsim_init returned ' + rc);
            }
            Module._free(out);
            bridge = {
                Module,
                reqPtr: Module._malloc(REQUEST_SIZE),
                respPtr: Module._malloc(RESPONSE_SIZE),
                invPtr: Module._malloc(INV_CAP * 4),
                statsPtr: Module._malloc(STATS_SIZE),
                capacity
            };
            events.postMessage({ type: 'bridge-ready' });
            for (const pool of pools.values()) publishStats(pool, true);
            return bridge;
        } catch (err) {
            bridgeError = err;
            broadcastDegraded(err && err.message ? err.message : String(err));
            return null;
        }
    })();
    return bridgeReady;
}

function broadcastDegraded(reason) {
    for (const pool of pools.values()) {
        for (const client of pool.clients.values()) {
            client.port.postMessage({ type: 'degraded', reason });
        }
    }
}

function normalizePoolName(name) {
    const text = String(name || '').trim();
    return text || DEFAULT_POOL;
}

function clampSize(size) {
    const value = Number(size);
    if (!Number.isFinite(value) || value <= 0) return DEFAULT_SIZE;
    return Math.max(64 * 1024 * 1024, Math.min(value, 1024 * 1024 * 1024));
}

function makePool(name, size) {
    const buffer = new SharedArrayBuffer(clampSize(size));
    return {
        name, size: buffer.byteLength,
        buffer, bytes: new Uint8Array(buffer), view: new DataView(buffer),
        clients: new Map(),
        stats: { reads: 0, writes: 0, atomics: 0, fences: 0,
                 messages: 0, invalidations: 0, bytesRead: 0,
                 bytesWritten: 0, errors: 0 }
    };
}

function getPool(name, size) {
    const poolName = normalizePoolName(name);
    let pool = pools.get(poolName);
    if (!pool) {
        pool = makePool(poolName, size);
        pools.set(poolName, pool);
    }
    return pool;
}

function toOffset(lo, hi) {
    return Number(lo >>> 0) + Number(hi >>> 0) * 4294967296;
}

function setResponse(sab, status, payload, oldValue = 0n, latencyNs = 0n) {
    const control = new Int32Array(sab, 0, 1);
    const bytes = new Uint8Array(sab);
    const view = new DataView(sab);
    bytes.fill(0, RESPONSE_OFFSET, RESPONSE_OFFSET + RESPONSE_SIZE);
    view.setUint8(RESPONSE_OFFSET, status);
    view.setBigUint64(RESPONSE_OFFSET + 1, BigInt(latencyNs), true);
    view.setBigUint64(RESPONSE_OFFSET + 9, BigInt(oldValue), true);
    if (payload && payload.length) {
        bytes.set(payload.subarray(0, 64), RESPONSE_OFFSET + 17);
    }
    Atomics.store(control, 0, 1);
    Atomics.notify(control, 0, 1);
}

function makeType2Message(type, addr, size, state, source, payload) {
    const buffer = new ArrayBuffer(TYPE2_MSG_SIZE);
    const view = new DataView(buffer);
    const bytes = new Uint8Array(buffer);
    view.setUint32(0, type, true);
    view.setUint32(4, size >>> 0, true);
    view.setBigUint64(8, BigInt(addr), true);
    view.setBigUint64(16, BigInt(Date.now()) * 1000000n, true);
    view.setUint8(24, state >>> 0);
    view.setUint8(25, source >>> 0);
    if (payload && payload.length) {
        bytes.set(payload.subarray(0, 64), TYPE2_DATA_OFFSET);
    }
    return buffer;
}

function broadcastType2(pool, sourceId, buffer) {
    for (const client of pool.clients.values()) {
        if (client.id === sourceId || client.role !== 'qemu') continue;
        client.port.postMessage({ type: 'message', bytes: buffer.slice(0) });
    }
}

function copyRequestInto(bridgeState, msg) {
    const { Module, reqPtr } = bridgeState;
    const heap = Module.HEAPU8;
    heap.fill(0, reqPtr, reqPtr + REQUEST_SIZE);
    heap[reqPtr] = msg.op >>> 0;
    const view = new DataView(heap.buffer, reqPtr, REQUEST_SIZE);
    view.setBigUint64(1, BigInt(toOffset(msg.addrLo, msg.addrHi)), true);
    view.setBigUint64(9, BigInt(msg.size >>> 0), true);
    view.setBigUint64(17, 0n, true);
    view.setBigUint64(25, BigInt((msg.valueHi >>> 0) * 4294967296 + (msg.valueLo >>> 0)), true);
    view.setBigUint64(33, BigInt((msg.expectedHi >>> 0) * 4294967296 + (msg.expectedLo >>> 0)), true);
    if (msg.op === 1 || msg.op === 7) {
        const src = new Uint8Array(msg.sab, REQUEST_DATA_OFFSET, 64);
        heap.set(src, reqPtr + 41);
    }
}

function handleSyncRequest(pool, msg) {
    const sab = msg.sab;
    try {
        if (!bridge) {
            handleSyncRequestFlat(pool, msg);
            return;
        }
        copyRequestInto(bridge, msg);
        const n = bridge.Module._cxlmemsim_handle_request(
            bridge.reqPtr, bridge.respPtr, bridge.invPtr, INV_CAP);
        const respBytes = bridge.Module.HEAPU8.subarray(
            bridge.respPtr, bridge.respPtr + RESPONSE_SIZE);
        const status = respBytes[0];
        const view = new DataView(respBytes.buffer, respBytes.byteOffset, RESPONSE_SIZE);
        const latency = view.getBigUint64(1, true);
        const oldVal = view.getBigUint64(9, true);
        const payload = respBytes.subarray(17, 17 + 64);
        setResponse(sab, status, payload, oldVal, latency);

        if (status !== 0) pool.stats.errors++;
        if (msg.op === 0 || msg.op === 6) {
            pool.stats.reads++;
            pool.stats.bytesRead += Number(msg.size >>> 0);
        } else if (msg.op === 1 || msg.op === 7) {
            pool.stats.writes++;
            pool.stats.bytesWritten += Number(msg.size >>> 0);
        } else if (msg.op === 3 || msg.op === 4) {
            pool.stats.atomics++;
        } else if (msg.op === 5) {
            pool.stats.fences++;
        }

        if (n > 0) {
            const invView = new DataView(bridge.Module.HEAPU8.buffer,
                                         bridge.invPtr, n * 4);
            for (let i = 0; i < n; i++) {
                const addr = invView.getUint32(i * 4, true);
                pool.stats.invalidations++;
                broadcastType2(pool, msg.clientId,
                    makeType2Message(CXL_T2_MSG_INVALIDATE,
                                     addr, 64, 0, 0xff));
            }
        }
    } catch (err) {
        pool.stats.errors++;
        setResponse(sab, 1, null);
        events.postMessage({ type: 'error', reason: String(err) });
    }
    publishStats(pool);
}

function handleSyncRequestFlat(pool, msg) {
    const sab = msg.sab;
    const requestBytes = new Uint8Array(sab);
    const addr = toOffset(msg.addrLo, msg.addrHi);
    const size = Number(msg.size >>> 0);
    const inRange = Number.isInteger(addr) && Number.isInteger(size) &&
        addr >= 0 && size >= 0 && size <= 64 && addr + size <= pool.size;
    if (!inRange) {
        pool.stats.errors++;
        setResponse(sab, 2, null);
        return;
    }
    switch (msg.op) {
    case 0: case 6: {
        const payload = pool.bytes.subarray(addr, addr + size);
        pool.stats.reads++; pool.stats.bytesRead += size;
        setResponse(sab, 0, payload);
        break;
    }
    case 1: case 7: {
        pool.bytes.set(requestBytes.subarray(REQUEST_DATA_OFFSET,
            REQUEST_DATA_OFFSET + size), addr);
        pool.stats.writes++; pool.stats.bytesWritten += size;
        setResponse(sab, 0, null);
        broadcastType2(pool, msg.clientId,
            makeType2Message(CXL_T2_MSG_INVALIDATE, addr, size, 0, 0xff));
        break;
    }
    case 5:
        pool.stats.fences++;
        setResponse(sab, 0, null);
        break;
    default:
        pool.stats.errors++;
        setResponse(sab, 3, null);
        break;
    }
}

function handleQemuMessage(pool, client, msg) {
    if (!msg.bytes) return;
    const buffer = msg.bytes;
    pool.stats.messages++;
    if (bridge) {
        try {
            bridge.Module.HEAPU8.set(new Uint8Array(buffer), bridge.reqPtr);
            bridge.Module._cxlmemsim_handle_type2(bridge.reqPtr);
        } catch (err) {
            events.postMessage({ type: 'error', reason: String(err) });
        }
    }
    broadcastType2(pool, client.id, buffer);
    publishStats(pool);
}

let lastBroadcast = 0;
function publishStats(pool, force = false) {
    const now = Date.now();
    if (!force && now - lastBroadcast < 50) return;
    lastBroadcast = now;
    let extra = {};
    if (bridge) {
        bridge.Module._cxlmemsim_get_stats(bridge.statsPtr);
        const stats = new DataView(bridge.Module.HEAPU8.buffer, bridge.statsPtr, STATS_SIZE);
        extra = {
            total_reads: Number(stats.getBigUint64(0, true)),
            total_writes: Number(stats.getBigUint64(8, true)),
            total_atomics: Number(stats.getBigUint64(16, true)),
            total_invalidations: Number(stats.getBigUint64(24, true)),
            total_errors: Number(stats.getBigUint64(32, true)),
            total_latency_ns: Number(stats.getBigUint64(56, true)),
            pool_capacity_bytes: Number(stats.getBigUint64(64, true))
        };
    }
    events.postMessage({
        type: 'stats',
        pool: pool.name,
        port: pool.stats,
        sim: extra
    });
    broadcastStatus(pool);
}

function broadcastStatus(pool) {
    const status = {
        type: 'status', pool: pool.name, size: pool.size,
        clients: Array.from(pool.clients.values()).map((client) => ({
            id: client.id, role: client.role, device: client.device
        })),
        stats: { ...pool.stats }
    };
    for (const client of pool.clients.values()) {
        if (client.role === 'ui') client.port.postMessage(status);
    }
}

function attachPort(port) {
    let client = null;
    port.onmessage = async (event) => {
        const msg = event.data || {};
        const pool = getPool(msg.pool, msg.size);

        if (msg.type === 'connect') {
            const id = msg.clientId ||
                `${msg.role || 'client'}-${Math.random().toString(16).slice(2)}`;
            client = { id, role: msg.role || 'client',
                       device: msg.device || '', port };
            pool.clients.set(id, client);
            port.postMessage({
                type: 'connected', clientId: id,
                pool: pool.name, size: pool.size
            });
            ensureBridge(pool.size).then((b) => {
                if (!b && bridgeError) {
                    port.postMessage({ type: 'degraded',
                        reason: String(bridgeError.message || bridgeError) });
                }
            });
            broadcastStatus(pool);
            return;
        }
        if (msg.type === 'disconnect') {
            const id = msg.clientId || (client && client.id);
            if (id) pool.clients.delete(id);
            broadcastStatus(pool);
            return;
        }
        if (msg.type === 'sync-request') {
            if (!bridge && !bridgeError) await ensureBridge(pool.size);
            handleSyncRequest(pool, msg);
            return;
        }
        if (msg.type === 'qemu-message' && client) {
            handleQemuMessage(pool, client, msg);
            return;
        }
        if (msg.type === 'reset') {
            pool.bytes.fill(0);
            for (const key of Object.keys(pool.stats)) pool.stats[key] = 0;
            if (bridge) bridge.Module._cxlmemsim_reset();
            broadcastStatus(pool);
            return;
        }
        if (msg.type === 'get-status') {
            broadcastStatus(pool);
            publishStats(pool, true);
        }
    };
    port.start();
}

self.onconnect = (event) => { attachPort(event.ports[0]); };
