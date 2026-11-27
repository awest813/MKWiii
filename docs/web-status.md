# Browser port status

Updated November 26, 2026. The browser port is experimental. The results below
come from local development builds tested in headless Chrome with hardware
WebGPU and virtual controllers. The `main` branch contains the synthetic browser
test harness. The game launcher and static packaging tools are still in development.

## Verified in local builds

| Area | Result |
| --- | --- |
| Single-player | All four races of the 50cc Mushroom Cup finished through the game menus. The gold trophy, B rank, and save data survived a browser restart. |
| Same-screen multiplayer | Two-, three-, and four-player tests produced physical race finishes and passed Save & Exit. These tests covered Luigi Circuit with virtual controllers. |
| Four-player audio | A diagnostic build using an AudioWorklet and a longer mixer catch-up window played two minutes of active racing audio with zero missing or dropped frames. All four racers finished, and Save & Exit passed. Earlier diagnostic runs recorded audio gaps. |
| Local ISO launcher | A PAL RMCP01 revision 0 ISO loaded from the browser's file picker. The tests recorded zero disc uploads and verified startup retry, fullscreen focus restoration, and save recovery after intentional WebGPU device loss. |

## Performance work

In the measured headless race, the retained static build ran at about 45% of
native pace. A renderer experiment skipped about 23% of texture-binding calls
over the sampled portion of a race. The staff replay matched its reference
finish time and Save & Exit passed. A separate pipeline-cache experiment
compiled to WebAssembly, but its game link failed on unresolved audio symbols,
so there is no game-speed measurement for that change.

## Static hosting requirements

The launcher uses a local file picker for the supported ISO. The browser reads
the disc on the player's device. The tested launcher requires a secure context,
WebGPU, and cross-origin isolation for shared memory. Apply the following
response headers when serving the launcher and its runtime files over HTTPS.

```http
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Evaluate `window.crossOriginIsolated` in the launcher page's browser console
and confirm it returns `true` before testing disc selection. Saves are tied to the site origin,
so changing hostnames gives the launcher a separate save store. The current
static package is a local development build generated from a supplied disc.

## Release gates

- Verify native game speed in a focused browser for single-player and each
  split-screen mode. A reproducible game-speed gain remains unverified.
- Verify audio during startup, track transitions, and complete sessions across
  all player counts. The four-player AudioWorklet result covers one sample in
  one diagnostic race.
- Expand testing to physical controllers, more cups and courses, and recovery
  from an actual graphics-driver reset.
- Publish the launcher source and packaging tools. A public release also needs
  a distribution workflow that follows the project's policy for disc-derived
  builds.
