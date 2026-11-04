// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The native renderer's present side (bozo/render/renderer.h): the GPU half of the fork's native_renderer.cpp (its
// struct Gpu, present(), the overlay's drawing, the depth tiles), moved from the team's code (phase4-inventory.md
// section 2.1) with raylib's rlgl and render-texture calls replaced by plain OpenGL 3.3 core (bozo/render/gl.h). The
// version 330 shaders are the fork's; what raylib drew with its own shader and immediate-mode quads (the 2D, the movie
// pictures, the frame's target onto the window) is drawn here with a small shader of the same arithmetic (texel times
// the vertex colour) and the same quad split (a, b, e / a, e, c), under raylib's blend modes (alpha: SRC_ALPHA,
// ONE_MINUS_SRC_ALPHA; additive: SRC_ALPHA, ONE; premultiplied: ONE, ONE_MINUS_SRC_ALPHA).
//
// Not moved (the GS front end's and the interpolation's, which BozoEngine does not have yet): the GS-stream groups and
// their clips, frames drawn between the game's (the Video settings' frame rate), the texture cache's lookups.
// Written fresh for BozoEngine: the depth readback (a pixel-pack buffer per present, mapped one present later: the
// fork read it in its GS-side file) and Material::sampleTarget (the frame drawn so far copied, then sampled).
#include "bozo/render/gl.h"
#include "bozo/render/movie.h"
#include "bozo/render/renderer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace bozo::render
{
    namespace
    {
        // The object's matrix takes a vertex to PS2 screen space (x, y in GS window coordinates, z GS depth, w); the
        // PS2 divides by w. Here the same point goes to the window: NDC from the screen rectangle, depth from the Z
        // range (larger is nearer on the GS), and w kept so texture coordinates interpolate in perspective and the
        // GPU clips what lies behind the camera (w <= 0: where the PS2 relies on the microprogram's clipping).
        // uUvMap: texture coordinates as given (1, 0, 1, 0), or a sampleTarget batch's GS pixels to the target copy.
        const char *kVertexShader = R"(#version 330
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
layout(location = 3) in float aFog;
uniform vec4 uM[4];
uniform vec4 uScreen;
uniform float uDepthMax;
uniform vec4 uUvMap;
out vec2 vUv;
out vec4 vCol;
out float vFog;
void main()
{
    vec4 c = uM[0] * aPos.x + uM[1] * aPos.y + uM[2] * aPos.z + uM[3];
    float w = c.w;
    vec2 s = c.xy / c.w;
    vec2 ndc = vec2((s.x - uScreen.x) / uScreen.z * 2.0 - 1.0, 1.0 - (s.y - uScreen.y) / uScreen.w * 2.0);
    // GS Z as XYZF2 holds it: past the Z buffer's range it wraps (24 bits kept), as on the GS, rather than clamps.
    float gz = c.z / c.w;
    gz -= (uDepthMax + 1.0) * floor(gz / (uDepthMax + 1.0));
    float z = clamp(gz / uDepthMax, 0.0, 1.0);
    gl_Position = vec4(ndc * w, (1.0 - 2.0 * z) * w, w);
    vUv = vec2(aUv.x * uUvMap.x + uUvMap.y, aUv.y * uUvMap.z + uUvMap.w);
    vCol = aCol;
    vFog = aFog;
}
)";

        // GS modulate: texel * vertex colour, where a vertex colour of 0x80 is 1.0, then fog toward the fog colour (RGB
        // only, before blending). The render target holds premultiplied colour and coverage (opaque draws cover
        // fully), so present() composites it over the window. uTarget: the texture is the target's copy, its alpha
        // taken as the GS's 0x80 (1.0).
        const char *kFragmentShader = R"(#version 330
in vec2 vUv;
in vec4 vCol;
in float vFog;
uniform sampler2D uTex;
uniform int uTextured;
uniform int uTarget;
uniform int uAlphaTest;
uniform float uAlphaRef;
uniform int uOpaque;
uniform int uFog;
uniform vec3 uFogColour;
out vec4 fragColor;
void main()
{
    vec4 colour = vCol * (255.0 / 128.0);
    vec4 texel = uTextured != 0 ? texture(uTex, vUv) * vec4(1.0, 1.0, 1.0, 255.0 / 128.0) : vec4(1.0); // GS alpha
    if (uTarget != 0)
        texel.a = 1.0;
    fragColor = clamp(texel * colour, 0.0, 1.0);
    if (uAlphaTest != 0 && fragColor.a <= uAlphaRef)
        discard;
    if (uFog != 0)
        fragColor.rgb = mix(uFogColour, fragColor.rgb, clamp(vFog, 0.0, 1.0));
    if (uOpaque != 0)
        fragColor.a = 1.0;
}
)";

        // The 2D onto the window (what raylib's default shader did): window pixels from the top left, texel times the
        // vertex colour (bytes, 255 = 1.0).
        const char *kFlatVertexShader = R"(#version 330
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
uniform vec2 uViewport;
out vec2 vUv;
out vec4 vCol;
void main()
{
    gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);
    vUv = aUv;
    vCol = aCol;
}
)";
        const char *kFlatFragmentShader = R"(#version 330
in vec2 vUv;
in vec4 vCol;
uniform sampler2D uTex;
out vec4 fragColor;
void main()
{
    fragColor = texture(uTex, vUv) * vCol;
}
)";

        double secondsNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        struct FlatVertex
        {
            float x, y, u, v;
            uint8_t rgba[4];
        };
        static_assert(sizeof(FlatVertex) == 20u, "FlatVertex: five 4-byte fields");

        enum class FlatBlend
        {
            Alpha,         // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
            Additive,      // SRC_ALPHA, ONE
            Premultiplied, // ONE, ONE_MINUS_SRC_ALPHA
            Replace,       // ONE, ZERO: the frame as it is, its alpha ignored
        };

        gl::GLuint compileShader(gl::GLenum type, const char *source)
        {
            const gl::Api &g = gl::api();
            const gl::GLuint shader = g.CreateShader(type);
            g.ShaderSource(shader, 1, &source, nullptr);
            g.CompileShader(shader);
            gl::GLint ok = 0;
            g.GetShaderiv(shader, gl::kCompileStatus, &ok);
            if (!ok)
            {
                char log[2048] = {};
                g.GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                std::fprintf(stderr, "[renderer] shader compile failed: %s\n", log);
                g.DeleteShader(shader);
                return 0;
            }
            return shader;
        }

        gl::GLuint linkProgram(const char *vertex, const char *fragment)
        {
            const gl::Api &g = gl::api();
            const gl::GLuint vs = compileShader(gl::kVertexShader, vertex), fs = compileShader(gl::kFragmentShader, fragment);
            if (!vs || !fs)
                return 0;
            const gl::GLuint program = g.CreateProgram();
            g.AttachShader(program, vs);
            g.AttachShader(program, fs);
            g.LinkProgram(program);
            g.DeleteShader(vs);
            g.DeleteShader(fs);
            gl::GLint ok = 0;
            g.GetProgramiv(program, gl::kLinkStatus, &ok);
            if (!ok)
            {
                char log[2048] = {};
                g.GetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                std::fprintf(stderr, "[renderer] shader link failed: %s\n", log);
                g.DeleteProgram(program);
                return 0;
            }
            return program;
        }

        void setFlatBlend(FlatBlend blend)
        {
            const gl::Api &g = gl::api();
            g.Enable(gl::kBlend);
            g.BlendEquation(gl::kFuncAdd);
            switch (blend)
            {
            case FlatBlend::Alpha: g.BlendFunc(gl::kSrcAlpha, gl::kOneMinusSrcAlpha); break;
            case FlatBlend::Additive: g.BlendFunc(gl::kSrcAlpha, gl::kOne); break;
            case FlatBlend::Premultiplied: g.BlendFunc(gl::kOne, gl::kOneMinusSrcAlpha); break;
            case FlatBlend::Replace: g.BlendFunc(gl::kOne, gl::kZero); break;
            }
        }
    }

    struct Renderer::Gpu
    {
        gl::GLuint shader = 0;
        gl::GLint locM = -1, locScreen = -1, locDepthMax = -1, locUvMap = -1, locTex = -1, locTextured = -1, locTarget = -1,
                  locAlphaTest = -1, locAlphaRef = -1, locOpaque = -1, locFog = -1, locFogColour = -1;
        gl::GLuint vao = 0, vbo = 0;
        size_t vboBytes = 0;
        gl::GLuint flatShader = 0;
        gl::GLint locViewport = -1, locFlatTex = -1;
        gl::GLuint flatVao = 0, flatVbo = 0;
        size_t flatVboBytes = 0;
        std::vector<FlatVertex> flat; // the 2D being gathered for one draw
        gl::GLuint white = 0;         // 1 x 1 white: an untextured 2D draw
        // The frame's target (raylib's render texture: RGBA8 colour, a depth renderbuffer).
        gl::GLuint fbo = 0, colour = 0, depth = 0;
        int targetWidth = 0, targetHeight = 0;
        bool targetFiltered = false; // bilinear (render scale other than 1) rather than point
        gl::GLuint targetCopy = 0;   // Material::sampleTarget: the target's colour, copied per batch
        int copyWidth = 0, copyHeight = 0;
        struct GpuTexture
        {
            gl::GLuint id = 0;
            std::shared_ptr<const Texture> source; // held, so its address is not reused while compared
        };
        std::unordered_map<uint64_t, GpuTexture> textures;
        // The overlay's textures, their alpha from the GS's (0x80 is 1.0) to 255. By the texture's key when it has one
        // (a picture that changes: one slot, its image replaced), else its address.
        std::unordered_map<uint64_t, GpuTexture> overlayTextures;
        gl::GLuint displayPicture = 0; // the display's circuit 2 picture (bozo::render::displayPicture), its version
        uint64_t displayPictureVersion = 0;
        // The depth tiles read into a pixel-pack buffer at a present, taken at the next.
        gl::GLuint depthPbo = 0;
        size_t depthPboBytes = 0;
        std::vector<uint64_t> depthPending; // the tiles in the buffer, in order (T x T floats each)
        std::vector<int> depthPendingSize;  // each one's read width and height
        int depthPendingWidth = 0, depthPendingHeight = 0;

        gl::GLuint makeTexture(const uint32_t *rgba, int width, int height, gl::GLenum wrap)
        {
            const gl::Api &g = gl::api();
            gl::GLuint id = 0;
            g.GenTextures(1, &id);
            g.BindTexture(gl::kTexture2D, id);
            g.PixelStorei(gl::kUnpackAlignment, 4);
            g.TexImage2D(gl::kTexture2D, 0, static_cast<gl::GLint>(gl::kRgba8), width, height, 0, gl::kRgba, gl::kUnsignedByte, rgba);
            g.TexParameteri(gl::kTexture2D, gl::kTextureMagFilter, static_cast<gl::GLint>(gl::kLinear));
            g.TexParameteri(gl::kTexture2D, gl::kTextureMinFilter, static_cast<gl::GLint>(gl::kLinear));
            g.TexParameteri(gl::kTexture2D, gl::kTextureWrapS, static_cast<gl::GLint>(wrap));
            g.TexParameteri(gl::kTexture2D, gl::kTextureWrapT, static_cast<gl::GLint>(wrap));
            return id;
        }

        gl::GLuint overlayTexture(const std::shared_ptr<const Texture> &source)
        {
            GpuTexture &slot = overlayTextures[source->key != 0u ? source->key : reinterpret_cast<uintptr_t>(source.get())];
            if (slot.id == 0 || slot.source != source)
            {
                if (slot.id != 0)
                    gl::api().DeleteTextures(1, &slot.id);
                std::vector<uint32_t> rgba(source->rgba);
                for (uint32_t &c : rgba)
                    c = (c & 0xffffffu) | (std::min<uint32_t>(255u, ((c >> 24) * 255u + 64u) / 128u) << 24);
                slot.id = makeTexture(rgba.data(), static_cast<int>(source->width), static_cast<int>(source->height), gl::kClampToEdge);
                slot.source = source;
            }
            return slot.id;
        }

        gl::GLuint texture(uint64_t key, const std::shared_ptr<const Texture> &source)
        {
            GpuTexture &slot = textures[key];
            if (slot.source != source || slot.id == 0)
            {
                if (slot.id != 0)
                    gl::api().DeleteTextures(1, &slot.id);
                slot.id = makeTexture(source->rgba.data(), static_cast<int>(source->width), static_cast<int>(source->height), gl::kRepeat);
                slot.source = source;
            }
            return slot.id;
        }

        bool init()
        {
            const gl::Api &g = gl::api();
            shader = linkProgram(kVertexShader, kFragmentShader);
            flatShader = linkProgram(kFlatVertexShader, kFlatFragmentShader);
            if (shader == 0 || flatShader == 0)
                return false;
            locM = g.GetUniformLocation(shader, "uM");
            locScreen = g.GetUniformLocation(shader, "uScreen");
            locDepthMax = g.GetUniformLocation(shader, "uDepthMax");
            locUvMap = g.GetUniformLocation(shader, "uUvMap");
            locTex = g.GetUniformLocation(shader, "uTex");
            locTextured = g.GetUniformLocation(shader, "uTextured");
            locTarget = g.GetUniformLocation(shader, "uTarget");
            locAlphaTest = g.GetUniformLocation(shader, "uAlphaTest");
            locAlphaRef = g.GetUniformLocation(shader, "uAlphaRef");
            locOpaque = g.GetUniformLocation(shader, "uOpaque");
            locFog = g.GetUniformLocation(shader, "uFog");
            locFogColour = g.GetUniformLocation(shader, "uFogColour");
            locViewport = g.GetUniformLocation(flatShader, "uViewport");
            locFlatTex = g.GetUniformLocation(flatShader, "uTex");
            g.GenVertexArrays(1, &vao);
            g.GenVertexArrays(1, &flatVao);
            g.GenBuffers(1, &flatVbo);
            g.BindVertexArray(flatVao);
            g.BindBuffer(gl::kArrayBuffer, flatVbo);
            g.VertexAttribPointer(0, 2, gl::kFloat, gl::kFalse, sizeof(FlatVertex), reinterpret_cast<const void *>(0));
            g.VertexAttribPointer(1, 2, gl::kFloat, gl::kFalse, sizeof(FlatVertex), reinterpret_cast<const void *>(8));
            g.VertexAttribPointer(2, 4, gl::kUnsignedByte, gl::kTrue, sizeof(FlatVertex), reinterpret_cast<const void *>(16));
            g.EnableVertexAttribArray(0);
            g.EnableVertexAttribArray(1);
            g.EnableVertexAttribArray(2);
            g.BindVertexArray(0);
            const uint32_t opaqueWhite = 0xffffffffu;
            white = makeTexture(&opaqueWhite, 1, 1, gl::kClampToEdge);
            return true;
        }

        void release()
        {
            const gl::Api &g = gl::api();
            for (auto *map : {&textures, &overlayTextures})
                for (auto &[key, slot] : *map)
                    if (slot.id != 0)
                        g.DeleteTextures(1, &slot.id);
            textures.clear();
            overlayTextures.clear();
            for (gl::GLuint *t : {&white, &colour, &targetCopy, &displayPicture})
                if (*t != 0)
                    g.DeleteTextures(1, t);
            if (depth != 0)
                g.DeleteRenderbuffers(1, &depth);
            if (fbo != 0)
                g.DeleteFramebuffers(1, &fbo);
            for (gl::GLuint *b : {&vbo, &flatVbo, &depthPbo})
                if (*b != 0)
                    g.DeleteBuffers(1, b);
            for (gl::GLuint *a : {&vao, &flatVao})
                if (*a != 0)
                    g.DeleteVertexArrays(1, a);
            if (shader != 0)
                g.DeleteProgram(shader);
            if (flatShader != 0)
                g.DeleteProgram(flatShader);
        }

        // The target at (width x height): a colour texture and a depth renderbuffer.
        bool target(int width, int height)
        {
            const gl::Api &g = gl::api();
            if (targetWidth == width && targetHeight == height)
                return true;
            if (fbo == 0)
            {
                g.GenFramebuffers(1, &fbo);
                g.GenTextures(1, &colour);
                g.GenRenderbuffers(1, &depth);
            }
            g.BindTexture(gl::kTexture2D, colour);
            g.TexImage2D(gl::kTexture2D, 0, static_cast<gl::GLint>(gl::kRgba8), width, height, 0, gl::kRgba, gl::kUnsignedByte, nullptr);
            g.TexParameteri(gl::kTexture2D, gl::kTextureWrapS, static_cast<gl::GLint>(gl::kRepeat));
            g.TexParameteri(gl::kTexture2D, gl::kTextureWrapT, static_cast<gl::GLint>(gl::kRepeat));
            g.TexParameteri(gl::kTexture2D, gl::kTextureMagFilter, static_cast<gl::GLint>(gl::kNearest));
            g.TexParameteri(gl::kTexture2D, gl::kTextureMinFilter, static_cast<gl::GLint>(gl::kNearest));
            targetFiltered = false;
            g.BindRenderbuffer(gl::kRenderbuffer, depth);
            g.RenderbufferStorage(gl::kRenderbuffer, gl::kDepthComponent24, width, height);
            g.BindFramebuffer(gl::kFramebuffer, fbo);
            g.FramebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment0, gl::kTexture2D, colour, 0);
            g.FramebufferRenderbuffer(gl::kFramebuffer, gl::kDepthAttachment, gl::kRenderbuffer, depth);
            const bool complete = g.CheckFramebufferStatus(gl::kFramebuffer) == gl::kFramebufferComplete;
            g.BindFramebuffer(gl::kFramebuffer, 0);
            if (!complete)
            {
                std::fprintf(stderr, "[renderer] the %d x %d target is incomplete\n", width, height);
                return false;
            }
            targetWidth = width;
            targetHeight = height;
            return true;
        }

        // The gathered 2D, drawn with `textureId` under `blend` onto the current framebuffer (viewport w x h).
        void flushFlat(gl::GLuint textureId, FlatBlend blend, int viewportWidth, int viewportHeight)
        {
            if (flat.empty())
                return;
            const gl::Api &g = gl::api();
            const size_t bytes = flat.size() * sizeof(FlatVertex);
            g.BindVertexArray(flatVao);
            g.BindBuffer(gl::kArrayBuffer, flatVbo);
            if (bytes > flatVboBytes)
            {
                flatVboBytes = std::max(bytes, flatVboBytes * 2u);
                g.BufferData(gl::kArrayBuffer, static_cast<gl::GLsizeiptr>(flatVboBytes), nullptr, gl::kStreamDraw);
            }
            g.BufferSubData(gl::kArrayBuffer, 0, static_cast<gl::GLsizeiptr>(bytes), flat.data());
            g.UseProgram(flatShader);
            g.Uniform2f(locViewport, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
            g.Uniform1i(locFlatTex, 0);
            g.ActiveTexture(gl::kTexture0);
            g.BindTexture(gl::kTexture2D, textureId);
            setFlatBlend(blend);
            g.DrawArrays(gl::kTriangles, 0, static_cast<gl::GLsizei>(flat.size()));
            g.BindVertexArray(0);
            flat.clear();
        }

        // A textured rectangle onto the current framebuffer: (x, y) to (x + w, y + h) in window pixels, the texture's
        // (u0, v0) at the top left and (u1, v1) at the bottom right, opaque white.
        void rectangle(gl::GLuint textureId, FlatBlend blend, float x, float y, float w, float h, float u0, float v0, float u1,
                       float v1, int viewportWidth, int viewportHeight)
        {
            const FlatVertex tl{x, y, u0, v0, {255, 255, 255, 255}}, tr{x + w, y, u1, v0, {255, 255, 255, 255}},
                bl{x, y + h, u0, v1, {255, 255, 255, 255}}, br{x + w, y + h, u1, v1, {255, 255, 255, 255}};
            flat.insert(flat.end(), {tl, bl, br, tl, br, tr});
            flushFlat(textureId, blend, viewportWidth, viewportHeight);
        }
    };

    void Renderer::releaseGpu()
    {
        if (m_gpu)
            m_gpu->release();
        m_gpu.reset();
        m_shown.reset();
    }

    // The overlay onto the window: GS coordinates to the window rectangle, colours from the GS's (0x80 is 1.0), in
    // submission order. A sprite's two triangles (a, b, c) (b, e, c) are drawn as the quad a, b, e, c split a, b, e /
    // a, e, c (as raylib's quads were); any other triangle as it is.
    void Renderer::drawOverlay(const Frame &overlay, float x, float y, float width, float height)
    {
        const gl::Api &g = gl::api();
        Gpu &gpu = *m_gpu;
        gl::GLint viewport[4] = {0, 0, 1, 1};
        g.GetIntegerv(gl::kViewport, viewport);
        const Screen &screen = overlay.screen;
        const float sx = width / screen.width, sy = height / screen.height;
        g.Disable(gl::kDepthTest);
        g.Disable(gl::kCullFace);
        g.Disable(gl::kScissorTest);
        const auto scale = [](uint32_t c) { return static_cast<uint8_t>(std::min<uint32_t>(255u, (c * 255u + 64u) / 128u)); };
        gl::GLuint texture = 0;
        FlatBlend blend = FlatBlend::Alpha;
        for (const Batch &batch : overlay.batches)
        {
            const Material &m = batch.material;
            const FlatBlend want = m.blend == Material::Blend::Additive || m.blend == Material::Blend::AdditiveByDestination
                                       ? FlatBlend::Additive
                                       : FlatBlend::Alpha;
            const gl::GLuint id = m.textured && batch.texture ? gpu.overlayTexture(batch.texture) : gpu.white;
            if (id != texture || want != blend)
            {
                gpu.flushFlat(texture, blend, viewport[2], viewport[3]);
                texture = id;
                blend = want;
            }
            const auto put = [&](const Vertex &v)
            {
                FlatVertex f;
                f.x = x + (v.x - screen.originX) * sx;
                f.y = y + (v.y - screen.originY) * sy;
                f.u = v.u;
                f.v = v.v;
                f.rgba[0] = scale(v.rgba & 0xffu);
                f.rgba[1] = scale((v.rgba >> 8) & 0xffu);
                f.rgba[2] = scale((v.rgba >> 16) & 0xffu);
                f.rgba[3] = m.blend == Material::Blend::Opaque ? 255 : scale(v.rgba >> 24);
                gpu.flat.push_back(f);
            };
            const auto same = [](const Vertex &a, const Vertex &b)
            { return a.x == b.x && a.y == b.y && a.u == b.u && a.v == b.v && a.rgba == b.rgba; };
            const uint32_t end = batch.first + batch.count;
            for (uint32_t i = batch.first; i + 3u <= end && i + 3u <= overlay.vertices.size();)
            {
                const Vertex *t = &overlay.vertices[i];
                if (i + 6u <= end && same(t[3], t[1]) && same(t[5], t[2]))
                {
                    put(t[0]); // the quad a, b, e, c
                    put(t[1]);
                    put(t[4]);
                    put(t[0]);
                    put(t[4]);
                    put(t[2]);
                    i += 6u;
                    continue;
                }
                put(t[0]);
                put(t[1]);
                put(t[2]);
                i += 3u;
            }
        }
        gpu.flushFlat(texture, blend, viewport[2], viewport[3]);
    }

    void Renderer::wantDepth() { m_depthWantedAt.store(secondsNow(), std::memory_order_relaxed); }

    bool Renderer::depthAt(float gsX, float gsY, uint32_t &gsZ)
    {
        std::lock_guard<std::mutex> lock(m_depthMutex);
        if (m_depthWidth <= 0 || m_depthScreen.width <= 0.0f || m_depthScreen.height <= 0.0f)
            return false;
        const int x = static_cast<int>((gsX - m_depthScreen.originX) / m_depthScreen.width * static_cast<float>(m_depthWidth));
        const int y = static_cast<int>((gsY - m_depthScreen.originY) / m_depthScreen.height * static_cast<float>(m_depthHeight));
        if (x < 0 || y < 0 || x >= m_depthWidth || y >= m_depthHeight)
            return false;
        // Its tile and the ones around it are read from now on (what moves a little by the next frame is still read).
        const int tx = x / kDepthTile, ty = y / kDepthTile;
        const double now = secondsNow();
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (tx + dx >= 0 && ty + dy >= 0 && (tx + dx) * kDepthTile < m_depthWidth && (ty + dy) * kDepthTile < m_depthHeight)
                    m_depthTilesWanted[static_cast<uint64_t>(ty + dy) << 32 | static_cast<uint32_t>(tx + dx)] = now;
        const auto tile = m_depthTiles.find(static_cast<uint64_t>(ty) << 32 | static_cast<uint32_t>(tx));
        if (tile == m_depthTiles.end())
            return false; // not read yet
        // present()'s depth: window depth 1 - (z / w) / depthMax (cleared to 1, far), GS Z = z / w * gsZScale.
        const float d = tile->second[static_cast<size_t>(y - ty * kDepthTile) * kDepthTile + static_cast<size_t>(x - tx * kDepthTile)];
        const double z = (1.0 - static_cast<double>(d)) * m_depthScreen.depthMax * m_depthScreen.gsZScale;
        gsZ = static_cast<uint32_t>(std::clamp(z, 0.0, 4294967295.0));
        return true;
    }

    bool Renderer::present(float x, float y, float width, float height)
    {
        if (!gl::loaded())
            return false;
        const gl::Api &g = gl::api();
        std::shared_ptr<const Frame> published, overlay, backdrop;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            published = m_published;
            overlay = m_overlayPublished;
            backdrop = m_backdrop;
        }
        if (published && published != m_shown && (!m_shown || published->time > m_shown->time))
            m_shown = published;
        if (backdrop && secondsNow() - backdrop->time > 0.2)
            backdrop = nullptr;
        // Nothing of the port's own frame yet (a movie, a loading bar): only its 2D and the movie's picture.
        const bool overlayOnly = !m_shown && ((overlay && !overlay->batches.empty()) || backdrop);
        if (!m_enabled || (!overlayOnly && !m_shown))
            return false;
        if (!m_gpu)
        {
            m_gpu = std::make_shared<Gpu>();
            if (!m_gpu->init())
            {
                m_gpu->release();
                m_enabled = false;
                return false;
            }
        }
        Gpu &gpu = *m_gpu;
        gl::GLint viewport[4] = {0, 0, 1, 1};
        g.GetIntegerv(gl::kViewport, viewport);
        if (overlayOnly)
        {
            if (backdrop)
                drawOverlay(*backdrop, x, y, width, height);
            if (overlay)
                drawOverlay(*overlay, x, y, width, height);
            return true;
        }
        const std::shared_ptr<const Frame> frame = m_shown;
        const float scale = renderScale();
        const int w = std::max(1, static_cast<int>(std::lround(width * scale))), h = std::max(1, static_cast<int>(std::lround(height * scale)));
        if (!gpu.target(w, h))
            return false;
        // Filtered to the window when the render scale makes it another size, as it is (point) otherwise.
        if (const bool filtered = scale != 1.0f; filtered != gpu.targetFiltered)
        {
            g.BindTexture(gl::kTexture2D, gpu.colour);
            g.TexParameteri(gl::kTexture2D, gl::kTextureMagFilter, static_cast<gl::GLint>(filtered ? gl::kLinear : gl::kNearest));
            g.TexParameteri(gl::kTexture2D, gl::kTextureMinFilter, static_cast<gl::GLint>(filtered ? gl::kLinear : gl::kNearest));
            gpu.targetFiltered = filtered;
        }

        const size_t bytes = frame->vertices.size() * sizeof(Vertex);
        g.BindVertexArray(gpu.vao);
        if (bytes > gpu.vboBytes)
        {
            if (gpu.vbo != 0)
                g.DeleteBuffers(1, &gpu.vbo);
            gpu.vboBytes = std::max(bytes, gpu.vboBytes * 2u);
            g.GenBuffers(1, &gpu.vbo);
            g.BindBuffer(gl::kArrayBuffer, gpu.vbo);
            g.BufferData(gl::kArrayBuffer, static_cast<gl::GLsizeiptr>(gpu.vboBytes), nullptr, gl::kDynamicDraw);
            g.VertexAttribPointer(0, 3, gl::kFloat, gl::kFalse, sizeof(Vertex), reinterpret_cast<const void *>(0));
            g.VertexAttribPointer(1, 2, gl::kFloat, gl::kFalse, sizeof(Vertex), reinterpret_cast<const void *>(12));
            g.VertexAttribPointer(2, 4, gl::kUnsignedByte, gl::kTrue, sizeof(Vertex), reinterpret_cast<const void *>(20));
            g.VertexAttribPointer(3, 1, gl::kFloat, gl::kFalse, sizeof(Vertex), reinterpret_cast<const void *>(24));
            g.EnableVertexAttribArray(0);
            g.EnableVertexAttribArray(1);
            g.EnableVertexAttribArray(2);
            g.EnableVertexAttribArray(3);
        }
        if (bytes != 0u)
        {
            g.BindBuffer(gl::kArrayBuffer, gpu.vbo);
            g.BufferSubData(gl::kArrayBuffer, 0, static_cast<gl::GLsizeiptr>(bytes), frame->vertices.data());
        }

        const Screen &screen = frame->screen;
        g.BindFramebuffer(gl::kFramebuffer, gpu.fbo);
        g.Viewport(0, 0, w, h);
        g.Disable(gl::kScissorTest);
        g.DepthMask(gl::kTrue);
        if (m_presentsAlone)
        {
            const uint32_t c = m_clearColour.load(std::memory_order_relaxed);
            // Alpha 0: under it, circuit 2 shows (menus).
            g.ClearColor(static_cast<float>(c & 0xffu) / 255.0f, static_cast<float>((c >> 8) & 0xffu) / 255.0f,
                         static_cast<float>((c >> 16) & 0xffu) / 255.0f, 0.0f);
        }
        else
            g.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        g.ClearDepth(1.0);
        g.Clear(gl::kColorBufferBit | gl::kDepthBufferBit);
        const float screenRect[4] = {screen.originX, screen.originY, screen.width, screen.height};
        g.Disable(gl::kCullFace);
        g.DepthFunc(gl::kLequal);
        g.UseProgram(gpu.shader);
        g.Uniform4fv(gpu.locScreen, 1, screenRect);
        g.Uniform1f(gpu.locDepthMax, screen.depthMax);
        g.Uniform1i(gpu.locTex, 0);
        g.ActiveTexture(gl::kTexture0);
        const float uvAsGiven[4] = {1.0f, 0.0f, 1.0f, 0.0f};
        // Each batch's clip (the port's setClip) in the target's pixels, bottom-up; x1 <= x0: none.
        struct Rect
        {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        };
        const auto clipOf = [&](const Batch &batch)
        {
            Rect rect;
            const float *own = batch.clip;
            if (own[2] <= own[0])
                return rect;
            const float sx = static_cast<float>(w) / screen.width, sy = static_cast<float>(h) / screen.height;
            rect.x0 = std::clamp(static_cast<int>(std::lround((own[0] - screen.originX) * sx)), 0, w);
            rect.x1 = std::clamp(static_cast<int>(std::lround((own[2] - screen.originX) * sx)), 0, w);
            rect.y0 = std::clamp(static_cast<int>(std::lround(h - (own[3] - screen.originY) * sy)), 0, h);
            rect.y1 = std::clamp(static_cast<int>(std::lround(h - (own[1] - screen.originY) * sy)), 0, h);
            if (rect.x0 == 0 && rect.y0 == 0 && rect.x1 == w && rect.y1 == h)
                rect = Rect{}; // the whole target: no clip
            else if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                rect = Rect{0, 0, 0, -1}; // nothing shows
            return rect;
        };
        bool scissored = false;
        for (const Batch &batch : frame->batches)
        {
            const Material &m = batch.material;
            const Rect clip = clipOf(batch);
            if (clip.y1 < 0)
                continue;
            if (m.blend == Material::Blend::Opaque)
                g.Disable(gl::kBlend);
            else
            {
                g.Enable(gl::kBlend);
                g.BlendFuncSeparate(gl::kSrcAlpha, m.blend == Material::Blend::Alpha ? gl::kOneMinusSrcAlpha : gl::kOne, gl::kOne,
                                    gl::kOneMinusSrcAlpha);
                g.BlendEquation(gl::kFuncAdd);
            }
            g.DepthMask(m.depthWrite ? gl::kTrue : gl::kFalse);
            // The GS's depth test is GEQUAL (larger nearer): here LEQUAL on 1 - z, so what lies at the far end (a sky at
            // Z 0, a clear) passes against the cleared depth as on the GS; a draw without ZTE is not tested.
            if (m.depthTest)
                g.Enable(gl::kDepthTest);
            else
                g.Disable(gl::kDepthTest);
            g.Uniform4fv(gpu.locM, 4, batch.matrix);
            const std::shared_ptr<const Texture> &texture = batch.texture;
            const int textured = m.textured && texture ? 1 : 0, alphaTest = m.alphaTest ? 1 : 0;
            const int opaque = m.blend == Material::Blend::Opaque ? 1 : 0, fog = m.fog ? 1 : 0;
            const float fogColour[3] = {static_cast<float>(m.fogColour & 0xffu) / 255.0f, static_cast<float>((m.fogColour >> 8) & 0xffu) / 255.0f,
                                        static_cast<float>((m.fogColour >> 16) & 0xffu) / 255.0f};
            g.Uniform1i(gpu.locOpaque, opaque);
            g.Uniform1i(gpu.locFog, fog);
            g.Uniform3fv(gpu.locFogColour, 1, fogColour);
            g.Uniform1i(gpu.locAlphaTest, alphaTest);
            g.Uniform1f(gpu.locAlphaRef, m.alphaRef / 128.0f);
            if (m.sampleTarget)
            {
                // The frame drawn so far, as the texture: u, v in its GS pixels from the screen's top left.
                if (gpu.targetCopy == 0 || gpu.copyWidth != w || gpu.copyHeight != h)
                {
                    if (gpu.targetCopy != 0)
                        g.DeleteTextures(1, &gpu.targetCopy);
                    gpu.targetCopy = gpu.makeTexture(nullptr, w, h, gl::kClampToEdge);
                    gpu.copyWidth = w;
                    gpu.copyHeight = h;
                }
                g.BindTexture(gl::kTexture2D, gpu.targetCopy);
                g.CopyTexSubImage2D(gl::kTexture2D, 0, 0, 0, 0, 0, w, h);
                const float toTarget[4] = {1.0f / screen.width, 0.0f, -1.0f / screen.height, 1.0f};
                g.Uniform4fv(gpu.locUvMap, 1, toTarget);
                g.Uniform1i(gpu.locTextured, 1);
                g.Uniform1i(gpu.locTarget, 1);
            }
            else
            {
                g.Uniform4fv(gpu.locUvMap, 1, uvAsGiven);
                g.Uniform1i(gpu.locTextured, textured);
                g.Uniform1i(gpu.locTarget, 0);
                if (textured)
                {
                    const gl::GLuint id = gpu.texture(m.tex0 ^ (m.texa * 0x9e3779b97f4a7c15ull) ^ (m.region * 0xc2b2ae3d27d4eb4full), texture);
                    g.BindTexture(gl::kTexture2D, id);
                    g.TexParameteri(gl::kTexture2D, gl::kTextureWrapS, static_cast<gl::GLint>(m.clampU ? gl::kClampToEdge : gl::kRepeat));
                    g.TexParameteri(gl::kTexture2D, gl::kTextureWrapT, static_cast<gl::GLint>(m.clampV ? gl::kClampToEdge : gl::kRepeat));
                }
            }
            if (clip.x1 > clip.x0)
            {
                g.Enable(gl::kScissorTest);
                g.Scissor(clip.x0, clip.y0, clip.x1 - clip.x0, clip.y1 - clip.y0);
                scissored = true;
            }
            else if (scissored)
            {
                g.Disable(gl::kScissorTest);
                scissored = false;
            }
            g.DrawArrays(gl::kTriangles, static_cast<gl::GLint>(batch.first), static_cast<gl::GLsizei>(batch.count));
        }
        g.Disable(gl::kScissorTest);
        g.BindVertexArray(0);
        if (secondsNow() - m_depthWantedAt.load(std::memory_order_relaxed) < 1.0)
            readDepthTiles(w, h, screen);
        g.DepthMask(gl::kTrue);
        g.Disable(gl::kDepthTest);
        g.BindFramebuffer(gl::kFramebuffer, 0);
        g.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);

        // Under the frame, as the display merges it: the picture the game shows through circuit 2 (a menu's movie frame,
        // where the frame's alpha is 0), then a movie's backdrop. Without either the frame is shown alone, its alpha
        // ignored (PMODE with circuit 1 only: what a draw leaves in the alpha, a sky's 0, never shows).
        bool under = backdrop != nullptr;
        {
            const DisplayPicture picture = displayPicture();
            if (picture.rgba && picture.width != 0u && picture.height != 0u)
            {
                under = true;
                if (gpu.displayPictureVersion != picture.version || gpu.displayPicture == 0u)
                {
                    if (gpu.displayPicture != 0u)
                        g.DeleteTextures(1, &gpu.displayPicture);
                    std::vector<uint32_t> rgba(*picture.rgba);
                    for (uint32_t &c : rgba)
                        c = (c & 0xffffffu) | 0xff000000u;
                    gpu.displayPicture = gpu.makeTexture(rgba.data(), static_cast<int>(picture.width), static_cast<int>(picture.height), gl::kRepeat);
                    gpu.displayPictureVersion = picture.version;
                }
                gpu.rectangle(gpu.displayPicture, FlatBlend::Alpha, x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f, viewport[2], viewport[3]);
            }
        }
        if (backdrop)
            drawOverlay(*backdrop, x, y, width, height);
        // The target's rows are bottom-up: its top (v = 1) at the rectangle's top.
        gpu.rectangle(gpu.colour, under || !m_presentsAlone ? FlatBlend::Premultiplied : FlatBlend::Replace, x, y, width, height, 0.0f, 1.0f,
                      1.0f, 0.0f, viewport[2], viewport[3]);
        // Over it, the port's 2D.
        if (overlay)
            drawOverlay(*overlay, x, y, width, height);
        g.UseProgram(0);
        g.Enable(gl::kBlend);
        g.BlendFunc(gl::kSrcAlpha, gl::kOneMinusSrcAlpha);
        return true;
    }

    // The depth tiles depthAt asked for in the last half second: read from the target (bound) into a pixel-pack buffer
    // now, taken from the buffer the next present (one present late, so the read does not wait for the GPU).
    void Renderer::readDepthTiles(int w, int h, const Screen &screen)
    {
        const gl::Api &g = gl::api();
        Gpu &gpu = *m_gpu;
        std::vector<uint64_t> tiles;
        {
            std::lock_guard<std::mutex> lock(m_depthMutex);
            if (w != m_depthWidth || h != m_depthHeight)
            {
                m_depthTiles.clear();
                m_depthTilesWanted.clear();
            }
            m_depthWidth = w;
            m_depthHeight = h;
            m_depthScreen = screen;
            const double now = secondsNow();
            for (auto it = m_depthTilesWanted.begin(); it != m_depthTilesWanted.end();)
                if (now - it->second > 0.5)
                    it = m_depthTilesWanted.erase(it);
                else
                    tiles.push_back((it++)->first);
        }
        constexpr size_t kTileBytes = static_cast<size_t>(kDepthTile) * kDepthTile * sizeof(float);
        // The previous present's tiles, if they were of this size.
        if (!gpu.depthPending.empty() && gpu.depthPbo != 0)
        {
            std::unordered_map<uint64_t, std::vector<float>> depth;
            if (gpu.depthPendingWidth == w && gpu.depthPendingHeight == h)
            {
                g.BindBuffer(gl::kPixelPackBuffer, gpu.depthPbo);
                const auto *mapped = static_cast<const float *>(
                    g.MapBufferRange(gl::kPixelPackBuffer, 0, static_cast<gl::GLsizeiptr>(gpu.depthPending.size() * kTileBytes), gl::kMapReadBit));
                if (mapped)
                {
                    for (size_t i = 0; i < gpu.depthPending.size(); ++i)
                    {
                        const int cw = gpu.depthPendingSize[2 * i], ch = gpu.depthPendingSize[2 * i + 1];
                        const float *src = mapped + i * kDepthTile * kDepthTile;
                        std::vector<float> &tile = depth[gpu.depthPending[i]];
                        tile.assign(static_cast<size_t>(kDepthTile) * kDepthTile, 1.0f); // past the edge: far
                        for (int r = 0; r < ch; ++r) // read bottom-up, kept top-down
                            std::memcpy(&tile[static_cast<size_t>(ch - 1 - r) * kDepthTile], src + static_cast<size_t>(r) * cw,
                                        static_cast<size_t>(cw) * sizeof(float));
                    }
                    g.UnmapBuffer(gl::kPixelPackBuffer);
                }
                g.BindBuffer(gl::kPixelPackBuffer, 0);
            }
            std::lock_guard<std::mutex> lock(m_depthMutex);
            m_depthTiles.swap(depth);
        }
        gpu.depthPending.clear();
        gpu.depthPendingSize.clear();
        if (tiles.empty())
            return;
        const size_t bytes = tiles.size() * kTileBytes;
        if (gpu.depthPbo == 0)
            g.GenBuffers(1, &gpu.depthPbo);
        g.BindBuffer(gl::kPixelPackBuffer, gpu.depthPbo);
        if (bytes > gpu.depthPboBytes)
        {
            gpu.depthPboBytes = std::max(bytes, gpu.depthPboBytes * 2u);
            g.BufferData(gl::kPixelPackBuffer, static_cast<gl::GLsizeiptr>(gpu.depthPboBytes), nullptr, gl::kStreamRead);
        }
        g.PixelStorei(gl::kPackAlignment, 4);
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            const int tx = static_cast<int>(tiles[i] & 0xffffffffu), ty = static_cast<int>(tiles[i] >> 32);
            const int x0 = tx * kDepthTile, top = ty * kDepthTile;
            const int cw = std::min(kDepthTile, w - x0), ch = std::min(kDepthTile, h - top);
            if (cw <= 0 || ch <= 0)
                continue;
            g.ReadPixels(x0, h - (top + ch), cw, ch, gl::kDepthComponent, gl::kFloat,
                         reinterpret_cast<void *>(static_cast<uintptr_t>(gpu.depthPending.size() * kTileBytes)));
            gpu.depthPending.push_back(tiles[i]);
            gpu.depthPendingSize.push_back(cw);
            gpu.depthPendingSize.push_back(ch);
        }
        g.BindBuffer(gl::kPixelPackBuffer, 0);
        gpu.depthPendingWidth = w;
        gpu.depthPendingHeight = h;
    }
}
