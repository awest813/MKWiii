# Browser port status

Updated November 27, 2026. The browser port is experimental. Gameplay results
come from local development builds tested in headless Chrome with hardware
WebGPU and virtual controllers. The `main` branch contains the synthetic browser
test harness. The game launcher and static packaging tools are still in development.

## Verified in local builds

| Area | Result |
| --- | --- |
| Single-player | All four races of the 50cc Mushroom Cup finished through the game menus. The gold trophy, B rank, and save data survived a browser restart. |
| Same-screen multiplayer | Two-, three-, and four-player tests produced physical race finishes and passed Save & Exit. These tests covered Luigi Circuit with virtual controllers. |
| Split-screen audio | Earlier AudioWorklet checks had gap-free active racing in separate two-, three-, and four-player races. Later renderer candidates had gaps in full-session recordings. Menu and course-loading gaps persist. |
| Local ISO launcher | A PAL RMCP01 revision 0 ISO loaded from the browser's file picker. Tests recorded zero disc uploads and verified startup retry with the same ISO, fullscreen focus restoration, disc reads after a worker restart, and save recovery after intentional WebGPU device loss. |

## Performance work

In the measured headless race, the retained static build ran at about 45% of
native pace. A renderer experiment skipped about 23% of texture-binding calls
over the sampled portion of a race. The staff replay matched its reference
finish time and Save & Exit passed. A version that skips the calls inside Wasm
ran at 46.07% of native pace, about 2.89% faster than the mean of two matched
controls in one headless race sequence. It also matched the reference finish
of 1:40.349 in a full CPU-driven race. A separate pipeline-cache experiment ran
at 44.25% of native pace in headless Chrome. The two matched controls measured
43.48% and 43.75%. The candidate ran about 1.46% faster than the mean control
time over guest frames 600–3000. The result covers one diagnostic race sequence.

With both renderer changes enabled, the candidate ran at 48.94% of native pace.
The two controls ran at 45.16% and 49.15%, so the combined result fell within
the observed control range. A separate staging-buffer change passed four
hardware WebGPU fixture cases covering immediate release, timer fallback,
release after mapping, and map cancellation. Each case returned its slot to the
pool and left no pending waiter. Its timing result was 47.18% of native pace,
compared with 46.74% and 47.02% for the controls. A separate candidate avoids
remapping frame staging buffers. It reached 57.78% and 58.80% of native pace
in two matched single-player headless sequences, about 22% faster than the
faster control in each sequence. In a matched four-player headless sequence,
the candidate reached 42.94% of native pace, compared with 39.08% and 39.24%
for the controls. All four local racers physically finished and saved in a
separate check. That run also recorded seven all-zero audio callbacks in
30 seconds. A follow-up that wrote draw buffers directly reached 42.60% of
native pace, within its control range of 34.45% to 46.41%, and remains an
experiment. Native pace and continuous audio remain open.

### Frame uploads and audio

In a four-player course load, Chrome spent about 1,025 ms waiting for a GPU
command-buffer token inside a 1,026 ms call that uploaded 2,716,208 bytes of
uniform data. Limiting staging to two in-flight frames reduced course-loading
audio loss to 694 ms in a full four-player repeat. All four players finished
and saved. The 268-second active race still lost 437 ms of audio.

A hardware WebGPU fixture combined the used staging ranges into one upload per
frame. It reduced writes from 178 to 41 without adding payload bytes. All
98,304 bytes read back from the GPU matched the original buffer and texture
data. The prototype preserves the staging offsets used by already-encoded
commands. Its effect on race speed and full-session audio remains unmeasured.

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
and confirm it returns `true` before testing disc selection. Use a static host
that lets you configure both response headers. Saves are tied to the site
origin, so changing hostnames gives the launcher a separate save store. The
current static package is a local development build generated from a supplied disc.
It passed local launcher checks with 15 content-hashed Wasm chunks. Serve the
complete package from one origin, preserve JavaScript and WebAssembly MIME types,
revalidate the launcher and manifests, and use immutable caching for the hashed
chunks. Public hosting is still unverified.

## Release gates

- Verify native game speed in a focused browser for single-player and each
  split-screen mode.
- Fix the observed audio gaps during loading and active play. Verify startup,
  track transitions, and complete sessions across all player counts.
- Expand testing to physical controllers, more cups and courses, and recovery
  from an actual graphics-driver reset.
- Publish the launcher source and packaging tools. A public release also needs
  a distribution workflow that follows the project's policy for disc-derived
  builds.
