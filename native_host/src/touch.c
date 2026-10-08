// On-screen touch controls (PDHOST_TOUCH, eg. Android): a virtual game controller. It shows up to
// the game like any other pad (SDL GameController layout), so menus, play and online matches need
// nothing special. Drawn over the game's frame just before the swap.
//
//   left half: floating move stick     right half: floating look stick
//   upper right: A B X Y diamond        lower right: Fire (RT), Aim (LT)
//   top centre: Back, Start
//
// It appears with the first touch and hides while a physical controller or the keyboard is used.
#include <math.h>
#include <string.h>
#include <SDL.h>
#include "host.h"
#include "touch.h"
#include "../../port/include/pdhost.h"

enum { B_A, B_B, B_X, B_Y, B_BACK, B_START, B_FIRE, B_AIM, NUM_BUTTONS };

static const struct {
	float x, y, r;              // centre (fraction of width / height), radius (fraction of height)
	float rgb[3];
	int sdlButton;              // SDL_GameControllerButton, or -1 for a trigger
	int sdlAxis;                // trigger axis
} g_Buttons[NUM_BUTTONS] = {
	[B_A] = { 0.880f, 0.420f, 0.060f, { 0.30f, 0.85f, 0.40f }, SDL_CONTROLLER_BUTTON_A, -1 },
	[B_B] = { 0.950f, 0.300f, 0.060f, { 0.95f, 0.35f, 0.30f }, SDL_CONTROLLER_BUTTON_B, -1 },
	[B_X] = { 0.810f, 0.300f, 0.060f, { 0.35f, 0.55f, 0.95f }, SDL_CONTROLLER_BUTTON_X, -1 },
	[B_Y] = { 0.880f, 0.180f, 0.060f, { 0.95f, 0.85f, 0.30f }, SDL_CONTROLLER_BUTTON_Y, -1 },
	[B_BACK] = { 0.440f, 0.070f, 0.040f, { 0.75f, 0.75f, 0.75f }, SDL_CONTROLLER_BUTTON_BACK, -1 },
	[B_START] = { 0.560f, 0.070f, 0.040f, { 0.75f, 0.75f, 0.75f }, SDL_CONTROLLER_BUTTON_START, -1 },
	[B_FIRE] = { 0.900f, 0.700f, 0.095f, { 0.90f, 0.20f, 0.15f }, -1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT },
	[B_AIM] = { 0.765f, 0.850f, 0.070f, { 0.95f, 0.80f, 0.20f }, -1, SDL_CONTROLLER_AXIS_TRIGGERLEFT },
};

#define STICK_RADIUS 0.11f       // full deflection, fraction of height
#define MAX_FINGERS 10

enum { ROLE_NONE, ROLE_MOVE, ROLE_LOOK, ROLE_BUTTON };

static struct {
	SDL_FingerID id;
	int role;
	int button;
	float ox, oy;               // where the stick started (pixels)
	float x, y;                 // current position (pixels)
} g_Fingers[MAX_FINGERS];

static int g_Slot = -1;          // pad id once registered
static int g_Visible;
static int g_Buttons2[SDL_CONTROLLER_BUTTON_MAX];
static int g_Axes[SDL_CONTROLLER_AXIS_MAX];

// events for the game, handed out by touchPollEvent
static int32_t g_Queue[64][8];
static int g_QHead, g_QTail;

static void queue(int32_t a, int32_t b, int32_t c, int32_t d)
{
	const int next = (g_QTail + 1) % 64;
	if (next == g_QHead) {
		return;
	}
	memset(g_Queue[g_QTail], 0, sizeof(g_Queue[0]));
	g_Queue[g_QTail][0] = a;
	g_Queue[g_QTail][1] = b;
	g_Queue[g_QTail][2] = c;
	g_Queue[g_QTail][3] = d;
	g_QTail = next;
}

int touchPollEvent(int32_t *ev)
{
	if (g_QHead == g_QTail) {
		return 0;
	}
	memcpy(ev, g_Queue[g_QHead], 8 * sizeof(*ev));
	g_QHead = (g_QHead + 1) % 64;
	return 1;
}

int touchSlot(void)
{
	return g_Slot;
}

void touchResend(void)
{
	if (g_Slot >= 0) {
		queue(PDHOST_EV_PADADDED, g_Slot, 0, 0);
	}
}

void touchHide(void)
{
	g_Visible = 0;
}

static void setButton(int b, int down)
{
	if (g_Buttons2[b] != down) {
		g_Buttons2[b] = down;
		queue(down ? PDHOST_EV_PADBUTTONDOWN : PDHOST_EV_PADBUTTONUP, g_Slot, b, 0);
	}
}

static void setAxis(int a, int v)
{
	if (v > 32767) v = 32767;
	if (v < -32768) v = -32768;
	if (g_Axes[a] != v) {
		g_Axes[a] = v;
		queue(PDHOST_EV_PADAXIS, g_Slot, a, v);
	}
}

static void pressControl(int button, int down)
{
	if (g_Buttons[button].sdlButton >= 0) {
		setButton(g_Buttons[button].sdlButton, down);
	} else {
		setAxis(g_Buttons[button].sdlAxis, down ? 32767 : 0);
	}
}

static void updateSticks(float h)
{
	int moved[2] = { 0, 0 };
	for (int i = 0; i < MAX_FINGERS; i++) {
		const int role = g_Fingers[i].role;
		if (role != ROLE_MOVE && role != ROLE_LOOK) {
			continue;
		}
		const float r = STICK_RADIUS * h;
		float dx = (g_Fingers[i].x - g_Fingers[i].ox) / r;
		float dy = (g_Fingers[i].y - g_Fingers[i].oy) / r;
		const float len = sqrtf(dx * dx + dy * dy);
		if (len > 1.f) {
			dx /= len;
			dy /= len;
		}
		const int ax = role == ROLE_MOVE ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX;
		setAxis(ax, (int)(dx * 32767.f));
		setAxis(ax + 1, (int)(dy * 32767.f));
		moved[role == ROLE_LOOK] = 1;
	}
	if (!moved[0]) {
		setAxis(SDL_CONTROLLER_AXIS_LEFTX, 0);
		setAxis(SDL_CONTROLLER_AXIS_LEFTY, 0);
	}
	if (!moved[1]) {
		setAxis(SDL_CONTROLLER_AXIS_RIGHTX, 0);
		setAxis(SDL_CONTROLLER_AXIS_RIGHTY, 0);
	}
}

void touchHandleEvent(const SDL_Event *e, int w, int h, int (*freeSlot)(void))
{
	if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERUP && e->type != SDL_FINGERMOTION) {
		return;
	}

	if (g_Slot < 0) {
		g_Slot = freeSlot();
		if (g_Slot < 0) {
			return;
		}
		queue(PDHOST_EV_PADADDED, g_Slot, 0, 0);
		hostLog("touch controls: pad %d", g_Slot);
	}
	g_Visible = 1;

	const float px = e->tfinger.x * (float)w;
	const float py = e->tfinger.y * (float)h;
	int f = -1;
	for (int i = 0; i < MAX_FINGERS; i++) {
		if (g_Fingers[i].role != ROLE_NONE && g_Fingers[i].id == e->tfinger.fingerId) {
			f = i;
			break;
		}
	}

	if (e->type == SDL_FINGERDOWN) {
		if (f < 0) {
			for (int i = 0; i < MAX_FINGERS; i++) {
				if (g_Fingers[i].role == ROLE_NONE) {
					f = i;
					break;
				}
			}
		}
		if (f < 0) {
			return;
		}
		g_Fingers[f].id = e->tfinger.fingerId;
		g_Fingers[f].ox = g_Fingers[f].x = px;
		g_Fingers[f].oy = g_Fingers[f].y = py;
		g_Fingers[f].role = e->tfinger.x < 0.45f ? ROLE_MOVE : ROLE_LOOK;
		for (int b = 0; b < NUM_BUTTONS; b++) {
			const float bx = g_Buttons[b].x * w, by = g_Buttons[b].y * h, r = g_Buttons[b].r * h * 1.15f;
			if ((px - bx) * (px - bx) + (py - by) * (py - by) <= r * r) {
				g_Fingers[f].role = ROLE_BUTTON;
				g_Fingers[f].button = b;
				pressControl(b, 1);
				break;
			}
		}
	} else if (f >= 0 && e->type == SDL_FINGERMOTION) {
		g_Fingers[f].x = px;
		g_Fingers[f].y = py;
	} else if (f >= 0 && e->type == SDL_FINGERUP) {
		if (g_Fingers[f].role == ROLE_BUTTON) {
			pressControl(g_Fingers[f].button, 0);
		}
		g_Fingers[f].role = ROLE_NONE;
	}

	updateSticks((float)h);
}

int touchPadButton(int button)
{
	return button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX ? g_Buttons2[button] : 0;
}

int touchPadAxis(int axis)
{
	return axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX ? g_Axes[axis] : 0;
}

/* ------------------------------------------------------------------------
 * drawing (GLES 3 / GL 3.3): a few translucent circles over the frame
 * ------------------------------------------------------------------------ */

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;

#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_FRAMEBUFFER 0x8D40
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA

static struct {
	int ready, failed;
	GLuint prog, vao, vbo;
	GLint uCenter, uRadius, uColor, uRing, uScreen;
	GLuint (*CreateShader)(GLenum);
	void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
	void (*CompileShader)(GLuint);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint, GLuint);
	void (*BindAttribLocation)(GLuint, GLuint, const GLchar *);
	void (*LinkProgram)(GLuint);
	GLint (*GetUniformLocation)(GLuint, const GLchar *);
	void (*UseProgram)(GLuint);
	void (*GenBuffers)(GLsizei, GLuint *);
	void (*BindBuffer)(GLenum, GLuint);
	void (*BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
	void (*GenVertexArrays)(GLsizei, GLuint *);
	void (*BindVertexArray)(GLuint);
	void (*EnableVertexAttribArray)(GLuint);
	void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
	void (*Uniform2f)(GLint, GLfloat, GLfloat);
	void (*Uniform1f)(GLint, GLfloat);
	void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
	void (*DrawArrays)(GLenum, GLint, GLsizei);
	void (*Enable)(GLenum);
	void (*Disable)(GLenum);
	GLboolean (*IsEnabled)(GLenum);
	void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
	void (*GetIntegerv)(GLenum, GLint *);
	void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
	void (*BindFramebuffer)(GLenum, GLuint);
} gl;

static const char *g_VS =
	"in vec2 aPos;\n"
	"uniform vec2 uScreen;\n"
	"void main() { gl_Position = vec4(aPos / uScreen * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *g_FS =
	"precision mediump float;\n"
	"uniform vec2 uCenter;\n"
	"uniform float uRadius;\n"
	"uniform vec4 uColor;\n"
	"uniform float uRing;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"  float d = length(gl_FragCoord.xy - uCenter);\n"
	"  float edge = 1.0 - smoothstep(uRadius - 1.5, uRadius, d);\n"
	"  float inner = uRing > 0.0 ? smoothstep(uRadius - uRing - 1.5, uRadius - uRing, d) : 1.0;\n"
	"  fragColor = vec4(uColor.rgb, uColor.a * edge * inner);\n"
	"}\n";

static int glSetup(void)
{
#define LOAD(name) if (!(*(void **)&gl.name = platGlProc("gl" #name))) return 0;
	LOAD(CreateShader) LOAD(ShaderSource) LOAD(CompileShader) LOAD(CreateProgram) LOAD(AttachShader)
	LOAD(BindAttribLocation) LOAD(LinkProgram) LOAD(GetUniformLocation) LOAD(UseProgram) LOAD(GenBuffers)
	LOAD(BindBuffer) LOAD(BufferData) LOAD(GenVertexArrays) LOAD(BindVertexArray) LOAD(EnableVertexAttribArray)
	LOAD(VertexAttribPointer) LOAD(Uniform2f) LOAD(Uniform1f) LOAD(Uniform4f) LOAD(DrawArrays) LOAD(Enable)
	LOAD(Disable) LOAD(IsEnabled) LOAD(BlendFuncSeparate) LOAD(GetIntegerv) LOAD(Viewport) LOAD(BindFramebuffer)
#undef LOAD

	// the same GLSL dialect as the game's shaders on this platform
	const char *version = platGlslVersion();
	char head[64];
	snprintf(head, sizeof(head), "#version %s\n", version ? version : "300 es");
	const char *vs[2] = { head, g_VS };
	const char *fs[2] = { head, g_FS };
	const GLuint v = gl.CreateShader(GL_VERTEX_SHADER);
	gl.ShaderSource(v, 2, vs, NULL);
	gl.CompileShader(v);
	const GLuint f = gl.CreateShader(GL_FRAGMENT_SHADER);
	gl.ShaderSource(f, 2, fs, NULL);
	gl.CompileShader(f);
	gl.prog = gl.CreateProgram();
	gl.AttachShader(gl.prog, v);
	gl.AttachShader(gl.prog, f);
	gl.BindAttribLocation(gl.prog, 0, "aPos");
	gl.LinkProgram(gl.prog);
	gl.uCenter = gl.GetUniformLocation(gl.prog, "uCenter");
	gl.uRadius = gl.GetUniformLocation(gl.prog, "uRadius");
	gl.uColor = gl.GetUniformLocation(gl.prog, "uColor");
	gl.uRing = gl.GetUniformLocation(gl.prog, "uRing");
	gl.uScreen = gl.GetUniformLocation(gl.prog, "uScreen");
	gl.GenVertexArrays(1, &gl.vao);
	gl.GenBuffers(1, &gl.vbo);
	return 1;
}

// one circle; x, y in pixels from the top left
static void circle(float x, float y, float r, float cr, float cg, float cb, float a, float ring, int h)
{
	const float yb = (float)h - y;
	const float q[8] = { x - r, yb - r, x + r, yb - r, x - r, yb + r, x + r, yb + r };
	gl.BufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STREAM_DRAW);
	gl.Uniform2f(gl.uCenter, x, yb);
	gl.Uniform1f(gl.uRadius, r);
	gl.Uniform4f(gl.uColor, cr, cg, cb, a);
	gl.Uniform1f(gl.uRing, ring);
	gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void touchDraw(int w, int h)
{
	if (!g_Visible || g_Slot < 0 || gl.failed || w <= 0 || h <= 0) {
		return;
	}
	if (!gl.ready) {
		if (!glSetup()) {
			gl.failed = 1;
			hostLog("touch controls: GL functions missing, not drawing them");
			return;
		}
		gl.ready = 1;
	}

	// keep the game's GL state
	GLint prog, vao, vbo, fbo, vp[4], bs, bd, bsa, bda;
	gl.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
	gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
	gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
	gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	gl.GetIntegerv(GL_VIEWPORT, vp);
	gl.GetIntegerv(GL_BLEND_SRC_RGB, &bs);
	gl.GetIntegerv(GL_BLEND_DST_RGB, &bd);
	gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &bsa);
	gl.GetIntegerv(GL_BLEND_DST_ALPHA, &bda);
	const GLboolean blend = gl.IsEnabled(GL_BLEND), depth = gl.IsEnabled(GL_DEPTH_TEST);
	const GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST), cull = gl.IsEnabled(GL_CULL_FACE);

	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.Viewport(0, 0, w, h);
	gl.Disable(GL_DEPTH_TEST);
	gl.Disable(GL_SCISSOR_TEST);
	gl.Disable(GL_CULL_FACE);
	gl.Enable(GL_BLEND);
	gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gl.UseProgram(gl.prog);
	gl.BindVertexArray(gl.vao);
	gl.BindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	gl.EnableVertexAttribArray(0);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
	gl.Uniform2f(gl.uScreen, (float)w, (float)h);

	for (int b = 0; b < NUM_BUTTONS; b++) {
		const float r = g_Buttons[b].r * h;
		int down = g_Buttons[b].sdlButton >= 0 ? g_Buttons2[g_Buttons[b].sdlButton] : g_Axes[g_Buttons[b].sdlAxis] > 0;
		circle(g_Buttons[b].x * w, g_Buttons[b].y * h, r, g_Buttons[b].rgb[0], g_Buttons[b].rgb[1], g_Buttons[b].rgb[2],
			down ? 0.55f : 0.28f, 0.f, h);
		circle(g_Buttons[b].x * w, g_Buttons[b].y * h, r, 1.f, 1.f, 1.f, 0.45f, 3.f, h);
	}
	for (int i = 0; i < MAX_FINGERS; i++) {
		if (g_Fingers[i].role == ROLE_MOVE || g_Fingers[i].role == ROLE_LOOK) {
			const float r = STICK_RADIUS * h;
			float kx = g_Fingers[i].x - g_Fingers[i].ox, ky = g_Fingers[i].y - g_Fingers[i].oy;
			const float len = sqrtf(kx * kx + ky * ky);
			if (len > r) {
				kx = kx / len * r;
				ky = ky / len * r;
			}
			circle(g_Fingers[i].ox, g_Fingers[i].oy, r, 1.f, 1.f, 1.f, 0.35f, 3.f, h);
			circle(g_Fingers[i].ox + kx, g_Fingers[i].oy + ky, r * 0.42f, 1.f, 1.f, 1.f, 0.35f, 0.f, h);
		}
	}

	gl.UseProgram((GLuint)prog);
	gl.BindVertexArray((GLuint)vao);
	gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)vbo);
	gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
	gl.BlendFuncSeparate((GLenum)bs, (GLenum)bd, (GLenum)bsa, (GLenum)bda);
	if (!blend) gl.Disable(GL_BLEND);
	if (depth) gl.Enable(GL_DEPTH_TEST);
	if (scissor) gl.Enable(GL_SCISSOR_TEST);
	if (cull) gl.Enable(GL_CULL_FACE);
}
