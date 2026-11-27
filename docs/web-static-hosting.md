# Static browser launcher

The launcher runs an experimental local build of the WebAssembly and WebGPU
port. You need a prepared static package from a browser development build.
The public repository does not currently contain the complete build and
packaging tools. See the [browser status report](web-status.md).

## Play from a local ISO

1. Open the launcher in a compatible Chromium browser with hardware WebGPU.
2. Select or drop a PAL Mario Kart Wii ISO with disc ID RMCP01, revision 0.
   The browser reads the selected file locally and sends no disc bytes to the
   hosting service.
3. Start the game after verification and runtime preparation. Connect controllers
   before starting a multiplayer session.
4. Use **Save & Exit** to sync progress before closing the game. The launcher
   can reuse the selected ISO in the same tab. Select it again after reloading
   the page. Browser saves are tied to the site origin.

## Serve a prepared package

Serve the complete package over HTTPS from one origin. Keep `index.html`,
the worker, manifests, game files, and content-hashed Wasm chunks at their
packaged relative paths. Serve JavaScript files with a JavaScript MIME type.
Keep launcher and manifest responses revalidatable, and cache content-hashed
Wasm chunks as immutable. Use normal missing-file responses for absent
resources so script and Wasm requests do not resolve to the launcher HTML.

The launcher and runtime need these response headers for shared memory.

```http
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Use a host that supports custom response headers. If the package includes a
`_headers` file, keep it alongside `index.html` and confirm that the host
applies its rules. For deployment under a path prefix, check that both the
isolation and cache rules cover that path.

## Check the deployed launcher

1. Open the direct launcher URL in a top-level browser tab.
2. Check the launcher response for both isolation headers in DevTools.
3. Run `window.crossOriginIsolated` in the console. It must return `true` before
   the game can run. If it returns `false`, verify the actual HTTP responses
   from the host or CDN.
4. Confirm the game frame, worker, manifests and a Wasm chunk load with the
   expected response types. Select an ISO and check that the game starts,
   saves, and restores progress after reopening.

Local packaging and HTTP checks passed. A public deployment of the optimized
package still needs verification.

## Validate from the browser development checkout

In a checkout containing the experimental tools, set `$browserBuild` to the
optimized build directory containing `WiiCompiled.js`, `WiiCompiled.wasm`, and
its build metadata. Use a fresh output directory. The packager excludes the
ISO and extracted disc files. The verifier checks the inventory and the complete
Wasm checksum reconstructed from the chunks.

```powershell
node runtime/web/package-static.mjs $browserBuild build-web-static
node runtime/web/verify-static.mjs build-web-static
node runtime/web/serve-static.mjs build-web-static --port 4177
```

After deploying a prepared package, the origin verifier checks the launcher,
runtime and worker headers, JavaScript response types, and chunk caching.

```powershell
node runtime/web/verify-static-origin.mjs https://your-host.example/mkw/
```

If Save & Exit reports a storage failure, use **Browser controls → Download
save backup** while the game remains open. If the runtime fails during startup,
**Retry game** reuses the verified ISO in the current tab.
