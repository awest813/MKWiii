// Runs a web build of the runtime product in headless Chromium and reports how it ended.
//
//   node runtime/web/run-product.mjs <build-dir> [--product WiiCompiled] [--timeout <seconds>]
//        [--expect-exit <status>] [--expect-log <regex>]... [-- runtime arguments]
//
// Serves <build-dir> with cross-origin isolation, opens <product>.html (installed next to the .js
// and .wasm by the build; see runtime/web/product) with WebGPU on SwiftShader, passes the runtime
// arguments through the query string (--allow-software-gpu is always added, since a headless
// browser only has a CPU adapter), and waits until main() exits or the module aborts. Fails when
// it aborts, times out, exits with a status other than --expect-exit (default 0), or any
// --expect-log pattern never matched the runtime's output.
import process from 'node:process';
import { installOffscreenCanvasContext, launchChromium, serveIsolated } from './browser-harness.mjs';

const usage = 'usage: node run-product.mjs <build-dir> [--product <name>] [--timeout <s>] ' +
    '[--expect-exit <status>] [--expect-log <regex>]... [-- runtime arguments]';
const argv = process.argv.slice(2);
const buildDir = argv.shift();
if (!buildDir || buildDir.startsWith('--')) {
    console.error(usage);
    process.exit(2);
}
let product = 'WiiCompiled';
let timeoutMs = 300_000;
let expectExit = 0;
const expectLogs = [];
const runtimeArgs = ['--allow-software-gpu'];
while (argv.length) {
    const flag = argv.shift();
    if (flag === '--') {
        runtimeArgs.push(...argv.splice(0));
    } else if (flag === '--product') {
        product = argv.shift();
    } else if (flag === '--timeout') {
        timeoutMs = Number(argv.shift()) * 1000;
    } else if (flag === '--expect-exit') {
        expectExit = Number(argv.shift());
    } else if (flag === '--expect-log') {
        expectLogs.push(new RegExp(argv.shift()));
    } else {
        console.error(`run-product: unknown argument ${flag}\n${usage}`);
        process.exit(2);
    }
}

const { server, base } = await serveIsolated(buildDir);
const query = runtimeArgs.map((arg) => `arg=${encodeURIComponent(arg)}`).join('&');
const browser = await launchChromium();
let failure = null;
let probe = null;
try {
    const page = await browser.newPage({ viewport: { width: 800, height: 600 } });
    await page.addInitScript(installOffscreenCanvasContext);
    // print/printErr already go to the console; the page mirrors them in mkwProbe().log too.
    page.on('console', (message) => console.log(`[page] ${message.text()}`));
    page.on('pageerror', (error) => {
        console.log(`[page error] ${error.stack ?? error.message}`);
        failure ??= `page error: ${error.message}`;
    });
    await page.goto(`${base}/${product}.html?${query}`);
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline && !failure) {
        probe = await page.evaluate(() => window.mkwProbe());
        if (probe.exitStatus !== null || probe.aborted !== null) break;
        await page.waitForTimeout(250);
    }
    if (!failure) {
        if (probe?.aborted != null) {
            failure = `aborted: ${probe.aborted}`;
        } else if (probe?.exitStatus == null) {
            failure = `did not exit within ${timeoutMs / 1000}s`;
        } else if (probe.exitStatus !== expectExit) {
            failure = `exited with status ${probe.exitStatus}, expected ${expectExit}`;
        }
    }
    const log = (probe?.log ?? []).join('\n');
    for (const pattern of expectLogs) {
        if (!pattern.test(log)) failure ??= `no runtime output matched ${pattern}`;
    }
} finally {
    await browser.close();
    server.close();
}
if (failure) {
    console.error(`run-product: FAILED: ${failure}`);
    process.exit(1);
}
console.log(`run-product: ${product} exited with status ${probe.exitStatus}`);
