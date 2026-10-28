// Runs the aurora web smoke test (runtime/web/smoke) in headless Chromium and checks what it drew.
//
//   node runtime/web/smoke/run-smoke.mjs <build-dir> [--keep-screenshot <file>]
//
// Serves <build-dir> with the cross-origin isolation headers wasm threads need, opens
// aurora_smoke.html with WebGPU enabled (SwiftShader where there is no GPU), waits for the app to
// present its frames, and reads back the middle pixel of the last presented frame.
//
// Headless Chromium cannot allocate a WebGPU canvas swap chain on SwiftShader (its GPU process
// reports "Could not find SharedImageBackingFactory" and the device is lost), so the page's
// GPUCanvasContext is replaced with one that hands out ordinary textures on the same device. The
// app's whole render and present path still runs; only the browser's final compositing of the
// canvas is skipped, and the readback inspects exactly what would have been composited.
//
// Needs Node 18+ and Playwright's Chromium (`npm i playwright && npx playwright install chromium`;
// a preinstalled copy is used when require('playwright') already resolves).
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';
import process from 'node:process';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

// Must match kClearColor and kFrames in aurora_smoke.cpp.
const kExpected = [32, 160, 96, 255];
const kFrames = 120;
const kTolerance = 2;
const kTimeoutMs = 120_000;

const [buildDir, ...rest] = process.argv.slice(2);
if (!buildDir) {
    console.error('usage: node run-smoke.mjs <build-dir> [--keep-screenshot <file>]');
    process.exit(2);
}
const screenshotIndex = rest.indexOf('--keep-screenshot');
const screenshotPath = screenshotIndex >= 0 ? rest[screenshotIndex + 1] : null;

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = createServer(async (req, res) => {
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    const file = path.join(buildDir, path.normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)));
    try {
        const data = await readFile(file);
        res.setHeader('Content-Type', types[path.extname(file)] ?? 'application/octet-stream');
        res.end(data);
    } catch {
        res.statusCode = 404;
        res.end();
    }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/aurora_smoke.html`;

// Installed before any page script runs. See the header comment.
function installOffscreenCanvasContext() {
    const contexts = new WeakMap();
    const usage = () => GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC |
        GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST;
    GPUCanvasContext.prototype.configure = function (config) {
        contexts.set(this, { config, texture: null });
    };
    GPUCanvasContext.prototype.unconfigure = function () {
        contexts.delete(this);
    };
    GPUCanvasContext.prototype.getConfiguration = function () {
        return contexts.get(this)?.config ?? null;
    };
    GPUCanvasContext.prototype.getCurrentTexture = function () {
        const state = contexts.get(this);
        if (!state) throw new DOMException('context is not configured', 'InvalidStateError');
        const { width, height } = this.canvas;
        if (!state.texture || state.texture.width !== width || state.texture.height !== height) {
            state.texture?.destroy();
            state.texture = state.config.device.createTexture({
                size: [width, height], format: state.config.format, usage: usage(),
            });
        }
        globalThis.mkwLastCanvasTexture = { texture: state.texture, device: state.config.device };
        return state.texture;
    };
    // The RGBA bytes of one pixel of the last presented frame.
    globalThis.mkwReadCanvasPixel = async (x, y) => {
        const last = globalThis.mkwLastCanvasTexture;
        if (!last) return null;
        const buffer = last.device.createBuffer({ size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        const encoder = last.device.createCommandEncoder();
        encoder.copyTextureToBuffer({ texture: last.texture, origin: [x, y] }, { buffer, bytesPerRow: 256 }, [1, 1]);
        last.device.queue.submit([encoder.finish()]);
        await buffer.mapAsync(GPUMapMode.READ);
        const px = [...new Uint8Array(buffer.getMappedRange(), 0, 4)];
        buffer.unmap();
        return { px, format: last.texture.format, width: last.texture.width, height: last.texture.height };
    };
}

const browser = await chromium.launch({
    args: ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-webgpu-adapter=swiftshader'],
});
let failure = null;
try {
    const page = await browser.newPage({ viewport: { width: 800, height: 600 } });
    await page.addInitScript(installOffscreenCanvasContext);
    page.on('console', (message) => console.log(`[page] ${message.text()}`));
    page.on('pageerror', (error) => {
        console.log(`[page error] ${error.stack ?? error.message}`);
        failure ??= `page error: ${error.message}`;
    });
    await page.goto(url);
    const deadline = Date.now() + kTimeoutMs;
    let probe;
    while (Date.now() < deadline && !failure) {
        probe = await page.evaluate(() => window.mkwProbe());
        if (probe.status !== null) break;
        await page.waitForTimeout(250);
    }
    if (!failure) {
        if (probe?.status !== 0) {
            failure = `app did not finish cleanly: ${JSON.stringify(probe)}`;
        } else if (probe.frames !== kFrames) {
            failure = `expected ${kFrames} frames, got ${probe.frames}`;
        } else {
            const read = await page.evaluate(async () => {
                const canvas = document.getElementById('canvas');
                return globalThis.mkwReadCanvasPixel(canvas.width >> 1, canvas.height >> 1);
            });
            if (!read) {
                failure = 'nothing was presented on the canvas';
            } else {
                // Present formats are RGBA8 or BGRA8; compare in RGBA order.
                const rgba = read.format.startsWith('bgra') ? [read.px[2], read.px[1], read.px[0], read.px[3]] : read.px;
                console.log(`center pixel ${JSON.stringify(rgba)} (${read.format}, ${read.width}x${read.height})`);
                if (rgba.some((value, i) => Math.abs(value - kExpected[i]) > kTolerance)) {
                    failure = `center pixel ${JSON.stringify(rgba)}, expected ${JSON.stringify(kExpected)}`;
                }
            }
        }
    }
    if (screenshotPath) await page.screenshot({ path: screenshotPath });
} finally {
    await browser.close();
    server.close();
}
if (failure) {
    console.error(`aurora web smoke test FAILED: ${failure}`);
    process.exit(1);
}
console.log('aurora web smoke test passed');
