/* zv2_cursor.c -- the mouse cursor, drawn over the game's frame.
 *
 * The right stick moves a pointer (dcr_input.c); this draws it: an arrow, dark
 * outline and white fill (yellow while a tap button is held), in the engine's
 * own GLES context on the render thread just before each present
 * (port_gl_before_swap, called from eglSwapBuffers on the thread that runs
 * nativeRender). Unity keeps its own picture of the GL state and does not look
 * it up again, so everything the drawing changes is put back afterwards --
 * program, buffer, vertex attributes 0 and 1 (their whole pointer state),
 * texture unit 0, blending, depth / stencil / scissor / cull, colour mask,
 * viewport and framebuffer. The approach is abspace_nx's overlay (abs_overlay.c),
 * reduced to coloured triangles. MIT. */
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dcr_config.h"
#include "dcr_input.h"
#include "gl_layer.h"
#include "rt_window.h"
#include "util.h"

#if DCR_GL_MESA
#include <GLES2/gl2.h>

static GLuint g_prog;
static GLint u_screen, u_color;
static int g_failed;

static GLuint shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[256] = "";
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    debugPrintf("[cursor] shader: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static int init(void) {
  if (g_prog)
    return 1;
  if (g_failed)
    return 0;
  static const char *vs = "attribute vec2 aPos;\n"
                          "uniform vec2 uScreen;\n"
                          "void main() {\n"
                          "  gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);\n"
                          "}\n";
  static const char *fs = "precision mediump float;\n"
                          "uniform vec4 uColor;\n"
                          "void main() { gl_FragColor = uColor; }\n";
  GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) {
    g_failed = 1;
    return 0;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(p);
    g_failed = 1;
    debugPrintf("[cursor] the program did not link: no cursor will be drawn\n");
    return 0;
  }
  g_prog = p;
  u_screen = glGetUniformLocation(p, "uScreen");
  u_color = glGetUniformLocation(p, "uColor");
  debugPrintf("[cursor] overlay ready\n");
  return 1;
}

typedef struct {
  GLint enabled, size, type, norm, stride, buf;
  void *ptr;
} Attrib;

static void attrib_save(GLuint i, Attrib *a) {
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->enabled);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->norm);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buf);
  glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->ptr);
}

static void attrib_restore(GLuint i, const Attrib *a) {
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a->buf);
  if (a->size > 0)
    glVertexAttribPointer(i, a->size, (GLenum)a->type, a->norm ? GL_TRUE : GL_FALSE, a->stride, a->ptr);
  if (a->enabled)
    glEnableVertexAttribArray(i);
  else
    glDisableVertexAttribArray(i);
}

static void tri(const float *xy, uint32_t rgba) {
  glUniform4f(u_color, (GLfloat)((rgba >> 24) & 255) / 255.0f, (GLfloat)((rgba >> 16) & 255) / 255.0f,
              (GLfloat)((rgba >> 8) & 255) / 255.0f, (GLfloat)(rgba & 255) / 255.0f);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, xy);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void draw_arrow(float x, float y, float s, int pressed) {
  /* a classic pointer: the tip at (x, y), seven points, fanned from the tip */
  static const float arrow[][2] = {{0, 0}, {0, 16}, {4, 12}, {7, 18}, {10, 16.5f}, {7, 10.5f}, {12, 10}};
  for (int pass = 0; pass < 2; pass++) {
    const float sc = pass ? s : s * 1.25f;
    const float ox = pass ? 0 : -1.5f * s, oy = pass ? 0 : -1.5f * s;
    const uint32_t col = pass ? (pressed ? 0xffe63cffu : 0xffffffffu) : 0x000000d8u;
    for (int i = 1; i + 1 < 7; i++) {
      const float t[6] = {x + ox + arrow[0][0] * sc, y + oy + arrow[0][1] * sc,
                          x + ox + arrow[i][0] * sc, y + oy + arrow[i][1] * sc,
                          x + ox + arrow[i + 1][0] * sc, y + oy + arrow[i + 1][1] * sc};
      tri(t, col);
    }
  }
}

void port_gl_before_swap(void) {
  const DcrConfig *c = dcr_config();
  if (!c->mouse || !c->mouse_cursor)
    return;
  float x, y;
  int visible, pressed;
  dcr_input_cursor(&x, &y, &visible, &pressed);
  if (!visible || !init())
    return;
  int w, h;
  dcr_window_size(&w, &h);

  Attrib a0, a1;
  GLint prog, abuf, fb, active, tex0, vp[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
  GLboolean mask[4];
  attrib_save(0, &a0);
  attrib_save(1, &a1);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beq_a);
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
                  cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST),
                  stencil = glIsEnabled(GL_STENCIL_TEST);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(g_prog);
  glUniform2f(u_screen, (GLfloat)w, (GLfloat)h);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(0);
  glDisableVertexAttribArray(1);

  draw_arrow(x, y, 1.6f * (float)h / 720.0f * c->mouse_size, pressed);

  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
  attrib_restore(0, &a0);
  attrib_restore(1, &a1);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
  glUseProgram((GLuint)prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
  glActiveTexture((GLenum)active);
  glBlendEquationSeparate((GLenum)beq_rgb, (GLenum)beq_a);
  glBlendFuncSeparate((GLenum)bsrc_rgb, (GLenum)bdst_rgb, (GLenum)bsrc_a, (GLenum)bdst_a);
  glColorMask(mask[0], mask[1], mask[2], mask[3]);
  if (!blend) glDisable(GL_BLEND);
  if (depth) glEnable(GL_DEPTH_TEST);
  if (cull) glEnable(GL_CULL_FACE);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (stencil) glEnable(GL_STENCIL_TEST);
}
#endif /* DCR_GL_MESA: with the null renderer there is nothing to draw on */
