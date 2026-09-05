/*
 * main.c -- Software ray tracer for the Nintendo 3DS (ARM11, libctru / devkitARM)
 *
 * Renders a mirror-finish unit cube resting on a reflective checkerboard plane
 * directly into the top screen's linear framebuffer.
 *
 * Pipeline per pixel:
 *   primary ray -> nearest hit (ray/box slab test or ray/plane test)
 *              -> Phong shading (ambient + diffuse + specular) from one
 *                 directional light, gated by a hard shadow ray
 *              -> recursive reflection ray, up to MAX_DEPTH bounces
 *              -> tone clamp + gamma, packed to BGR8 in the framebuffer
 *
 * Quality is the target, not speed. Expect seconds per frame on an Old 3DS.
 * Tunables live in the CONFIG block below.
 *
 * Build: drop this file into the `source/` directory of the standard devkitPro
 *        3DS application template and run `make` (template LIBS already has
 *        -lctru -lm, which is all this needs).
 *
 * Controls: START quits.
 */

#include <3ds.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ CONFIG */

#define SCREEN_W 400 /* top screen, displayed width  */
#define SCREEN_H 240 /* top screen, displayed height */

#define MAX_DEPTH 3   /* reflection bounces after the primary ray (spec: >= 2) */
#define AA 2          /* supersampling: AA*AA primary rays per pixel; 1 = off  */
#define RAY_EPS 1e-3f /* surface offset to stop self-intersection acne         */
#define FAR_T 1e30f   /* "no hit" distance                                     */

#define FOV_DEG 55.0f     /* vertical field of view                            */
#define FADE_START 18.0f  /* plane starts blending into the sky here...        */
#define FADE_END 55.0f    /* ...and is fully sky here (kills checker moire)    */

/* ------------------------------------------------------------------ VECTORS */

typedef struct {
	float x, y, z;
} Vec3;

static inline Vec3 v3(float x, float y, float z)
{
	Vec3 v = { x, y, z };
	return v;
}

static inline Vec3 v_add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 v_sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 v_scale(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline Vec3 v_mul(Vec3 a, Vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float v_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static inline Vec3 v_cross(Vec3 a, Vec3 b)
{
	return v3(a.y * b.z - a.z * b.y,
	          a.z * b.x - a.x * b.z,
	          a.x * b.y - a.y * b.x);
}

static inline Vec3 v_norm(Vec3 a)
{
	float len = sqrtf(v_dot(a, a));
	return (len > 1e-12f) ? v_scale(a, 1.0f / len) : a;
}

/* Mirror an incident direction about a (unit) surface normal. */
static inline Vec3 v_reflect(Vec3 incident, Vec3 n)
{
	return v_sub(incident, v_scale(n, 2.0f * v_dot(incident, n)));
}

static inline Vec3 v_lerp(Vec3 a, Vec3 b, float t)
{
	return v3(a.x + (b.x - a.x) * t,
	          a.y + (b.y - a.y) * t,
	          a.z + (b.z - a.z) * t);
}

static inline float clampf(float v, float lo, float hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

/* -------------------------------------------------------------------- SCENE */

/* Unit cube, sitting on the plane so its base and the plane meet exactly. */
static const Vec3 BOX_MIN = { -0.5f, 0.0f, -0.5f };
static const Vec3 BOX_MAX = { 0.5f, 1.0f, 0.5f };

#define PLANE_Y 0.0f /* infinite ground plane, normal +Y */

/* Camera. Static per frame -- every frame re-traces the same view. */
static const Vec3 CAM_POS = { 2.35f, 1.45f, 3.30f };
static const Vec3 CAM_TARGET = { 0.0f, 0.45f, 0.0f };
static const Vec3 WORLD_UP = { 0.0f, 1.0f, 0.0f };

/* Directional light: LIGHT_DIR points *towards* the light. */
static const Vec3 LIGHT_DIR = { 0.512459f, 0.745423f, 0.426299f }; /* unit length */
static const Vec3 LIGHT_COLOR = { 1.00f, 0.97f, 0.90f };

static const Vec3 AMBIENT = { 0.10f, 0.12f, 0.16f };

/* Cube material: tinted metal. Reflections are multiplied by REFL_TINT, which
 * is what gives the mirror a metallic colour rather than a plain silver one. */
static const Vec3 CUBE_TINT_A = { 0.95f, 0.72f, 0.34f }; /* checker light square */
static const Vec3 CUBE_TINT_B = { 0.55f, 0.32f, 0.12f }; /* checker dark square  */
static const Vec3 CUBE_REFL_TINT = { 0.98f, 0.84f, 0.58f };
#define CUBE_REFLECTIVITY 0.82f
#define CUBE_SHININESS 220.0f
#define CUBE_SPEC 1.15f

/* Plane material: polished checkerboard, Fresnel-weighted reflection. */
static const Vec3 PLANE_LIGHT = { 0.88f, 0.88f, 0.90f };
static const Vec3 PLANE_DARK = { 0.06f, 0.07f, 0.09f };
static const Vec3 PLANE_REFL_TINT = { 0.92f, 0.94f, 1.00f };
#define PLANE_BASE_REFLECT 0.28f /* head-on reflectivity; Fresnel raises it */
#define PLANE_SHININESS 90.0f
#define PLANE_SPEC 0.55f

/* ------------------------------------------------------------ INTERSECTIONS */

typedef struct {
	float t;           /* distance along the ray                       */
	Vec3 p;            /* hit position                                 */
	Vec3 n;            /* unit surface normal, facing the incoming ray */
	Vec3 albedo;       /* diffuse colour at the hit                    */
	Vec3 refl_tint;    /* colour multiplier applied to reflected light */
	float reflectivity;
	float shininess;
	float spec;
} Hit;

/*
 * Ray/AABB via the slab method. Records which slab produced the entry point so
 * the face normal falls straight out of the test -- no extra work, and it is
 * exact rather than derived from the hit position.
 */
static int hit_box(Vec3 ro, Vec3 rd, float t_min, float t_max,
                   float *t_out, Vec3 *n_out)
{
	const float o[3] = { ro.x, ro.y, ro.z };
	const float d[3] = { rd.x, rd.y, rd.z };
	const float bmin[3] = { BOX_MIN.x, BOX_MIN.y, BOX_MIN.z };
	const float bmax[3] = { BOX_MAX.x, BOX_MAX.y, BOX_MAX.z };

	float t_near = -FAR_T;
	float t_far = FAR_T;
	int axis = -1;
	float sign = -1.0f;

	for (int a = 0; a < 3; a++) {
		if (fabsf(d[a]) < 1e-8f) {
			/* Ray is parallel to this slab: it either misses outright
			 * or this axis places no constraint on t. */
			if (o[a] < bmin[a] || o[a] > bmax[a])
				return 0;
			continue;
		}

		const float inv = 1.0f / d[a];
		float t0 = (bmin[a] - o[a]) * inv;
		float t1 = (bmax[a] - o[a]) * inv;
		float face = -1.0f; /* entered through the min face */

		if (t0 > t1) {
			const float tmp = t0;
			t0 = t1;
			t1 = tmp;
			face = 1.0f; /* entered through the max face */
		}

		if (t0 > t_near) {
			t_near = t0;
			axis = a;
			sign = face;
		}
		if (t1 < t_far)
			t_far = t1;

		if (t_near > t_far)
			return 0;
	}

	if (axis < 0 || t_near < t_min || t_near > t_max)
		return 0;

	*t_out = t_near;
	*n_out = v3(axis == 0 ? sign : 0.0f,
	            axis == 1 ? sign : 0.0f,
	            axis == 2 ? sign : 0.0f);
	return 1;
}

/* Ray/plane for the axis-aligned ground plane y = PLANE_Y. */
static int hit_plane(Vec3 ro, Vec3 rd, float t_min, float t_max, float *t_out)
{
	if (fabsf(rd.y) < 1e-8f)
		return 0;

	const float t = (PLANE_Y - ro.y) / rd.y;
	if (t < t_min || t > t_max)
		return 0;

	*t_out = t;
	return 1;
}

/* Procedural checker on a cube face: uses the two axes that are not the face
 * normal, so the pattern stays square on every side of the cube. */
static Vec3 cube_face_color(Vec3 p, Vec3 n)
{
	float u, v;

	if (fabsf(n.x) > 0.5f) {
		u = p.z;
		v = p.y;
	} else if (fabsf(n.y) > 0.5f) {
		u = p.x;
		v = p.z;
	} else {
		u = p.x;
		v = p.y;
	}

	const int cell = (int)floorf(u * 4.0f) + (int)floorf(v * 4.0f);
	return ((cell & 1) == 0) ? CUBE_TINT_A : CUBE_TINT_B;
}

/* Nearest-hit query over the whole scene. Fills *h on a hit. */
static int scene_intersect(Vec3 ro, Vec3 rd, float t_min, float t_max, Hit *h)
{
	int found = 0;
	float best = t_max;

	float t;
	Vec3 n;

	if (hit_box(ro, rd, t_min, best, &t, &n)) {
		best = t;
		found = 1;

		h->t = t;
		h->p = v_add(ro, v_scale(rd, t));
		h->n = n;
		h->albedo = cube_face_color(h->p, n);
		h->refl_tint = CUBE_REFL_TINT;
		h->reflectivity = CUBE_REFLECTIVITY;
		h->shininess = CUBE_SHININESS;
		h->spec = CUBE_SPEC;
	}

	if (hit_plane(ro, rd, t_min, best, &t)) {
		best = t;
		found = 1;

		h->t = t;
		h->p = v_add(ro, v_scale(rd, t));
		h->n = v3(0.0f, (rd.y < 0.0f) ? 1.0f : -1.0f, 0.0f);

		const int cell = (int)floorf(h->p.x) + (int)floorf(h->p.z);
		h->albedo = ((cell & 1) == 0) ? PLANE_LIGHT : PLANE_DARK;
		h->refl_tint = PLANE_REFL_TINT;

		/* Schlick's approximation: a polished floor reflects far more at
		 * grazing angles than head-on. This is what sells "wet marble". */
		const float cos_theta = clampf(-v_dot(rd, h->n), 0.0f, 1.0f);
		const float f = 1.0f - cos_theta;
		const float f5 = f * f * f * f * f;
		h->reflectivity = PLANE_BASE_REFLECT +
		                  (1.0f - PLANE_BASE_REFLECT) * f5;

		h->shininess = PLANE_SHININESS;
		h->spec = PLANE_SPEC;
	}

	return found;
}

/* Shadow ray: any hit inside (t_min, t_max) occludes -- no need for the nearest. */
static int scene_occluded(Vec3 ro, Vec3 rd, float t_min, float t_max)
{
	float t;
	Vec3 n;

	if (hit_box(ro, rd, t_min, t_max, &t, &n))
		return 1;
	if (hit_plane(ro, rd, t_min, t_max, &t))
		return 1;

	return 0;
}

/* ------------------------------------------------------------ SKY / SHADING */

/* Sky gradient plus a small sun disc. The sun disc matters: it is what the
 * mirror faces catch, and it is the proof that reflections sample the world. */
static Vec3 sky_color(Vec3 rd)
{
	const Vec3 horizon = v3(0.78f, 0.85f, 0.95f);
	const Vec3 zenith = v3(0.16f, 0.34f, 0.74f);

	const float t = clampf(0.5f * (rd.y + 1.0f), 0.0f, 1.0f);
	Vec3 c = v_lerp(horizon, zenith, t);

	const float sun = v_dot(rd, LIGHT_DIR);
	if (sun > 0.0f) {
		const float disc = powf(sun, 1200.0f) * 9.0f;
		const float glow = powf(sun, 24.0f) * 0.35f;
		c = v_add(c, v_scale(LIGHT_COLOR, disc + glow));
	}

	return c;
}

/* Direct lighting at a hit: ambient + Lambert diffuse + Phong specular,
 * with the diffuse and specular terms killed by a hard shadow ray. */
static Vec3 shade_direct(const Hit *h, Vec3 view_dir)
{
	Vec3 color = v_mul(AMBIENT, h->albedo);

	const float n_dot_l = v_dot(h->n, LIGHT_DIR);
	if (n_dot_l <= 0.0f)
		return color; /* facing away from the light: ambient only */

	const Vec3 shadow_origin = v_add(h->p, v_scale(h->n, RAY_EPS));
	if (scene_occluded(shadow_origin, LIGHT_DIR, RAY_EPS, FAR_T))
		return color;

	/* Diffuse */
	const Vec3 diffuse = v_scale(v_mul(h->albedo, LIGHT_COLOR), n_dot_l);
	color = v_add(color, diffuse);

	/* Specular: classic Phong, reflected light vs. the view direction. */
	const Vec3 light_refl = v_reflect(v_scale(LIGHT_DIR, -1.0f), h->n);
	const float r_dot_v = v_dot(light_refl, v_scale(view_dir, -1.0f));
	if (r_dot_v > 0.0f) {
		const float s = powf(r_dot_v, h->shininess) * h->spec;
		color = v_add(color, v_scale(LIGHT_COLOR, s));
	}

	return color;
}

/*
 * Recursive trace. depth 0 is the primary ray; each reflection adds one.
 * Bounded by MAX_DEPTH, so the stack cost is trivially small.
 */
static Vec3 trace(Vec3 ro, Vec3 rd, int depth)
{
	Hit h;
	if (!scene_intersect(ro, rd, RAY_EPS, FAR_T, &h))
		return sky_color(rd);

	Vec3 color = shade_direct(&h, rd);

	if (depth < MAX_DEPTH && h.reflectivity > 0.001f) {
		const Vec3 refl_dir = v_norm(v_reflect(rd, h.n));
		const Vec3 refl_origin = v_add(h.p, v_scale(h.n, RAY_EPS));
		const Vec3 reflected = trace(refl_origin, refl_dir, depth + 1);

		/* Tinting the reflected radiance is what makes the cube read as
		 * coloured metal instead of a neutral mirror. */
		color = v_lerp(color, v_mul(reflected, h.refl_tint), h.reflectivity);
	}

	/* Fade the infinite plane out into the sky in the far distance. Without
	 * this the checker aliases into noise at the horizon. */
	if (fabsf(h.n.y) > 0.5f && h.t > FADE_START) {
		const float f = clampf((h.t - FADE_START) / (FADE_END - FADE_START),
		                       0.0f, 1.0f);
		color = v_lerp(color, sky_color(rd), f);
	}

	return color;
}

/* -------------------------------------------------------------- FRAMEBUFFER */

/*
 * The top screen framebuffer is GSP_BGR8_OES: 3 bytes per pixel, stored in
 * column-major order with the origin at the bottom-left. So the pixel at
 * display coordinate (x, y), y measured downwards from the top, lives at
 * byte offset 3 * (x * SCREEN_H + (SCREEN_H - 1 - y)).
 */
static inline void put_pixel(u8 *fb, int x, int y, Vec3 c)
{
	/* Clamp, then gamma-encode from linear to sRGB-ish for display. */
	const float inv_gamma = 1.0f / 2.2f;
	const float r = powf(clampf(c.x, 0.0f, 1.0f), inv_gamma);
	const float g = powf(clampf(c.y, 0.0f, 1.0f), inv_gamma);
	const float b = powf(clampf(c.z, 0.0f, 1.0f), inv_gamma);

	u8 *px = fb + 3 * (x * SCREEN_H + (SCREEN_H - 1 - y));
	px[0] = (u8)(b * 255.0f + 0.5f);
	px[1] = (u8)(g * 255.0f + 0.5f);
	px[2] = (u8)(r * 255.0f + 0.5f);
}

/* ------------------------------------------------------------------ CAMERA */

typedef struct {
	Vec3 origin;
	Vec3 right;
	Vec3 up;
	Vec3 forward;
	float tan_half_fov;
	float aspect;
} Camera;

static void camera_setup(Camera *cam)
{
	cam->origin = CAM_POS;
	cam->forward = v_norm(v_sub(CAM_TARGET, CAM_POS));
	cam->right = v_norm(v_cross(cam->forward, WORLD_UP));
	cam->up = v_cross(cam->right, cam->forward);
	cam->tan_half_fov = tanf(FOV_DEG * 0.5f * (float)M_PI / 180.0f);
	cam->aspect = (float)SCREEN_W / (float)SCREEN_H;
}

/* Primary ray through a point on the image plane, in pixel coordinates. */
static Vec3 camera_ray(const Camera *cam, float px, float py)
{
	const float sx = (2.0f * (px / (float)SCREEN_W) - 1.0f) *
	                 cam->aspect * cam->tan_half_fov;
	const float sy = (1.0f - 2.0f * (py / (float)SCREEN_H)) * cam->tan_half_fov;

	Vec3 dir = v_add(v_add(v_scale(cam->right, sx), v_scale(cam->up, sy)),
	                 cam->forward);
	return v_norm(dir);
}

/* ------------------------------------------------------------------ RENDER */

static void render_frame(u8 *fb, const Camera *cam)
{
	const float inv_samples = 1.0f / (float)(AA * AA);

	for (int y = 0; y < SCREEN_H; y++) {
		for (int x = 0; x < SCREEN_W; x++) {
			Vec3 acc = v3(0.0f, 0.0f, 0.0f);

			/* Regular AA x AA grid of sample positions inside the pixel. */
			for (int sy = 0; sy < AA; sy++) {
				for (int sx = 0; sx < AA; sx++) {
					const float ox = ((float)sx + 0.5f) / (float)AA;
					const float oy = ((float)sy + 0.5f) / (float)AA;

					const Vec3 dir = camera_ray(cam,
					                            (float)x + ox,
					                            (float)y + oy);
					acc = v_add(acc, trace(cam->origin, dir, 0));
				}
			}

			put_pixel(fb, x, y, v_scale(acc, inv_samples));
		}
	}
}

/* -------------------------------------------------------------------- MAIN */

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	gfxInitDefault();
	gfxSet3D(false);

	/* On a New 3DS this unlocks the 804MHz clock; on an Old 3DS it is a
	 * no-op. Purely a speed knob -- it does not change what is rendered. */
	osSetSpeedupEnable(true);

	/* Bottom screen carries the status readout so the top screen stays a
	 * clean render target. */
	consoleInit(GFX_BOTTOM, NULL);
	printf("3DS software ray tracer\n");
	printf("%dx%d  %dx%d AA  %d bounces\n",
	       SCREEN_W, SCREEN_H, AA, AA, MAX_DEPTH);
	printf("\nPress START to exit.\n\n");

	Camera cam;
	camera_setup(&cam);

	u32 frame = 0;

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		u16 fb_w = 0, fb_h = 0;
		u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h);

		const u64 t_start = osGetTime();
		render_frame(fb, &cam);
		const u64 elapsed = osGetTime() - t_start;

		frame++;
		printf("\x1b[7;1Hframe %-6lu  %6llu ms  %5.2f fps",
		       (unsigned long)frame,
		       (unsigned long long)elapsed,
		       elapsed ? 1000.0 / (double)elapsed : 0.0);

		/* Push the CPU-written pixels out of cache, then present. */
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	gfxExit();
	return 0;
}
