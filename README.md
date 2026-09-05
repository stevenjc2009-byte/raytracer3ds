# raytracer3ds

A software ray tracer for the Nintendo 3DS. No GPU, no PICA200 — every pixel on
the top screen is traced on the ARM11 and written straight into the linear
framebuffer.

Quality is the target, not speed — but it renders across every CPU core the
console will give it, and the per-pixel maths carries no library calls.

![placeholder](meta/icon.png)

## What it renders

A mirror-finish unit cube resting on an infinite reflective checkerboard plane.

- **400×240** primary rays, one per pixel, 2×2 supersampled by default
- **Ray/box** intersection by the slab method — the entry face normal falls out
  of the test itself, so normals are exact rather than reconstructed
- **Ray/plane** intersection for the ground
- **Recursive reflections**, 3 bounces deep, so the cube shows the floor showing
  the cube
- **Phong shading** — ambient + Lambert diffuse + specular — from one
  directional light
- **Hard shadows** via a shadow ray per hit
- **Procedural checkerboard** on both the cube faces and the ground plane
- **Metallic tint** applied to reflected radiance, so the cube reads as coloured
  metal rather than a neutral mirror
- **Schlick Fresnel** on the floor, so it reflects far more at grazing angles
- **Distance fade** of the checker into the sky, which kills horizon moiré
- **Gamma correction** on output

## Multicore rendering

The frame is split by **column interleave**: with `N` threads, thread `i` renders
columns `i, i+N, i+2N, …`. The bottom screen prints how many cores were acquired.

| Core | Availability | How it is obtained |
| --- | --- | --- |
| 0 | always | the app core |
| 1 | always | `APT_SetAppCpuTimeLimit()` — 80/70/50/30% requested in turn until one is granted (v1.0.3; see [Hot-path optimisations](#hot-path-optimisations-v103)) |
| 2 | New 3DS only | exheader kernel flag `0x2000`, set by `CanAccessCore2` in the RSF |

Core 2 is therefore **CIA-only**: a `.3dsx` under the Homebrew Launcher has no
exheader of its own and never gets it. Core 3 is not available to applications.

Two things about this design are deliberate:

- **Columns, not rows.** A framebuffer column is one contiguous 720-byte run in
  `GSP_BGR8_OES` layout, so each thread's inner loop is sequential *and* no two
  threads ever share a cache line. Row-interleaving would put every thread in the
  same line on every single pixel.
- **Static split, no work stealing.** It needs zero cross-core synchronisation and
  has no failure mode that can corrupt the image. Core 1 is throttled (v1.0.3
  requests 80/70/50/30% in turn until the OS grants one) and so finishes late;
  rather than guess a weighting to correct for that, the benchmark measures the
  real x1/x2/x3 scaling on the console.

### Cache coherency

ARMv6 cache maintenance is **not broadcast between cores**, and the display
controller is not a coherent bus master. `gfxInitDefault()` places the
framebuffers in cached linear heap memory, and `gfxFlushBuffers()` only cleans
the calling core. Each worker therefore cleans its own core's cache before
exiting:

```c
svcStoreProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)job->fb, SCREEN_W * SCREEN_H * 3);
```

`svcStoreProcessDataCache` is a plain syscall, so it needs no shared service
session — unlike `GSPGPU_FlushDataCache`, which would. If that clean ever fails
the error is printed on the bottom screen rather than silently leaving stale
pixels.

## Controls

| Button | Action |
| --- | --- |
| START | Exit |
| SELECT | Run the benchmark sweep |
| Y | Check GitHub for a newer release, and install it |
| X | Toggle half resolution (v1.0.4; see [Live half-resolution toggle](#live-half-resolution-toggle-v104)) |

The bottom screen shows frame count, frame time in milliseconds, and fps.

Input is only sampled between frames, and a frame takes seconds — so **hold the
button until the current frame finishes**. A quick tap lands between polls and
is missed.

## Benchmark

**SELECT** sweeps thirteen quality configurations, timing a real full-screen
render for each on the console itself — nothing synthetic, every case goes
through the ordinary render path. Results print as a `config / ms / fps` table on
the bottom screen and are written to `sdmc:/raytracer3ds_bench.txt`. **B** aborts
the sweep.

The cases isolate one lever each, so the cost of supersampling, of each
reflection bounce, of the shadow rays, of resolution, and of each additional core
can be read separately rather than guessed at:

| Case | Isolates |
| --- | --- |
| `2AA d3 full xN` | the shipped default, all cores |
| `2AA d3 full x1` | cost of losing multicore |
| `1AA d3 full x1 / x2 / x3` | **real scaling per core**, including core 1's throttle |
| `1AA d2 / d1 / d0 full` | cost of each reflection bounce |
| `1AA d1 full ns` | cost of the shadow rays |
| `1AA d3 half`, `1AA d1 half`, `1AA d3 qtr`, `1AA d1 qtr` | cost of resolution |

The report records the core count acquired, and each row's `thr` column shows the
thread count **actually used** rather than the count requested — so a case that
silently fell back to fewer threads cannot be misread as a scaling result.

`RenderConfig` holds these as runtime state rather than compile-time constants,
so a single build can sweep them all.

## Hardware measurements (v1.0.2, New 3DS)

The first real numbers this project has. The benchmark sweep, run for real on a
New 3DS that reported `cores: 2`:

| Case | ms/frame | fps |
| --- | --- | --- |
| `2AA d3 full xN` | 410.9 | 2.43 |
| `2AA d3 full x1` | 822.5 | 1.22 |
| `1AA d3 full x1` | 235.5 | 4.25 |
| `1AA d3 full x2` | 117.8 | 8.49 |
| `1AA d3 full x3` | 118.2 | 8.46 |
| `1AA d2 full xN` | 122.7 | 8.15 |
| `1AA d1 full xN` | 119.4 | 8.38 |
| `1AA d0 full xN` | 83.5 | 11.98 |
| `1AA d1 full no-shadows xN` | 96.9 | 10.32 |
| `1AA d3 half xN` | 31.0 | 32.26 |
| `1AA d1 half xN` | 29.3 | 34.07 |
| `1AA d3 quarter xN` | 8.4 | 119.18 |
| `1AA d1 quarter xN` | 8.0 | 124.59 |

What the numbers say:

- **Threading works and the split is right.** `235.5 / 117.8 = 1.999x` on two
  cores. x3 equals x2 because only two cores were available, so the clamp
  behaves correctly.
- **The cross-core cache clean is sound.** No vertical stripes of stale pixels
  appeared anywhere in the sweep — the first time `svcStoreProcessDataCache` had
  ever actually executed, and it held up.
- **Reflection bounces are essentially free; the first bounce is what costs.**
  d3 (117.8) ~= d2 (122.7) ~= d1 (119.4), all within noise of each other. d1 vs
  d0 is 119.4 vs 83.5 — about 36 ms for the first bounce alone. Hard shadows
  cost about 22.5 ms (d1 with shadows 119.4 vs d1 without 96.9).

### Is 20 fps reachable?

20 fps means a 50 ms frame. At full 400×240 that is not reachable by tuning:

- Even `1AA d0` — every reflection stripped out, which is no longer a
  raytracer in any meaningful sense — is 83.5 ms, 12 fps.
- Recovering a third core would take 117.8 ms to roughly 78.5 ms, about 12.7 fps.
- Micro-optimisation on top of that is plausibly another 10-25%, landing
  somewhere around 13-16 fps at full resolution.
- Half resolution at full quality (`1AA d3 half`) is already 31.0 ms, 32.26
  fps — comfortably past 20.

So the target is reachable at half resolution with the raytracing fully
intact, or it is not reachable at full resolution. Both of those are true at
the same time; this is a resolution/quality trade-off, not a bug to chase.

## Updating

Pressing **Y** asks GitHub for the latest release, compares it against
`APP_VERSION` in [`source/main.c`](source/main.c), and if there is a newer one,
offers to download and install it — A to confirm, B to cancel. The new CIA is
streamed straight into an AM install handle in 16 KB chunks, so it is never
buffered whole. Relaunch afterwards to run the new version.

Two things worth knowing about how it works:

- It reads the version out of the **302 redirect** on `/releases/latest` rather
  than calling the GitHub REST API. The API allows 60 requests per hour per IP
  and that budget is shared, so a self-updater built on it fails for reasons the
  user cannot see. The redirect has no such limit.
- **SSL verification is disabled** (`SSLCOPT_DisableVerify`). The 3DS
  certificate store predates GitHub's current CA chain, so the console's
  built-in check rejects the connection outright. That means the downloaded CIA
  is unauthenticated — it is trusted purely on the basis of coming from the
  expected URL.

Installing needs the `am:net` / `am:u` service access granted in
[`raytracer3ds.rsf`](raytracer3ds.rsf), so it only works from the installed CIA,
not from a `.3dsx` under the Homebrew Launcher.

## Building

Needs devkitPro with devkitARM and libctru.

```bash
make
```

That produces `raytracer3ds.3dsx` and `raytracer3ds.elf`.

### Build gotcha: no spaces in the path

The stock devkitPro template Makefile **cannot build from a directory whose path
contains a space**. Its self-recursion switch compares `$(BUILD)` against
`$(notdir $(CURDIR))`, and GNU Make word-splits the result, so the check never
matches and the Makefile recurses into itself instead of compiling:

```
make[1]: *** /c/Users/steve/Documents/fuck: Is a directory.  Stop.
make: *** [Makefile:168: all] Error 2
```

`arm-none-eabi-gcc` is never invoked. Clone to a path with no spaces.

### Build gotcha: use devkitPro's own MSYS2 shell

Run `make` from devkitPro's MSYS2 **login** shell
(`C:/devkitPro/msys2/usr/bin/bash.exe -l`), not from Git Bash or another MSYS2
install. The exported environment does not survive that runtime boundary, so
every `export` the devkitPro template Makefile performs is lost in the recursive
inner make. One cause, two very different-looking symptoms:

1. `DEVKITARM` reads empty →
   `*** "Please set DEVKITARM in your environment"`
2. `DEPSDIR` reads empty → the inner make runs `-include /*.d` and picks up
   **stray dependency files at the MSYS2 root**, left there by unrelated
   projects, so the build demands shader sources that do not exist in this
   repo at all.

Passing `DEVKITARM=` on the command line silences (1) but not (2), because the
other exported variables are still lost. Do not export `DEVKITARM` yourself
either — on a normal devkitPro install it is already set correctly, and
overwriting it with a Unix-form path reintroduces symptom (1).

## Verifying the maths

The per-pixel maths carries no library calls, which is a claim that can be
checked rather than taken on trust.

`source/main.c` compiles unmodified on a normal host PC — the harness
`#include`s it with `main` renamed aside and points it at a host-allocated
framebuffer, so the *real* file is verified, never a copy. That makes the render
byte-comparable against a reference image from the previous release.

What that harness established for this release:

- **Output is unchanged within one level in 255.** Replacing three `powf` calls
  per pixel with a gamma lookup table shifts some pixels by exactly ±1/255 and
  never more. Sweeping the table size showed the differing-pixel count falling
  ~4× for each 4× more entries (16.70% → 4.54% → 0.99% → 0.05%), converging
  toward zero — which proves the table's quantisation is the *entire* cause and
  exonerates the integer-exponent `powi` that replaced `powf` elsewhere.
- **Column interleave covers every pixel exactly once.** Rendering by slot with
  the buffer pre-filled with a poison byte, twice with *different* poison values,
  produces two byte-identical images — so no pixel was left untraced. Deliberately
  skipping one slot makes them differ on exactly 50.0% of pixels at 2 threads and
  33.25% at 3, which is precisely the fraction of columns dropped. The check can
  go red, for the right reason, at the right magnitude.
- **`powf` is gone from the render path.** Disassembling the shipped ELF finds
  exactly one call to it in the whole binary, inside `main` — the one-time table
  build. `render_columns` is 337 instructions calling out only to `__divsi3` and
  `trace`, and `sqrtf` compiles to a single hardware `vsqrt.f32`.

## Hot-path optimisations (v1.0.3)

Four changes, all in `source/main.c`, aimed at the ARM11 rather than the host:

- **`ifloor()` replaces `floorf()`.**

  ```c
  static inline int ifloor(float x) { int i = (int)x; return i - (x < (float)i); }
  ```

  `trace()` was calling `floorf` four times per hit — twice in
  `cube_face_color` for the cube's UV checker, twice in `scene_intersect` for
  the ground plane's checker. On ARM `floorf` is a real `bl` into libm; on x86
  it is a single hardware instruction, which is why the host harness cannot see
  this win at all. Measured on the ARM ELF, this change alone: `trace` went 672
  -> 626 instructions, and `floorf` call sites binary-wide went 8 -> 0. (The
  shipped v1.0.3 `trace` is 633, because the `powi` unrolling below trades six
  instructions back for a loop; see that bullet.)

- **Distance-fade early-out in `trace()`.** Past `FADE_END` (55.0) the distance
  fade is a full replacement of the surface colour by the sky, so the shadow
  ray, the Phong terms, and an entire recursive reflection were being computed
  and then discarded. `trace()` now returns `sky_color(rd)` immediately when
  `fabsf(h.n.y) > 0.5f && h.t >= FADE_END` — the same test the fade itself
  uses, so it cannot disagree with it: whenever it fires, the fade factor would
  have been exactly 1. Honest scope: the geometry works out to about 2.7% of
  pixels (`t >= FADE_END` needs `rd.y >= -0.026`, roughly 1.5 degrees of a
  55-degree vertical FOV, about 6 of 240 rows). Real but minor; kept because it
  is exact and free.

- **`powi()` now unrolls.** The specular term passed `h->shininess` — a
  runtime value — so `powi` stayed a real loop: eight iterations of
  test/multiply/square/shift inside the hottest function in the program. The
  scene only ever has two shininess values (`CUBE_SHININESS` 220,
  `PLANE_SHININESS` 90), so the call site now dispatches on which one it is and
  passes a compile-time constant, which lets GCC fold each into a
  straight-line chain of `vmul.f32`. Measured on the ARM ELF: the longest
  consecutive `vmul.f32` run inside `trace` went 3 -> 10 — that run *is* the
  unrolled chain — `vmul.f32` count in `trace` went 30 -> 44, and two backward
  loop branches disappeared. Price: +12 instructions binary-wide, i.e. +48 bytes
  on a 126,856-byte `.text`, 0.04%, to delete an eight-iteration branchy loop
  from the hottest function in the program.
  Caveat worth repeating: adding a third material means adding a case at that
  call site, or its specular highlight silently takes the plane's exponent.

- **Core diagnostics.** v1.0.2 printed `cores: 2` on a New 3DS, one fewer than
  expected, while the benchmark showed near-perfect 2.0x scaling — a throttled
  core 1 would not give a clean 2.0x, so the suspicion was that the syscore
  request had failed and the two cores in use were 0 and 2. That was
  inference, not measurement, so v1.0.3 measures it instead: `threads_init()`
  now retries `APT_SetAppCpuTimeLimit` at 80/70/50/30 percent instead of
  giving up after one attempt, and records the result. Both the on-screen
  banner and the benchmark `.txt` now print the actual core IDs in use and
  either the syscore percentage that was granted or
  `syscore REFUSED 0x........` with the real result code.

## Live half-resolution toggle (v1.0.4)

A single new control: pressing **X** in the live view toggles `g_cfg.scale`
between 1 (full 400x240, a ray per pixel) and 2 (200x120 traced, each sample
written as a 2x2 block). Nothing else changed — no rendering maths was
touched.

Why it exists: the
[v1.0.2 hardware measurements](#hardware-measurements-v102-new-3ds) showed
full resolution at 117.8 ms (8.49 fps) and half resolution *at full quality*
at 31.0 ms (32.26 fps), which makes the resolution/quality trade the only
remaining route to 20 fps. But there was no way to actually *look* at half
resolution — `g_cfg.scale` was only ever set by the benchmark sweep, which
restores it immediately. The toggle exists so the trade can be judged by eye
rather than from a table.

- **Applied between frames**, in the same place the other buttons are
  handled. That is safe without any lock because the render threads are
  joined before the loop reads input and the workers only ever read `g_cfg`
  — nothing is mid-render at that point.
- **Full coverage at half resolution.** `put_block` writes the full `scale`
  x `scale` block, and 400 and 240 are both divisible by 2, so toggling back
  to full leaves no stale half-res blocks on screen. Verified, not assumed:
  rendering twice at scale 2 with two different framebuffer poison fills
  (0x00 and 0xFF) produced identical output — 0 of 96000 pixels differ — and
  the check was proven able to go red by deliberately skipping one interleave
  slot, which produced exactly 50.0000% of pixels differing and a non-zero
  exit.
- **`print_render_mode()` prints every frame, not once at startup.** A
  one-shot print would go stale the moment X is pressed and the screen would
  claim 400x240 while half-resolution blocks were going up. It writes line 2
  of the bottom console at an absolute cursor position (`\x1b[2;1H`), which is
  why it cannot be called inline while the rest of the header is printed —
  doing so would leave the cursor mid-line and the next header line would
  land on top of it.
- **The control list stays five lines, ending on line 10.** The frame
  readout, the cache warning and `status()` address console lines 11, 12 and
  13+ absolutely, so a sixth line would be overwritten by the first rendered
  frame. The two-line "hold" note was condensed to one line to make room for
  the X entry. The controls are now: START exit / SELECT run benchmark / Y
  check for updates / X toggle half resolution.
- **No rendering change.** The host harness (`scratchpad/hostcheck`, which
  `#include`s the real `main.c`) shows full-resolution output bit-identical
  to v1.0.3 — 0 pixels differing out of 96000, max abs channel diff 0 — on
  both the `-O2` and the `-O3 -ffast-math` arms. Honest scope: this does not
  exercise the toggle itself, since the harness has no input. On ARM, `trace`
  is 633 instructions and `render_columns` 337, identical to v1.0.3; `.text`
  grew from 126,856 to 127,720 bytes (+864 B), all of it UI strings and the
  new helper — no cost in the render path.

The toggle has never been pressed on hardware. Specifically unverified: that
the bottom-screen console layout above is right — the line accounting is read
off the code, not seen on a console — and that X is not swallowed by the
"input is only sampled between frames" behaviour: at 118 ms per frame the
button must be held briefly for the tap to register, exactly like the
existing controls.

## Tunables

All at the top of [`source/main.c`](source/main.c):

| Define | Default | Effect |
| --- | --- | --- |
| `AA` | `2` | Supersampling grid; `AA*AA` rays per pixel. Set to `1` to roughly quarter the frame time. |
| `MAX_DEPTH` | `3` | Reflection bounces after the primary ray. |
| `FOV_DEG` | `55.0` | Vertical field of view. |
| `FADE_START` / `FADE_END` | `18` / `55` | Distance over which the ground plane blends into the sky. |
| `RAY_EPS` | `1e-3` | Surface offset that prevents shadow acne. |
| `GAMMA_LUT_SIZE` | `1024` | Gamma table entries. 1 KB stays resident in the ARM11's 16 KB L1 data cache. Larger tables cut the number of pixels showing the ±1/255 difference but not its magnitude. |
| `MAX_RENDER_THREADS` | `3` | Upper bound on render threads — one per usable core. |
| `SYSCORE_TIME_LIMIT` | `80` | Percent of core 1 requested from the OS. |

The table is indexed on `sqrt(linear)` rather than on the linear value. That
turns `v^(1/2.2)` into `255·s^(2/2.2)`, whose slope varies by less than 2× across
the whole range — so a small table is accurate everywhere, where a
linearly-indexed one of the same size would band visibly in the shadows.

`osSetSpeedupEnable(true)` in `main()` unlocks the 804 MHz clock on a New 3DS. It
is a documented no-op on an Old 3DS and changes nothing about what is rendered —
delete the line if you want the slower clock.

## Installing the CIA

Grab the `.cia` from [Releases](../../releases), copy it to your SD card, and
install it with FBI. Requires custom firmware.

## Install on your 3DS

![QR](meta/qr-latest.png)

Scan this in FBI's "Scan QR Code" to download and install the latest release
directly on the console.

Each release also gets its own branch and its own pinned QR code under `meta/`,
so a specific version can be installed rather than whatever is newest:

| Version | Branch | QR |
| --- | --- | --- |
| v1.0.1 | `v1.0.1` | `meta/qr-v1.0.1.png` |
| v1.0.2 | `v1.0.2` | `meta/qr-v1.0.2.png` |
| v1.0.3 | `v1.0.3` | `meta/qr-v1.0.3.png` |
| v1.0.4 | `v1.0.4` | `meta/qr-v1.0.4.png` |

## Art

The icon and banner in `meta/` are **placeholder art**, generated to make the
CIA build. Replace them and rebuild if you want something better.

## Verification status

**Verified by running something:**

- Compiles clean against libctru with `-Wall -O3 -ffast-math`: **zero warnings,
  zero errors**.
- The maths refactor is output-equivalent to v1.0.1 within one level in 255, and
  the column split covers every pixel exactly once — both measured, both with a
  check proven able to fail. See [Verifying the maths](#verifying-the-maths).
- `powf` is absent from the render path in the **shipped ELF**, confirmed by
  disassembling that artifact rather than by reading the source.
- **Real frame times, and that multicore actually helps.** Measured on a New
  3DS: full-resolution 1AA renders scale x1 -> x2 at 235.5 ms -> 117.8 ms,
  1.999x on two cores. See
  [Hardware measurements](#hardware-measurements-v102-new-3ds).
- **The cross-core cache clean.** No vertical stripes of stale pixels appeared
  anywhere in the v1.0.2 benchmark sweep on that New 3DS — the first time
  `svcStoreProcessDataCache` had ever actually executed outside compilation,
  and it held up.
- **v1.0.4's half-resolution toggle changes no rendering.** The host harness
  shows full-resolution output bit-identical to v1.0.3 — 0 pixels differing
  out of 96000, max abs channel diff 0 — on both the `-O2` and the
  `-O3 -ffast-math` arms, and on ARM `trace` (633 instructions) and
  `render_columns` (337) are unchanged from v1.0.3. See
  [Live half-resolution toggle](#live-half-resolution-toggle-v104).
- **Half-resolution coverage is complete.** Rendering twice at scale 2 with
  two different framebuffer poison fills produced identical output — 0 of
  96000 pixels differ — and the check was proven able to go red:
  deliberately skipping one interleave slot at scale 2 produced exactly
  50.0000% of pixels differing.

**Not verified — these need hardware:**

- **Phase 3's speed on hardware, and the core-diagnostic readout.** v1.0.3 has
  been proven bit-identical to v1.0.2 on the host harness — 0 pixels differing
  on both the `-O2` and the `-O3 -ffast-math` arms — and the ARM instruction
  counts in [Hot-path optimisations](#hot-path-optimisations-v103) are real,
  measured on the shipped ELF. But none of it has run on a console: the
  `ifloor`/early-out/`powi` wins are unmeasured in milliseconds, and the
  retried `APT_SetAppCpuTimeLimit` ladder and the core-ID/syscore-percentage
  readout have never been seen on real hardware.
- **How it looks.**
- **The entire updater.** The HTTP request, the redirect parsing, the download,
  and the AM install have never been executed — only compiled.
- **The v1.0.4 half-resolution toggle, on hardware.** X has never been pressed
  on a console: the bottom-screen console layout is read off the code, not
  seen on a console, and it is unverified that X is not swallowed by the
  "input is only sampled between frames" behaviour — at 118 ms per frame the
  button must be held briefly for the tap to register, exactly like the
  existing controls.

Ray/box normals, camera basis, reflection vectors, and recursion depth were
hand-derived and check out, but that is reasoning, not a measurement.

## Licence

None specified.
