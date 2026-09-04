# Dynamic C ABI Loader

This prototype keeps the dynamic-loading algorithm entirely in Python/Cython.
Node.js only sends a module name and imports the verified JavaScript path returned
by the loader.

## Three principles

1. **Discover by need** — a manifest maps a requested runtime capability to a JS
   entry point, native library, ABI signatures, integrity hashes, and dependencies.
2. **Score deterministically** — compiled Cython runs Dijkstra over non-negative
   `load_cost` edges and selects the lowest-cost valid dependency route.
3. **Verify then load** — Python confines paths to the manifest directory, checks
   optional SHA-256 hashes, loads DLLs in route order, and validates every C export
   before releasing the JavaScript entry point to Node.

## Build

```powershell
py -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -e .
$env:PYTHONPATH = "$PWD\python"
```

Build each DLL for the same CPU architecture as Python. Keep the exported ABI
`extern "C"` when compiling C++, and do not pass C++ objects across the boundary.

## Node usage

```js
import { LoaderClient } from "./node/loader-client.mjs";

const loader = new LoaderClient(
  ".venv/Scripts/python.exe",
  "python/server.py",
  "manifest.json"
);
const loaded = await loader.import("math-runtime");
console.log(loaded.metadata, loaded.namespace);
loader.close();
```

`ctypes` owns the DLL handle inside the persistent Python process. The JS module
does not call DLL functions directly in this first version. Add explicit RPC
commands to `server.py` for allow-listed native calls; never accept arbitrary
symbol names or raw pointer values from Node.

