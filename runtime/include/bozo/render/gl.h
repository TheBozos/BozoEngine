// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The OpenGL 3.3 core functions the renderer's present side uses, loaded at run time through the platform layer's
// loader (SDL_GL_GetProcAddress): a small table of our own instead of a generated loader (phase4-inventory.md section
// 5, renderer backend). Types and constants are the OpenGL 3.3 core specification's.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(_WIN64)
#define BOZO_GL_CALL __stdcall
#else
#define BOZO_GL_CALL
#endif

namespace bozo::render::gl
{
    using GLenum = unsigned int;
    using GLuint = unsigned int;
    using GLint = int;
    using GLsizei = int;
    using GLboolean = unsigned char;
    using GLbitfield = unsigned int;
    using GLfloat = float;
    using GLdouble = double;
    using GLchar = char;
    using GLubyte = unsigned char;
    using GLintptr = std::ptrdiff_t;
    using GLsizeiptr = std::ptrdiff_t;

    // ---- Constants (OpenGL 3.3 core) ----
    constexpr GLenum kFalse = 0, kTrue = 1;
    constexpr GLenum kTriangles = 0x0004;
    constexpr GLenum kUnsignedByte = 0x1401, kUnsignedInt = 0x1405, kFloat = 0x1406;
    constexpr GLenum kZero = 0, kOne = 1, kSrcAlpha = 0x0302, kOneMinusSrcAlpha = 0x0303;
    constexpr GLenum kFuncAdd = 0x8006;
    constexpr GLenum kCullFace = 0x0B44, kDepthTest = 0x0B71, kBlend = 0x0BE2, kScissorTest = 0x0C11;
    constexpr GLenum kLequal = 0x0203;
    constexpr GLbitfield kDepthBufferBit = 0x0100, kColorBufferBit = 0x4000;
    constexpr GLenum kTexture2D = 0x0DE1, kTexture0 = 0x84C0;
    constexpr GLenum kTextureMagFilter = 0x2800, kTextureMinFilter = 0x2801, kTextureWrapS = 0x2802, kTextureWrapT = 0x2803;
    constexpr GLenum kNearest = 0x2600, kLinear = 0x2601, kRepeat = 0x2901, kClampToEdge = 0x812F;
    constexpr GLenum kRgb = 0x1907, kRgba = 0x1908, kRgba8 = 0x8058;
    constexpr GLenum kDepthComponent = 0x1902, kDepthComponent24 = 0x81A6;
    constexpr GLenum kArrayBuffer = 0x8892, kPixelPackBuffer = 0x88EB;
    constexpr GLenum kStreamDraw = 0x88E0, kStreamRead = 0x88E1, kDynamicDraw = 0x88E8;
    constexpr GLbitfield kMapReadBit = 0x0001;
    constexpr GLenum kFramebuffer = 0x8D40, kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9, kRenderbuffer = 0x8D41;
    constexpr GLenum kColorAttachment0 = 0x8CE0, kDepthAttachment = 0x8D00, kFramebufferComplete = 0x8CD5;
    constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30;
    constexpr GLenum kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kInfoLogLength = 0x8B84;
    constexpr GLenum kPackAlignment = 0x0D05, kUnpackAlignment = 0x0CF5;
    constexpr GLenum kFront = 0x0404, kBack = 0x0405;
    constexpr GLenum kVendor = 0x1F00, kRenderer = 0x1F01, kVersion = 0x1F02;
    constexpr GLenum kViewport = 0x0BA2;

#define BOZO_GL_FUNCTIONS(X)                                                                                           \
    X(GLenum, GetError, (void))                                                                                        \
    X(const GLubyte *, GetString, (GLenum name))                                                                       \
    X(void, Viewport, (GLint x, GLint y, GLsizei width, GLsizei height))                                               \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                                  \
    X(void, ClearDepth, (GLdouble depth))                                                                              \
    X(void, Clear, (GLbitfield mask))                                                                                  \
    X(void, Enable, (GLenum cap))                                                                                      \
    X(void, Disable, (GLenum cap))                                                                                     \
    X(void, BlendFunc, (GLenum src, GLenum dst))                                                                       \
    X(void, BlendFuncSeparate, (GLenum srcRgb, GLenum dstRgb, GLenum srcAlpha, GLenum dstAlpha))                       \
    X(void, BlendEquation, (GLenum mode))                                                                              \
    X(void, DepthFunc, (GLenum func))                                                                                  \
    X(void, DepthMask, (GLboolean flag))                                                                               \
    X(void, Scissor, (GLint x, GLint y, GLsizei width, GLsizei height))                                                \
    X(void, PixelStorei, (GLenum name, GLint param))                                                                   \
    X(void, ReadBuffer, (GLenum mode))                                                                                 \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels))    \
    X(void, GetIntegerv, (GLenum name, GLint *data))                                                                   \
    X(void, Finish, (void))                                                                                          \
    X(GLuint, CreateShader, (GLenum type))                                                                             \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths))          \
    X(void, CompileShader, (GLuint shader))                                                                            \
    X(void, GetShaderiv, (GLuint shader, GLenum name, GLint *value))                                                   \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei *length, GLchar *log))                             \
    X(void, DeleteShader, (GLuint shader))                                                                             \
    X(GLuint, CreateProgram, (void))                                                                                   \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                             \
    X(void, LinkProgram, (GLuint program))                                                                             \
    X(void, GetProgramiv, (GLuint program, GLenum name, GLint *value))                                                 \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei *length, GLchar *log))                           \
    X(void, DeleteProgram, (GLuint program))                                                                           \
    X(void, UseProgram, (GLuint program))                                                                              \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar *name))                                                 \
    X(void, Uniform1i, (GLint location, GLint v0))                                                                     \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                                                   \
    X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))                                                       \
    X(void, Uniform3fv, (GLint location, GLsizei count, const GLfloat *v))                                             \
    X(void, Uniform4fv, (GLint location, GLsizei count, const GLfloat *v))                                             \
    X(void, GenVertexArrays, (GLsizei n, GLuint *arrays))                                                              \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint *arrays))                                                     \
    X(void, BindVertexArray, (GLuint array))                                                                           \
    X(void, GenBuffers, (GLsizei n, GLuint *buffers))                                                                  \
    X(void, DeleteBuffers, (GLsizei n, const GLuint *buffers))                                                         \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                                \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))                              \
    X(void, BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data))                        \
    X(void *, MapBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access))                  \
    X(GLboolean, UnmapBuffer, (GLenum target))                                                                         \
    X(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,         \
                                  const void *pointer))                                                                \
    X(void, EnableVertexAttribArray, (GLuint index))                                                                   \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                                     \
    X(void, GenTextures, (GLsizei n, GLuint *textures))                                                                \
    X(void, DeleteTextures, (GLsizei n, const GLuint *textures))                                                       \
    X(void, BindTexture, (GLenum target, GLuint texture))                                                              \
    X(void, ActiveTexture, (GLenum texture))                                                                           \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, \
                         GLenum format, GLenum type, const void *pixels))                                              \
    X(void, TexParameteri, (GLenum target, GLenum name, GLint param))                                                  \
    X(void, CopyTexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,            \
                                GLsizei width, GLsizei height))                                                        \
    X(void, GenFramebuffers, (GLsizei n, GLuint *framebuffers))                                                        \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))                                               \
    X(void, BindFramebuffer, (GLenum target, GLuint framebuffer))                                                      \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))   \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint renderbuffer))         \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                                 \
    X(void, GenRenderbuffers, (GLsizei n, GLuint *renderbuffers))                                                      \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers))                                             \
    X(void, BindRenderbuffer, (GLenum target, GLuint renderbuffer))                                                    \
    X(void, RenderbufferStorage, (GLenum target, GLenum internalFormat, GLsizei width, GLsizei height))

    struct Api
    {
#define BOZO_GL_MEMBER(ret, name, args) ret(BOZO_GL_CALL *name) args = nullptr;
        BOZO_GL_FUNCTIONS(BOZO_GL_MEMBER)
#undef BOZO_GL_MEMBER
    };

    // The table, filled by load(); every member null before.
    const Api &api();
    // Loads every function through `getProc` (the current context's: call it with a context current). False with the
    // first missing function's name in `missing`.
    using ProcLoader = void *(*)(const char *name);
    bool load(ProcLoader getProc, const char **missing = nullptr);
    bool loaded();
}
