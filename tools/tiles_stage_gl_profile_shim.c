// LD_PRELOAD instrumentation for the private compositor only. Never installed.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <GLES3/gl3.h>
typedef struct { uint64_t uniform_lookups, integer_reads, texture_allocations, texture_pixels, framebuffer_allocations, clears, clear_pixels, draws, shader_links; } Counters;
static Counters counters;
static GLint viewport[4],scissor[4];
static int scissoring;
void stage_profile_gl_reset(void) { memset(&counters,0,sizeof(counters)); }
void stage_profile_gl_snapshot(Counters* out) { *out=counters; }
#define LOAD(name) static __typeof__(&name) real; if (!real) real=dlsym(RTLD_NEXT,#name)
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h) { LOAD(glViewport); viewport[0]=x;viewport[1]=y;viewport[2]=w;viewport[3]=h;real(x,y,w,h); }
void glScissor(GLint x,GLint y,GLsizei w,GLsizei h) { LOAD(glScissor);scissor[0]=x;scissor[1]=y;scissor[2]=w;scissor[3]=h;real(x,y,w,h); }
void glEnable(GLenum cap) { LOAD(glEnable);if(cap==GL_SCISSOR_TEST)scissoring=1;real(cap); }
void glDisable(GLenum cap) { LOAD(glDisable);if(cap==GL_SCISSOR_TEST)scissoring=0;real(cap); }
void glGetIntegerv(GLenum key,GLint* value) { LOAD(glGetIntegerv);++counters.integer_reads;real(key,value); }
GLint glGetUniformLocation(GLuint program,const GLchar* name) { LOAD(glGetUniformLocation);++counters.uniform_lookups;return real(program,name); }
void glGenFramebuffers(GLsizei n,GLuint* fbs) { LOAD(glGenFramebuffers);counters.framebuffer_allocations+=n;real(n,fbs); }
void glTexImage2D(GLenum target,GLint level,GLint format,GLsizei w,GLsizei h,GLint border,GLenum type,GLenum dataType,const void* data) { LOAD(glTexImage2D);++counters.texture_allocations;counters.texture_pixels+=(uint64_t)w*h;real(target,level,format,w,h,border,type,dataType,data); }
void glTexStorage2D(GLenum target,GLsizei levels,GLenum format,GLsizei w,GLsizei h) { LOAD(glTexStorage2D);++counters.texture_allocations;counters.texture_pixels+=(uint64_t)w*h;real(target,levels,format,w,h); }
void glClearBufferfv(GLenum type,GLint index,const GLfloat* value) { LOAD(glClearBufferfv);++counters.clears;const GLint* r=scissoring?scissor:viewport;counters.clear_pixels+=(uint64_t)r[2]*r[3];real(type,index,value); }
void glDrawArrays(GLenum mode,GLint first,GLsizei count) { LOAD(glDrawArrays);++counters.draws;real(mode,first,count); }
void glLinkProgram(GLuint program) { LOAD(glLinkProgram);++counters.shader_links;real(program); }
