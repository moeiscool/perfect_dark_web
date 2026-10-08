// The GL functions pd.wasm imports (the ones port/src/web_sdl.c gives the renderer), forwarded to
// the native GL. Addresses in the game's memory become native pointers; strings the game reads
// back (glGetString) are copied into its heap. The game renders through OpenGL ES 3.0 (as in a
// browser), and the native context is desktop GL 3.3 core (or whatever platGlslVersion says), so
// shaders get their #version line rewritten.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NUM_EXTENSIONS 0x821D

#define GLFUNCS(X) \
	X(void, ActiveTexture, (GLenum)) \
	X(void, AttachShader, (GLuint, GLuint)) \
	X(void, BindBuffer, (GLenum, GLuint)) \
	X(void, BindFramebuffer, (GLenum, GLuint)) \
	X(void, BindRenderbuffer, (GLenum, GLuint)) \
	X(void, BindTexture, (GLenum, GLuint)) \
	X(void, BindVertexArray, (GLuint)) \
	X(void, BlendFunc, (GLenum, GLenum)) \
	X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
	X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
	X(GLenum, CheckFramebufferStatus, (GLenum)) \
	X(void, Clear, (GLbitfield)) \
	X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, CompileShader, (GLuint)) \
	X(GLuint, CreateProgram, (void)) \
	X(GLuint, CreateShader, (GLenum)) \
	X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
	X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
	X(void, DeleteProgram, (GLuint)) \
	X(void, DeleteRenderbuffers, (GLsizei, const GLuint *)) \
	X(void, DeleteShader, (GLuint)) \
	X(void, DeleteTextures, (GLsizei, const GLuint *)) \
	X(void, DeleteVertexArrays, (GLsizei, const GLuint *)) \
	X(void, DepthFunc, (GLenum)) \
	X(void, DepthMask, (GLboolean)) \
	X(void, DepthRange, (GLdouble, GLdouble)) \
	X(void, DepthRangef, (GLfloat, GLfloat)) \
	X(void, DetachShader, (GLuint, GLuint)) \
	X(void, Disable, (GLenum)) \
	X(void, DisableVertexAttribArray, (GLuint)) \
	X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
	X(void, Enable, (GLenum)) \
	X(void, EnableVertexAttribArray, (GLuint)) \
	X(void, Finish, (void)) \
	X(void, Flush, (void)) \
	X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
	X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
	X(void, GenBuffers, (GLsizei, GLuint *)) \
	X(void, GenFramebuffers, (GLsizei, GLuint *)) \
	X(void, GenRenderbuffers, (GLsizei, GLuint *)) \
	X(void, GenTextures, (GLsizei, GLuint *)) \
	X(void, GenVertexArrays, (GLsizei, GLuint *)) \
	X(void, GenerateMipmap, (GLenum)) \
	X(GLint, GetAttribLocation, (GLuint, const GLchar *)) \
	X(GLenum, GetError, (void)) \
	X(void, GetFloatv, (GLenum, GLfloat *)) \
	X(void, GetIntegerv, (GLenum, GLint *)) \
	X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
	X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
	X(const unsigned char *, GetString, (GLenum)) \
	X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
	X(void, LinkProgram, (GLuint)) \
	X(void, PixelStorei, (GLenum, GLint)) \
	X(void, PolygonOffset, (GLfloat, GLfloat)) \
	X(void, ReadBuffer, (GLenum)) \
	X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
	X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
	X(void, RenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
	X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	X(void, TexParameterf, (GLenum, GLenum, GLfloat)) \
	X(void, TexParameteri, (GLenum, GLenum, GLint)) \
	X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
	X(void, Uniform1f, (GLint, GLfloat)) \
	X(void, Uniform1i, (GLint, GLint)) \
	X(void, UseProgram, (GLuint)) \
	X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
	X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))

#define DECLARE(ret, name, args) static ret (*p##name) args;
GLFUNCS(DECLARE)
#undef DECLARE

// logs GL errors (a few, then every 600th), so rendering problems show up in the log
void glReportErrors(const char *where)
{
	static unsigned count;
	GLenum err;
	while (pGetError && (err = pGetError()) != 0) {
		if (count < 20 || count % 600 == 0) {
			hostLog("GL error 0x%04x %s", err, where);
		}
		count++;
	}
}

int glLoadFunctions(void)
{
#define LOAD(ret, name, args) p##name = (ret (*) args)platGlProc("gl" #name);
	GLFUNCS(LOAD)
#undef LOAD
	if (!pGetString || !pClear || !pDrawArrays || !pShaderSource) {
		hostLog("GL: required functions are missing");
		return 0;
	}
	hostLog("GL: %s, %s", (const char *)pGetString(GL_RENDERER), (const char *)pGetString(GL_VERSION));
	return 1;
}

#define I(x) ((GLint)(int32_t)(x))
#define F(x) (x)

void w2c_env_glActiveTexture(struct w2c_env *e, u32 a) { pActiveTexture(a); }
void w2c_env_glAttachShader(struct w2c_env *e, u32 a, u32 b) { pAttachShader(a, b); }
void w2c_env_glBindBuffer(struct w2c_env *e, u32 a, u32 b) { pBindBuffer(a, b); }
void w2c_env_glBindFramebuffer(struct w2c_env *e, u32 a, u32 b) { pBindFramebuffer(a, b); }
void w2c_env_glBindRenderbuffer(struct w2c_env *e, u32 a, u32 b) { pBindRenderbuffer(a, b); }
void w2c_env_glBindTexture(struct w2c_env *e, u32 a, u32 b) { pBindTexture(a, b); }
void w2c_env_glBindVertexArray(struct w2c_env *e, u32 a) { pBindVertexArray(a); }
void w2c_env_glBlendFunc(struct w2c_env *e, u32 a, u32 b) { pBlendFunc(a, b); }
void w2c_env_glBlitFramebuffer(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d, u32 f, u32 g, u32 h, u32 i, u32 mask, u32 filter)
{
	glReportErrors("before glBlitFramebuffer");
	pBlitFramebuffer(I(a), I(b), I(c), I(d), I(f), I(g), I(h), I(i), mask, filter);
	glReportErrors("in glBlitFramebuffer");
}
void w2c_env_glBufferData(struct w2c_env *e, u32 target, u32 size, u32 data, u32 usage) { pBufferData(target, (GLsizeiptr)size, wptr(data), usage); }
u32 w2c_env_glCheckFramebufferStatus(struct w2c_env *e, u32 a)
{
	const u32 status = pCheckFramebufferStatus(a);
	if (status != 0x8CD5) { // GL_FRAMEBUFFER_COMPLETE
		hostLog("GL: framebuffer incomplete: 0x%04x", status);
	}
	return status;
}
void w2c_env_glClear(struct w2c_env *e, u32 a) { pClear(a); }
void w2c_env_glClearColor(struct w2c_env *e, f32 r, f32 g, f32 b, f32 a) { pClearColor(r, g, b, a); }
void w2c_env_glCompileShader(struct w2c_env *e, u32 a) { pCompileShader(a); }
u32 w2c_env_glCreateProgram(struct w2c_env *e) { return pCreateProgram(); }
u32 w2c_env_glCreateShader(struct w2c_env *e, u32 a) { return pCreateShader(a); }
void w2c_env_glDeleteBuffers(struct w2c_env *e, u32 n, u32 p) { pDeleteBuffers(I(n), wptr(p)); }
void w2c_env_glDeleteFramebuffers(struct w2c_env *e, u32 n, u32 p) { pDeleteFramebuffers(I(n), wptr(p)); }
void w2c_env_glDeleteProgram(struct w2c_env *e, u32 a) { pDeleteProgram(a); }
void w2c_env_glDeleteRenderbuffers(struct w2c_env *e, u32 n, u32 p) { pDeleteRenderbuffers(I(n), wptr(p)); }
void w2c_env_glDeleteShader(struct w2c_env *e, u32 a) { pDeleteShader(a); }
void w2c_env_glDeleteTextures(struct w2c_env *e, u32 n, u32 p) { pDeleteTextures(I(n), wptr(p)); }
void w2c_env_glDeleteVertexArrays(struct w2c_env *e, u32 n, u32 p) { pDeleteVertexArrays(I(n), wptr(p)); }
void w2c_env_glDepthFunc(struct w2c_env *e, u32 a) { pDepthFunc(a); }
void w2c_env_glDepthMask(struct w2c_env *e, u32 a) { pDepthMask((GLboolean)a); }
void w2c_env_glDepthRangef(struct w2c_env *e, f32 n, f32 f)
{
	// glDepthRangef is GL 4.1 / ES; GL 3.3 has the double version
	if (pDepthRangef) {
		pDepthRangef(n, f);
	} else if (pDepthRange) {
		pDepthRange(n, f);
	}
}
void w2c_env_glDetachShader(struct w2c_env *e, u32 a, u32 b) { pDetachShader(a, b); }
void w2c_env_glDisable(struct w2c_env *e, u32 a) { pDisable(a); }
void w2c_env_glDisableVertexAttribArray(struct w2c_env *e, u32 a) { pDisableVertexAttribArray(a); }
void w2c_env_glDrawArrays(struct w2c_env *e, u32 mode, u32 first, u32 count) { pDrawArrays(mode, I(first), I(count)); }
void w2c_env_glEnable(struct w2c_env *e, u32 a) { pEnable(a); }
void w2c_env_glEnableVertexAttribArray(struct w2c_env *e, u32 a) { pEnableVertexAttribArray(a); }
void w2c_env_glFinish(struct w2c_env *e) { pFinish(); }
void w2c_env_glFlush(struct w2c_env *e) { pFlush(); }
void w2c_env_glFramebufferRenderbuffer(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d) { pFramebufferRenderbuffer(a, b, c, d); }
void w2c_env_glFramebufferTexture2D(struct w2c_env *e, u32 a, u32 b, u32 c, u32 d, u32 l) { pFramebufferTexture2D(a, b, c, d, I(l)); }
void w2c_env_glGenBuffers(struct w2c_env *e, u32 n, u32 p) { pGenBuffers(I(n), wptr(p)); }
void w2c_env_glGenFramebuffers(struct w2c_env *e, u32 n, u32 p) { pGenFramebuffers(I(n), wptr(p)); }
void w2c_env_glGenRenderbuffers(struct w2c_env *e, u32 n, u32 p) { pGenRenderbuffers(I(n), wptr(p)); }
void w2c_env_glGenTextures(struct w2c_env *e, u32 n, u32 p) { pGenTextures(I(n), wptr(p)); }
void w2c_env_glGenVertexArrays(struct w2c_env *e, u32 n, u32 p) { pGenVertexArrays(I(n), wptr(p)); }
void w2c_env_glGenerateMipmap(struct w2c_env *e, u32 a) { pGenerateMipmap(a); }
u32 w2c_env_glGetAttribLocation(struct w2c_env *e, u32 prog, u32 name) { return (u32)pGetAttribLocation(prog, wptr(name)); }
u32 w2c_env_glGetError(struct w2c_env *e) { return pGetError(); }
void w2c_env_glGetFloatv(struct w2c_env *e, u32 pname, u32 p) { pGetFloatv(pname, wptr(p)); }
void w2c_env_glGetIntegerv(struct w2c_env *e, u32 pname, u32 p)
{
	if (pname == GL_NUM_EXTENSIONS) {
		// the game's GL loader (glad) gives up when there are none; report one it doesn't know, so
		// the game uses the plain WebGL 2 / ES 3.0 feature set as in a browser
		wstore32(p, 1);
		return;
	}
	pGetIntegerv(pname, wptr(p));
}
void w2c_env_glGetProgramInfoLog(struct w2c_env *e, u32 prog, u32 size, u32 len, u32 log) { pGetProgramInfoLog(prog, I(size), wptr(len), wptr(log)); }
void w2c_env_glGetProgramiv(struct w2c_env *e, u32 prog, u32 pname, u32 p) { pGetProgramiv(prog, pname, wptr(p)); }
void w2c_env_glGetShaderInfoLog(struct w2c_env *e, u32 sh, u32 size, u32 len, u32 log) { pGetShaderInfoLog(sh, I(size), wptr(len), wptr(log)); }
void w2c_env_glGetShaderiv(struct w2c_env *e, u32 sh, u32 pname, u32 p) { pGetShaderiv(sh, pname, wptr(p)); }

u32 w2c_env_glGetString(struct w2c_env *e, u32 name)
{
	// the game takes the ES 3.0 path, as in the browser
	static uint32_t cache[5];
	int idx;
	char text[256];
	switch (name) {
	case GL_VENDOR: idx = 0; snprintf(text, sizeof(text), "%s", (const char *)pGetString(GL_VENDOR)); break;
	case GL_RENDERER: idx = 1; snprintf(text, sizeof(text), "%s", (const char *)pGetString(GL_RENDERER)); break;
	case GL_VERSION: idx = 2; snprintf(text, sizeof(text), "OpenGL ES 3.0 (native %s)", (const char *)pGetString(GL_VERSION)); break;
	case GL_SHADING_LANGUAGE_VERSION: idx = 3; snprintf(text, sizeof(text), "OpenGL ES GLSL ES 3.00"); break;
	case GL_EXTENSIONS: idx = 4; text[0] = '\0'; break;
	default: return 0;
	}
	if (!cache[idx]) {
		cache[idx] = hostNewString(text);
	}
	return cache[idx];
}

u32 w2c_env_glGetStringi(struct w2c_env *e, u32 name, u32 index)
{
	static uint32_t ext;
	if (name != GL_EXTENSIONS || index != 0) {
		return 0;
	}
	if (!ext) {
		ext = hostNewString("GL_PDHOST_native");
	}
	return ext;
}

u32 w2c_env_glGetUniformLocation(struct w2c_env *e, u32 prog, u32 name) { return (u32)pGetUniformLocation(prog, wptr(name)); }
void w2c_env_glLinkProgram(struct w2c_env *e, u32 a) { pLinkProgram(a); }
void w2c_env_glPixelStorei(struct w2c_env *e, u32 a, u32 b) { pPixelStorei(a, I(b)); }
void w2c_env_glPolygonOffset(struct w2c_env *e, f32 a, f32 b) { pPolygonOffset(a, b); }
void w2c_env_glReadBuffer(struct w2c_env *e, u32 a) { pReadBuffer(a); }
void w2c_env_glReadPixels(struct w2c_env *e, u32 x, u32 y, u32 w, u32 h, u32 fmt, u32 type, u32 p) { pReadPixels(I(x), I(y), I(w), I(h), fmt, type, wptr(p)); }
void w2c_env_glRenderbufferStorage(struct w2c_env *e, u32 a, u32 b, u32 w, u32 h) { pRenderbufferStorage(a, b, I(w), I(h)); }
void w2c_env_glRenderbufferStorageMultisample(struct w2c_env *e, u32 a, u32 s, u32 b, u32 w, u32 h) { pRenderbufferStorageMultisample(a, I(s), b, I(w), I(h)); }
void w2c_env_glScissor(struct w2c_env *e, u32 x, u32 y, u32 w, u32 h) { pScissor(I(x), I(y), I(w), I(h)); }

void w2c_env_glShaderSource(struct w2c_env *e, u32 shader, u32 count, u32 strings, u32 lengths)
{
	// join the parts, then make the GLSL ES 3.00 source fit the native GL
	size_t total = 0;
	for (u32 i = 0; i < count; i++) {
		const uint32_t s = wload32(strings + i * 4);
		const int32_t len = lengths ? (int32_t)wload32(lengths + i * 4) : -1;
		total += len >= 0 ? (size_t)len : strlen((const char *)wptr(s));
	}

	char *src = malloc(total + 64);
	size_t pos = 0;
	for (u32 i = 0; i < count; i++) {
		const uint32_t s = wload32(strings + i * 4);
		const int32_t len = lengths ? (int32_t)wload32(lengths + i * 4) : -1;
		const size_t n = len >= 0 ? (size_t)len : strlen((const char *)wptr(s));
		memcpy(src + pos, wptr(s), n);
		pos += n;
	}
	src[pos] = '\0';

	const char *es = "#version 300 es";
	const char *native = platGlslVersion();
	char *out = src;
	if (native && !strncmp(src, es, strlen(es))) {
		out = malloc(pos + 64);
		snprintf(out, pos + 64, "#version %s%s", native, src + strlen(es));
		free(src);
	}

	const GLchar *parts[1] = { out };
	pShaderSource(shader, 1, parts, NULL);
	free(out);
}

void w2c_env_glTexImage2D(struct w2c_env *e, u32 target, u32 level, u32 ifmt, u32 w, u32 h, u32 border, u32 fmt, u32 type, u32 p)
{
	pTexImage2D(target, I(level), I(ifmt), I(w), I(h), I(border), fmt, type, wptr(p));
}
void w2c_env_glTexParameterf(struct w2c_env *e, u32 a, u32 b, f32 v) { pTexParameterf(a, b, v); }
void w2c_env_glTexParameteri(struct w2c_env *e, u32 a, u32 b, u32 v) { pTexParameteri(a, b, I(v)); }
void w2c_env_glTexSubImage2D(struct w2c_env *e, u32 target, u32 level, u32 x, u32 y, u32 w, u32 h, u32 fmt, u32 type, u32 p)
{
	pTexSubImage2D(target, I(level), I(x), I(y), I(w), I(h), fmt, type, wptr(p));
}
void w2c_env_glUniform1f(struct w2c_env *e, u32 loc, f32 v) { pUniform1f(I(loc), v); }
void w2c_env_glUniform1i(struct w2c_env *e, u32 loc, u32 v) { pUniform1i(I(loc), I(v)); }
void w2c_env_glUseProgram(struct w2c_env *e, u32 a) { pUseProgram(a); }
void w2c_env_glVertexAttribPointer(struct w2c_env *e, u32 idx, u32 size, u32 type, u32 norm, u32 stride, u32 offset)
{
	// always a buffer offset (WebGL 2 has no client-side arrays)
	pVertexAttribPointer(idx, I(size), type, (GLboolean)norm, I(stride), (const void *)(uintptr_t)offset);
}
void w2c_env_glViewport(struct w2c_env *e, u32 x, u32 y, u32 w, u32 h) { pViewport(I(x), I(y), I(w), I(h)); }
