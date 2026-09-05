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

The bottom screen shows frame count, frame time in milliseconds, and fps.

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

## Art

The icon and banner in `meta/` are **placeholder art**, generated to make the
CIA build. Replace them and rebuild if you want something better.

## Verification status

- Compiles clean against libctru with `-Wall -O2`: **zero warnings, zero errors**.
- Ray/box normals, camera basis, reflection vectors, and recursion depth were
  hand-derived and check out.
- **Not verified:** how it actually looks, and its real frame time. Nothing here
  has been run on hardware or in an emulator.

## Licence

None specified.
