// Runs a wasm32-wasi test binary under Node's WASI implementation and exits with its status.
// Usage: node run-wasi.mjs <binary.wasm> [args...]
import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';
import process, { argv, exit } from 'node:process';

const [, , binary, ...args] = argv;
if (!binary) {
    console.error('usage: node run-wasi.mjs <binary.wasm> [args...]');
    exit(2);
}
const wasi = new WASI({ version: 'preview1', args: [binary, ...args], preopens: { '/': '/' }, returnOnExit: true });
const module = await WebAssembly.compile(await readFile(binary));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
// exitCode rather than exit(): exit() can drop stdout still queued for a pipe.
process.exitCode = wasi.start(instance);
