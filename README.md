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
| 1 | always | `APT_SetAppCpuTimeLimit(80)` — 80% of the system core |
| 2 | New 3DS only | exheader kernel flag `0x2000`, set by `CanAccessCore2` in the RSF |

Core 2 is therefore **CIA-only**: a `.3dsx` under the Homebrew Launcher has no
exheader of its own and never gets it. Core 3 is not available to applications.

Two things about this design are deliberate:

- **Columns, not rows.** A framebuffer column is one contiguous 720-byte run in
  `GSP_BGR8_OES` layout, so each thread's inner loop is sequential *and* no two
  threads ever share a cache line. Row-interleaving would put every thread in the
  same line on every single pixel.
- **Static split, no work stealing.** It needs zero cross-core synchronisation and
  has no failure mode that can corrupt the image. Core 1 is throttled to 80% and
  so finishes late; rather than guess a weighting to correct for that, the
  benchmark measures the real x1/x2/x3 scaling on the console.

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
| Y | Check GitHub for a newer release, and install it |
| SELECT | Run the benchmark sweep |

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
  build. `render_columns` is 338 instructions calling out only to `__divsi3` and
  `trace`, and `sqrtf` compiles to a single hardware `vsqrt.f32`.

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

**Not verified — these need hardware:**

- **Real frame times, and whether multicore actually helps.** Nothing in this
  release has been run on a console or an emulator. The benchmark exists
  precisely because these numbers have to be measured, not predicted; the x1/x2/x3
  cases are the ones that answer it.
- **The cross-core cache clean.** The host harness deliberately stubs threading
  out so the maths could be isolated, which means `svcStoreProcessDataCache` has
  never actually executed. If it were wrong, the symptom would be vertical stripes
  of stale pixels on the top screen. The failure path prints its error code on the
  bottom screen rather than failing silently.
- **How it looks.**
- **The entire updater.** The HTTP request, the redirect parsing, the download,
  and the AM install have never been executed — only compiled.

Ray/box normals, camera basis, reflection vectors, and recursion depth were
hand-derived and check out, but that is reasoning, not a measurement.

## Licence

None specified.
