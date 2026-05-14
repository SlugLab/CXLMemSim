# CXLMemSim Browser Server

`cxlmemsim-pool-worker.js` is the browser-hosted CXLMemSim
SharedWorker. On the first `connect` message it loads
`./cxlmemsim_wasm/cxlmemsim_wasm.mjs` and runs the real simulator
(controller + coherency engine + policy family) inside the worker.
The pool lives inside the WASM heap; each tab only ships a
per-request `SharedArrayBuffer` to handshake via
`Atomics.wait` / `Atomics.notify`.

If the WASM module fails to load, the worker falls back to a flat
pool so the page still works (just without latency or coherency
modelling).

## Deployment

- Canonical source: `CXLMemSim/web/cxlmemsim-pool-worker.js`
- Deployed mirror: `victoryang00.github.io/cxl2/cxlmemsim-pool-worker.js`
- WASM artifacts: `victoryang00.github.io/cxl2/cxlmemsim_wasm/`

Run `hetGPU_new/tools/build_cxlmemsim_wasm.sh` to (re)build and
deploy. Bump the `?v=…` cache-bust in
`victoryang00.github.io/cxl2/cxl-module.js`, `cxl2/index.html`, and
`cxl2/cxlmemsim.html` when you ship a behaviour change.

## Cross-tab events

The worker broadcasts simulator deltas on
`BroadcastChannel("cxlmemsim-events")`. The dashboard
(`cxl2/cxlmemsim.html`) subscribes and renders per-simulator
counters (sim reads / writes / atomics / invalidations / avg
latency / errors / mode).

## Verifying end-to-end

1. Build WASM: `tools/build_cxlmemsim_wasm.sh`
2. Serve the site: `cd victoryang00.github.io && python3 -m http.server 8000`
3. Open `http://localhost:8000/cxl2/test_cross_tab.html` and click Run.
4. Open `http://localhost:8000/cxl2/cxlmemsim.html` in another tab and
   watch the Sim Reads / Sim Writes tiles increment.
5. Optional: open `http://localhost:8000/cxl2/index.html?cxl=type2&cxlmemsim=browser`
   and Start VM — the dashboard's Mode tile shows `wasm` and counters
   move as the guest issues memory traffic.
