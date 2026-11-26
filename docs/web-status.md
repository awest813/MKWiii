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

## Release gates

- Verify native game speed in a focused browser for single-player and each
  split-screen mode. The retained static build reached about 45% of native pace
  in one headless measurement. Renderer experiments reduced binding calls, and
  a reproducible game-speed gain remains unverified.
- Verify audio during startup, track transitions, and complete sessions across
  all player counts. The four-player AudioWorklet result covers one sample in
  one diagnostic race.
- Expand testing to physical controllers, more cups and courses, and recovery
  from an actual graphics-driver reset.
- Publish the launcher and packaging tools before treating the repository as a
  source for a hosted browser build. The tested static bundle was local. It
  requires HTTPS and cross-origin isolation, and players supply their own ISO.
