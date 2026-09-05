# raytracer3ds

A software ray tracer for the Nintendo 3DS. No GPU, no PICA200 — every pixel on
the top screen is traced on the ARM11 and written straight into the linear
framebuffer.

Quality is the target, not speed. It runs at seconds per frame and that is the
intended trade.

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

**SELECT** sweeps nine quality configurations, timing a real full-screen render
for each on the console itself — nothing synthetic, every case goes through the
ordinary render path. Results print as a `config / ms / fps` table on the bottom
screen and are written to `sdmc:/raytracer3ds_bench.txt`. **B** aborts the sweep.

The cases isolate one lever each, so the cost of supersampling, of each
reflection bounce, of the shadow rays, and of resolution can be read separately
rather than guessed at:

| Case | Isolates |
| --- | --- |
| `2AA d3 full` | the shipped default |
| `1AA d3 full` | cost of 2×2 supersampling |
| `1AA d2 / d1 / d0 full` | cost of each reflection bounce |
| `1AA d1 full ns` | cost of the shadow rays |
| `1AA d3 half`, `1AA d1 half`, `1AA d3 qtr` | cost of resolution |

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

## Tunables

All at the top of [`source/main.c`](source/main.c):

| Define | Default | Effect |
| --- | --- | --- |
| `AA` | `2` | Supersampling grid; `AA*AA` rays per pixel. Set to `1` to roughly quarter the frame time. |
| `MAX_DEPTH` | `3` | Reflection bounces after the primary ray. |
| `FOV_DEG` | `55.0` | Vertical field of view. |
| `FADE_START` / `FADE_END` | `18` / `55` | Distance over which the ground plane blends into the sky. |
| `RAY_EPS` | `1e-3` | Surface offset that prevents shadow acne. |

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

## Art

The icon and banner in `meta/` are **placeholder art**, generated to make the
CIA build. Replace them and rebuild if you want something better.

## Verification status

- Compiles clean against libctru with `-Wall -O2`: **zero warnings, zero errors**.
- Ray/box normals, camera basis, reflection vectors, and recursion depth were
  hand-derived and check out.
- **Not verified:** how it actually looks, and its real frame time. Nothing here
  has been run on hardware or in an emulator.
- **Not verified:** the entire updater. The HTTP request, the redirect parsing,
  the download, and the AM install have never been executed — only compiled.

## Licence

None specified.
