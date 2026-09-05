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
#include <stdarg.h>
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

/* Updater. APP_VERSION is compared against the tag of the latest GitHub
 * release -- bump it whenever a new release is cut, or the app will offer to
 * install a build it is already running. */
#define APP_VERSION "1.0.1"
#define UPDATE_LATEST_URL \
	"https://github.com/stevenjc2009-byte/raytracer3ds/releases/latest"
#define UPDATE_CIA_URL \
	"https://github.com/stevenjc2009-byte/raytracer3ds/releases/latest/download/raytracer3ds.cia"
#define UPDATE_USER_AGENT "raytracer3ds-updater/" APP_VERSION
#define DL_CHUNK (16 * 1024) /* download / CIA-write granularity */
#define MAX_REDIRECTS 8      /* redirect chain depth before giving up */

/* ---------------------------------------------------- RUNTIME RENDER CONFIG */

/*
 * The quality knobs are runtime state rather than compile-time constants, so
 * the benchmark can sweep them in a single run on real hardware. Treated as
 * read-only for the duration of a render, which is what will make it safe to
 * share across render threads later.
 */
typedef struct {
	int aa;      /* samples per axis; 1 disables supersampling            */
	int depth;   /* reflection bounces after the primary ray              */
	int scale;   /* 1 = a ray per pixel; 2 = a ray per 2x2 block, etc.    */
	int shadows; /* 0 skips the shadow ray entirely                       */
} RenderConfig;

/* Defaults match the compile-time constants above, so behaviour is unchanged
 * until something deliberately overwrites this. */
static RenderConfig g_cfg = { AA, MAX_DEPTH, 1, 1 };

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

	if (g_cfg.shadows) {
		const Vec3 shadow_origin = v_add(h->p, v_scale(h->n, RAY_EPS));
		if (scene_occluded(shadow_origin, LIGHT_DIR, RAY_EPS, FAR_T))
			return color;
	}

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

	if (depth < g_cfg.depth && h.reflectivity > 0.001f) {
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
/*
 * Write one traced sample as a `scale` x `scale` block of framebuffer pixels.
 * At scale 1 this is a single pixel and the loops collapse; above that, one ray
 * covers the block, which is where the big frame-time savings come from.
 */
static inline void put_block(u8 *fb, int x0, int y0, int scale, Vec3 c)
{
	/* Clamp, then gamma-encode from linear to sRGB-ish for display. */
	const float inv_gamma = 1.0f / 2.2f;
	const u8 r = (u8)(powf(clampf(c.x, 0.0f, 1.0f), inv_gamma) * 255.0f + 0.5f);
	const u8 g = (u8)(powf(clampf(c.y, 0.0f, 1.0f), inv_gamma) * 255.0f + 0.5f);
	const u8 b = (u8)(powf(clampf(c.z, 0.0f, 1.0f), inv_gamma) * 255.0f + 0.5f);

	for (int dx = 0; dx < scale; dx++) {
		const int x = x0 + dx;
		if (x >= SCREEN_W)
			break;

		for (int dy = 0; dy < scale; dy++) {
			const int y = y0 + dy;
			if (y >= SCREEN_H)
				break;

			u8 *px = fb + 3 * (x * SCREEN_H + (SCREEN_H - 1 - y));
			px[0] = b;
			px[1] = g;
			px[2] = r;
		}
	}
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

/*
 * Trace the band of block-rows [row_begin, row_end) into the framebuffer.
 *
 * Split out from render_frame so a worker thread can be handed a slice of the
 * image; the bands touch disjoint pixels, so they need no locking.
 */
static void render_band(u8 *fb, const Camera *cam, int row_begin, int row_end)
{
	const int scale = g_cfg.scale;
	const int aa = g_cfg.aa;
	const int cols = SCREEN_W / scale;
	const float inv_samples = 1.0f / (float)(aa * aa);

	for (int y = row_begin; y < row_end; y++) {
		for (int x = 0; x < cols; x++) {
			Vec3 acc = v3(0.0f, 0.0f, 0.0f);

			/* Regular aa x aa grid of sample positions inside the block. */
			for (int sy = 0; sy < aa; sy++) {
				for (int sx = 0; sx < aa; sx++) {
					const float ox = ((float)sx + 0.5f) / (float)aa;
					const float oy = ((float)sy + 0.5f) / (float)aa;

					/* Block coordinates scale back up to full-res
					 * screen space before ray generation, so the
					 * camera never has to know the render scale. */
					const Vec3 dir = camera_ray(
					        cam,
					        ((float)x + ox) * (float)scale,
					        ((float)y + oy) * (float)scale);

					acc = v_add(acc, trace(cam->origin, dir, 0));
				}
			}

			put_block(fb, x * scale, y * scale, scale,
			          v_scale(acc, inv_samples));
		}
	}
}

static void render_frame(u8 *fb, const Camera *cam)
{
	render_band(fb, cam, 0, SCREEN_H / g_cfg.scale);
}

/* ----------------------------------------------------------------- DISPLAY */

/* Flush the CPU-written pixels out of cache and present both screens. */
static void present(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

/* Write a status line into the updater's region of the bottom console.
 * Row 10 and below; \x1b[K clears the rest of the line so shorter messages
 * do not leave tails of longer ones behind. */
static void status(int row, const char *fmt, ...)
{
	va_list args;

	printf("\x1b[%d;1H\x1b[K", 13 + row);

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);

	present();
}

/* ---------------------------------------------------------------- UPDATER */

static void parse_version(const char *s, int out[3])
{
	out[0] = out[1] = out[2] = 0;

	if (*s == 'v' || *s == 'V')
		s++;

	int field = 0;
	while (*s && field < 3) {
		if (*s >= '0' && *s <= '9')
			out[field] = out[field] * 10 + (*s - '0');
		else if (*s == '.')
			field++;
		else
			break; /* stop at any suffix, e.g. "1.2.0-beta" */
		s++;
	}
}

/* Strictly newer, so re-running the current version never offers an install. */
static int version_is_newer(const char *candidate, const char *current)
{
	int a[3], b[3];

	parse_version(candidate, a);
	parse_version(current, b);

	for (int i = 0; i < 3; i++) {
		if (a[i] != b[i])
			return a[i] > b[i];
	}

	return 0;
}

static int is_redirect(u32 status)
{
	return status == 301 || status == 302 || status == 303 ||
	       status == 307 || status == 308;
}

/*
 * Open a GET and send the request headers.
 *
 * SSL verification is disabled: the 3DS certificate store predates GitHub's
 * current CA chain, so the built-in check rejects the connection outright.
 * That makes the response untrusted input -- it is written straight into a CIA
 * install handle, and nothing here authenticates it beyond coming from the
 * expected host.
 */
static Result http_open(httpcContext *ctx, const char *url)
{
	Result rc = httpcOpenContext(ctx, HTTPC_METHOD_GET, url, 1);
	if (R_FAILED(rc))
		return rc;

	httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
	httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
	httpcAddRequestHeaderField(ctx, "User-Agent", UPDATE_USER_AGENT);
	httpcAddRequestHeaderField(ctx, "Connection", "Keep-Alive");

	return httpcBeginRequest(ctx);
}

/*
 * Read the tag of the latest release out of the redirect that GitHub serves
 * for /releases/latest -- its Location ends in /releases/tag/vX.Y.Z.
 *
 * Deliberately NOT the REST API: that is rate limited to 60 requests per hour
 * per IP, and the limit is shared, so it is routinely already exhausted before
 * the console ever asks. The redirect has no such limit.
 */
static int update_fetch_latest_tag(char *tag, size_t tag_size)
{
	httpcContext ctx;
	char location[512];
	u32 code = 0;

	Result rc = http_open(&ctx, UPDATE_LATEST_URL);
	if (R_FAILED(rc)) {
		status(0, "Request failed: 0x%08lX", (unsigned long)rc);
		return 0;
	}

	rc = httpcGetResponseStatusCode(&ctx, &code);
	if (R_FAILED(rc)) {
		status(0, "No status code: 0x%08lX", (unsigned long)rc);
		httpcCloseContext(&ctx);
		return 0;
	}

	if (!is_redirect(code)) {
		status(0, "Expected a redirect, got HTTP %lu", (unsigned long)code);
		httpcCloseContext(&ctx);
		return 0;
	}

	rc = httpcGetResponseHeader(&ctx, "Location", location, sizeof(location));
	httpcCloseContext(&ctx);

	if (R_FAILED(rc)) {
		status(0, "No Location header: 0x%08lX", (unsigned long)rc);
		return 0;
	}

	const char *last = strrchr(location, '/');
	if (!last || !last[1]) {
		status(0, "Could not parse a tag from the redirect");
		return 0;
	}

	strncpy(tag, last + 1, tag_size - 1);
	tag[tag_size - 1] = '\0';
	return 1;
}

/*
 * Download the release asset and stream it straight into a CIA install handle.
 *
 * libctru's httpc does not follow redirects, and the asset URL bounces to
 * objects.githubusercontent.com, so the chain is walked by hand. The payload is
 * never buffered whole -- each chunk goes to the install handle as it arrives,
 * so memory use is DL_CHUNK regardless of how large the CIA grows.
 */
static int update_download_and_install(void)
{
	char url[512];
	char location[512];
	httpcContext ctx;
	u32 code = 0;
	int hop;

	strncpy(url, UPDATE_CIA_URL, sizeof(url) - 1);
	url[sizeof(url) - 1] = '\0';

	for (hop = 0; hop <= MAX_REDIRECTS; hop++) {
		Result rc = http_open(&ctx, url);
		if (R_FAILED(rc)) {
			status(0, "Download request failed: 0x%08lX", (unsigned long)rc);
			return 0;
		}

		rc = httpcGetResponseStatusCode(&ctx, &code);
		if (R_FAILED(rc)) {
			status(0, "No status code: 0x%08lX", (unsigned long)rc);
			httpcCloseContext(&ctx);
			return 0;
		}

		if (!is_redirect(code))
			break;

		rc = httpcGetResponseHeader(&ctx, "Location", location,
		                            sizeof(location));
		httpcCloseContext(&ctx);
		if (R_FAILED(rc)) {
			status(0, "Redirect with no Location: 0x%08lX",
			       (unsigned long)rc);
			return 0;
		}

		strncpy(url, location, sizeof(url) - 1);
		url[sizeof(url) - 1] = '\0';
	}

	if (hop > MAX_REDIRECTS) {
		status(0, "Too many redirects (%d)", MAX_REDIRECTS);
		return 0;
	}

	if (code != 200) {
		status(0, "HTTP %lu downloading the CIA", (unsigned long)code);
		httpcCloseContext(&ctx);
		return 0;
	}

	u32 downloaded = 0, total = 0;
	httpcGetDownloadSizeState(&ctx, &downloaded, &total);

	Handle cia = 0;
	Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
	if (R_FAILED(rc)) {
		status(0, "AM_StartCiaInstall failed: 0x%08lX", (unsigned long)rc);
		httpcCloseContext(&ctx);
		return 0;
	}

	static u8 chunk[DL_CHUNK];
	u64 offset = 0;
	int ok = 1;

	for (;;) {
		u32 got = 0;
		rc = httpcDownloadData(&ctx, chunk, sizeof(chunk), &got);

		if (got > 0) {
			u32 written = 0;
			Result wrc = FSFILE_Write(cia, &written, offset, chunk, got, 0);

			if (R_FAILED(wrc)) {
				status(0, "CIA write failed: 0x%08lX", (unsigned long)wrc);
				ok = 0;
				break;
			}
			if (written != got) {
				status(0, "Short CIA write: %lu of %lu bytes",
				       (unsigned long)written, (unsigned long)got);
				ok = 0;
				break;
			}

			offset += got;
			if (total)
				status(1, "Installing... %lu%%  (%lu / %lu bytes)",
				       (unsigned long)(offset * 100 / total),
				       (unsigned long)offset, (unsigned long)total);
			else
				status(1, "Installing... %lu bytes",
				       (unsigned long)offset);
		}

		if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)
			continue; /* more data to come */

		if (R_FAILED(rc)) {
			status(0, "Download failed: 0x%08lX", (unsigned long)rc);
			ok = 0;
		}
		break;
	}

	httpcCloseContext(&ctx);

	if (!ok || offset == 0) {
		if (offset == 0 && ok)
			status(0, "Download was empty -- nothing installed");
		AM_CancelCIAInstall(cia);
		return 0;
	}

	rc = AM_FinishCiaInstall(cia);
	if (R_FAILED(rc)) {
		status(0, "AM_FinishCiaInstall failed: 0x%08lX", (unsigned long)rc);
		return 0;
	}

	return 1;
}

/* Wait for a single button out of `mask`, returning which one was pressed.
 * Used for the install confirmation -- an install is not something to start
 * off the same keypress that asked for the check. */
static u32 wait_for_keys(u32 mask)
{
	while (aptMainLoop()) {
		hidScanInput();

		const u32 down = hidKeysDown();
		if (down & mask)
			return down & mask;

		gspWaitForVBlank();
	}

	return 0;
}

/* Y-button entry point: check, confirm, install. */
static void run_update_check(void)
{
	char tag[64];

	status(0, "Checking for updates...");
	status(1, "");
	status(2, "");

	Result rc = httpcInit(0);
	if (R_FAILED(rc)) {
		status(0, "httpcInit failed: 0x%08lX", (unsigned long)rc);
		return;
	}

	rc = amInit();
	if (R_FAILED(rc)) {
		status(0, "amInit failed: 0x%08lX (am:net/am:u denied?)",
		       (unsigned long)rc);
		httpcExit();
		return;
	}

	if (!update_fetch_latest_tag(tag, sizeof(tag)))
		goto done; /* the fetch already printed why */

	if (!version_is_newer(tag, APP_VERSION)) {
		status(0, "Up to date. Running v%s, latest is %s",
		       APP_VERSION, tag);
		goto done;
	}

	status(0, "Update available: %s (running v%s)", tag, APP_VERSION);
	status(1, "A = download and install, B = cancel");

	if (!(wait_for_keys(KEY_A | KEY_B) & KEY_A)) {
		status(0, "Update cancelled.");
		status(1, "");
		goto done;
	}

	status(0, "Downloading %s ...", tag);

	if (update_download_and_install()) {
		status(0, "Installed %s. Exit and relaunch to run it.", tag);
		status(1, "");
	}
	/* On failure the installer printed the specific error already. */

done:
	amExit();
	httpcExit();
}

/* -------------------------------------------------------------- BENCHMARK */

/*
 * Sweeps the quality knobs and times a real full-screen render for each, on
 * the actual console. Every case renders to the real framebuffer through the
 * ordinary path -- nothing synthetic -- so the numbers are what the app would
 * genuinely run at with that configuration.
 */
typedef struct {
	const char *name;
	RenderConfig cfg;
	double ms;
} BenchCase;

static BenchCase g_bench[] = {
	/* name              aa  depth scale shadows */
	{ "2AA d3 full",    { 2, 3, 1, 1 }, 0.0 }, /* the shipped default */
	{ "1AA d3 full",    { 1, 3, 1, 1 }, 0.0 }, /* cost of supersampling */
	{ "1AA d2 full",    { 1, 2, 1, 1 }, 0.0 },
	{ "1AA d1 full",    { 1, 1, 1, 1 }, 0.0 },
	{ "1AA d0 full",    { 1, 0, 1, 1 }, 0.0 }, /* cost of all reflections */
	{ "1AA d1 full ns", { 1, 1, 1, 0 }, 0.0 }, /* cost of shadow rays */
	{ "1AA d3 half",    { 1, 3, 2, 1 }, 0.0 },
	{ "1AA d1 half",    { 1, 1, 2, 1 }, 0.0 },
	{ "1AA d3 qtr",     { 1, 3, 4, 1 }, 0.0 },
};

#define BENCH_COUNT ((int)(sizeof(g_bench) / sizeof(g_bench[0])))
#define BENCH_REPORT_PATH "sdmc:/raytracer3ds_bench.txt"

static int is_new_3ds(void)
{
	bool isnew = false;
	return R_SUCCEEDED(APT_CheckNew3DS(&isnew)) && isnew;
}

static void write_bench_report(void)
{
	FILE *f = fopen(BENCH_REPORT_PATH, "w");
	if (!f) {
		status(BENCH_COUNT + 2, "Could not write %s", BENCH_REPORT_PATH);
		return;
	}

	fprintf(f, "raytracer3ds v%s benchmark\n", APP_VERSION);
	fprintf(f, "console: %s\n", is_new_3ds() ? "New 3DS" : "Old 3DS");
	fprintf(f, "screen : %dx%d\n", SCREEN_W, SCREEN_H);
	fprintf(f, "target : 50.0 ms for 20 fps\n\n");
	fprintf(f, "%-16s %10s %8s %8s\n", "config", "ms", "fps", "rays");

	for (int i = 0; i < BENCH_COUNT; i++) {
		const RenderConfig *c = &g_bench[i].cfg;
		const long rays = (long)(SCREEN_W / c->scale) *
		                  (long)(SCREEN_H / c->scale) *
		                  (long)(c->aa * c->aa);

		fprintf(f, "%-16s %10.1f %8.2f %8ld\n",
		        g_bench[i].name,
		        g_bench[i].ms,
		        g_bench[i].ms > 0.0 ? 1000.0 / g_bench[i].ms : 0.0,
		        rays);
	}

	fclose(f);
	status(BENCH_COUNT + 2, "Wrote %s", BENCH_REPORT_PATH);
}

static void run_benchmark(const Camera *cam)
{
	const RenderConfig saved = g_cfg;

	printf("\x1b[13;1H\x1b[Jbenchmark -- B aborts\n");
	present();

	for (int i = 0; i < BENCH_COUNT; i++) {
		hidScanInput();
		if ((hidKeysDown() & KEY_B) || !aptMainLoop()) {
			status(0, "Benchmark aborted at case %d/%d",
			       i + 1, BENCH_COUNT);
			g_cfg = saved;
			return;
		}

		status(0, "Running %d/%d: %-16s", i + 1, BENCH_COUNT,
		       g_bench[i].name);

		g_cfg = g_bench[i].cfg;

		/* Re-fetch every case: status() presents, which swaps the back
		 * buffer, so a pointer cached across cases would be stale. */
		u16 w = 0, h = 0;
		u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);

		TickCounter tc;
		osTickCounterStart(&tc);
		render_frame(fb, cam);
		osTickCounterUpdate(&tc);

		g_bench[i].ms = osTickCounterRead(&tc);
	}

	g_cfg = saved;

	/* Results table, one row per case. */
	status(0, "%-14s %8s %6s", "config", "ms", "fps");
	for (int i = 0; i < BENCH_COUNT; i++) {
		const double ms = g_bench[i].ms;
		status(i + 1, "%-14s %8.1f %6.2f",
		       g_bench[i].name, ms, ms > 0.0 ? 1000.0 / ms : 0.0);
	}

	write_bench_report();
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
	printf("3DS software ray tracer  v%s\n", APP_VERSION);
	printf("%dx%d  %dx%d AA  %d bounces\n",
	       SCREEN_W, SCREEN_H, AA, AA, MAX_DEPTH);
	printf("console: %s\n", is_new_3ds() ? "New 3DS" : "Old 3DS");
	printf("\nSTART   exit\n");
	printf("Y       check for updates\n");
	printf("SELECT  run benchmark\n");
	printf("(hold -- input is only read\n");
	printf(" between frames)\n");

	Camera cam;
	camera_setup(&cam);

	u32 frame = 0;

	while (aptMainLoop()) {
		hidScanInput();

		/* Input is only sampled between frames, so a tap during a render
		 * is missed -- the button has to be held until the frame ends. */
		const u32 pressed = hidKeysDown();
		if (pressed & KEY_START)
			break;
		if (pressed & KEY_Y)
			run_update_check();
		if (pressed & KEY_SELECT)
			run_benchmark(&cam);

		u16 fb_w = 0, fb_h = 0;
		u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h);

		const u64 t_start = osGetTime();
		render_frame(fb, &cam);
		const u64 elapsed = osGetTime() - t_start;

		frame++;
		printf("\x1b[11;1Hframe %-6lu  %6llu ms  %5.2f fps",
		       (unsigned long)frame,
		       (unsigned long long)elapsed,
		       elapsed ? 1000.0 / (double)elapsed : 0.0);

		present();
	}

	gfxExit();
	return 0;
}
