// Runs the aurora web smoke test (runtime/web/smoke) in headless Chromium and checks what it drew.
//
//   node runtime/web/smoke/run-smoke.mjs <build-dir> [--keep-screenshot <file>]
//
// Serves <build-dir> with the cross-origin isolation headers wasm threads need, opens
// aurora_smoke.html with WebGPU enabled (SwiftShader where there is no GPU), waits for the app to
// present its frames, and reads back the middle pixel of the last presented frame. The canvas is
// offscreen in headless Chromium; see ../browser-harness.mjs.
import process from 'node:process';
import { installOffscreenCanvasContext, launchChromium, serveIsolated } from '../browser-harness.mjs';

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

const { server, base } = await serveIsolated(buildDir);
const url = `${base}/aurora_smoke.html`;

const browser = await launchChromium();
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
