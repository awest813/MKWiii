// Shared pieces of the headless browser tests (smoke/run-smoke.mjs, run-product.mjs): a static
// server with cross-origin isolation, Chromium with WebGPU, and an offscreen canvas context.
//
// Headless Chromium cannot allocate a WebGPU canvas swap chain on SwiftShader (its GPU process
// reports "Could not find SharedImageBackingFactory" and the device is lost), so the page's
// GPUCanvasContext is replaced with one that hands out ordinary textures on the same device. The
// app's whole render and present path still runs; only the browser's final compositing of the
// canvas is skipped, and mkwReadCanvasPixel inspects exactly what would have been composited.
//
// Needs Node 18+ and Playwright's Chromium (`npm i playwright && npx playwright install chromium`;
// a preinstalled copy is used when require('playwright') already resolves).
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);

const kTypes = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm' };

// Serves `root` on a free local port with the COOP/COEP headers wasm threads need. Returns the
// server and its base URL.
export async function serveIsolated(root) {
    const server = createServer(async (req, res) => {
        res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
        res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
        const file = path.join(root, path.normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)));
        try {
            const data = await readFile(file);
            res.setHeader('Content-Type', kTypes[path.extname(file)] ?? 'application/octet-stream');
            res.end(data);
        } catch {
            res.statusCode = 404;
            res.end();
        }
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    return { server, base: `http://127.0.0.1:${server.address().port}` };
}

// Chromium with WebGPU, on SwiftShader where there is no GPU.
export function launchChromium() {
    const { chromium } = require('playwright');
    return chromium.launch({
        args: ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-webgpu-adapter=swiftshader'],
    });
}

// Installed with page.addInitScript before any page script runs. See the header comment.
export function installOffscreenCanvasContext() {
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
