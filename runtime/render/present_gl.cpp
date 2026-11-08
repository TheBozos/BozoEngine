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
// Frames drawn between the game's (the Video settings' frame rate above 60: matchFrames, pickInterpolated, the keyed
// batches' matrices and the tracked triangles between two frames) moved from the same file in phase 4 M4b, without its
// statistics and trace logs. Not moved (the GS front end's): the GS-stream groups and their clips, the texture
// cache's lookups.
// Written fresh for BozoEngine: the depth readback (a pixel-pack buffer per present, mapped one present later: the
// fork read it in its GS-side file) and Material::sampleTarget (the frame drawn so far copied, then sampled).
#include "bozo/render/gl.h"
#include "bozo/render/movie.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <string>

namespace
{
    // Material::sampleTarget (TMB's heat haze: the frame drawn so far, sampled).
    // Enabled for the original game's distortion; BOZO_HEAT_HAZE=0 restores the older port's picture.
    bool sampleTargetEnabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("BOZO_HEAT_HAZE");
            return !v || *v != '0';
        }();
        return on;
    }
}

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
layout(location = 4) in float aQ;
uniform int uUseQ;
uniform vec4 uM[4];
uniform vec4 uScreen;
uniform float uDepthMax;
uniform vec4 uUvMap;
out vec2 vUv;
// Vertex colours are screen-space Gouraud values; only ST needs perspective.
noperspective out vec4 vCol;
out float vFog;
noperspective out vec4 vColScreen;
noperspective out float vFogScreen;
noperspective out float vQ;
void main()
{
    vec4 c = uM[0] * aPos.x + uM[1] * aPos.y + uM[2] * aPos.z + uM[3];
    float w = uUseQ != 0 && aQ > 0.0 ? c.w / aQ : c.w;
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
    vColScreen = aCol;
    vFogScreen = aFog;
    vQ = uUseQ != 0 && aQ > 0.0 ? aQ : 1.0 / c.w;
}
)";

        // (A - B) * C + D on the GPU's blender (a factor per colour). What it has no form for, a colour times 1 + C, saturates the
        // factor at 1.
        void applyGsBlend(const gl::Api &g, const Material &m)
        {
            const uint32_t a = m.blendA, b = m.blendB, c = m.blendC, d = m.blendD;
            const gl::GLenum factor = c == 0u ? gl::kSrcAlpha : c == 1u ? gl::kDstAlpha : gl::kConstantAlpha;
            const gl::GLenum inverse = c == 0u ? gl::kOneMinusSrcAlpha : c == 1u ? gl::kOneMinusDstAlpha : gl::kOneMinusConstantAlpha;
            g.Enable(gl::kBlend);
            g.BlendColor(0.0f, 0.0f, 0.0f, std::min(static_cast<float>(m.blendFix) / 128.0f, 1.0f));
            gl::GLenum src = gl::kZero, dst = gl::kZero, equation = gl::kFuncAdd;
            const uint32_t form = a == b ? 9u : a * 3u + b;
            switch (form)
            {
            case 9u: // D
                src = d == 0u ? gl::kOne : gl::kZero;
                dst = d == 1u ? gl::kOne : gl::kZero;
                break;
            case 1u: // (Cs - Cd) * C + D
                src = factor;
                dst = d == 1u ? inverse : factor;
                equation = d == 1u ? gl::kFuncAdd : gl::kFuncSubtract;
                break;
            case 2u: // Cs * C + D
                src = factor;
                dst = d == 1u ? gl::kOne : gl::kZero;
                break;
            case 3u: // (Cd - Cs) * C + D
                src = d == 0u ? inverse : factor;
                dst = d == 1u ? gl::kOne : factor;
                equation = d == 2u ? gl::kFuncReverseSubtract : gl::kFuncAdd;
                break;
            case 5u: // Cd * C + D
                src = d == 0u ? gl::kOne : gl::kZero;
                dst = d == 1u ? gl::kOne : factor;
                break;
            case 6u: // D - Cs * C
                src = d == 0u ? inverse : d == 1u ? factor : gl::kZero;
                dst = d == 1u ? gl::kOne : gl::kZero;
                equation = d == 1u ? gl::kFuncReverseSubtract : gl::kFuncAdd;
                break;
            default: // D - Cd * C
                src = d == 0u ? gl::kOne : gl::kZero;
                dst = d == 0u ? factor : d == 1u ? inverse : gl::kZero;
                equation = d == 0u ? gl::kFuncSubtract : gl::kFuncAdd;
                break;
            }
            g.BlendFuncSeparate(src, dst, gl::kOne, gl::kZero);
            g.BlendEquation(equation);
        }

        // GS modulate: texel * vertex colour, where a vertex colour of 0x80 is 1.0, then fog toward the fog colour (RGB
        // only, before blending). The render target holds premultiplied colour and coverage (opaque draws cover
        // fully), so present() composites it over the window. uTarget: the texture is the target's copy, its alpha
        // taken as the GS's 0x80 (1.0).
        const char *kFragmentShader = R"(#version 330
in vec2 vUv;
noperspective in vec4 vCol;
in float vFog;
noperspective in vec4 vColScreen;
noperspective in float vFogScreen;
noperspective in float vQ;
uniform int uUseQ;
uniform sampler2D uTex;
uniform int uLodMode;
uniform float uLodScale;
uniform float uLodBias;
uniform int uTextured;
uniform int uDirectColour;
uniform vec4 uUvBounds;
uniform int uTarget;
uniform int uAlphaTest;
uniform int uAlphaFunc;
uniform float uAlphaRef;
uniform int uOpaque;
uniform int uFog;
uniform vec3 uFogColour;
uniform int uLevels;
uniform vec3 uLevelGain;
uniform vec3 uLevelHigh;
uniform vec3 uLevelLow;
uniform int uTextureFunction;
uniform int uTextureAlpha;
uniform int uImageRadius;
uniform vec4 uImageStep;
uniform vec4 uImageKernel[5];
uniform int uImageDepth;
uniform sampler2D uImageDepthTex;
uniform vec4 uImageWeights[64];
uniform vec4 uImageSize;
uniform int uImageAlpha;
uniform sampler2D uImageAlphaTex;
out vec4 fragColor;
vec2 atlasUv(vec2 uv)
{
    if (uUvBounds.z > uUvBounds.x && uUvBounds.w > uUvBounds.y)
        return clamp(uv, uUvBounds.xy, uUvBounds.zw);
    return uv;
}
vec4 sampled()
{
    vec2 uv = atlasUv(vUv);
    if (uImageRadius != 0) {
        vec4 sum=textureLod(uTex,uv,0.0)*uImageKernel[0][0];
        for(int i=1;i<=16;++i) {
            if(i>uImageRadius)break;
            vec2 offset=uImageStep.xy*float(i);
            sum+=(textureLod(uTex,atlasUv(uv+offset),0.0)+textureLod(uTex,atlasUv(uv-offset),0.0))*uImageKernel[i/4][i%4];
        }
        return sum;
    }
    if (uLodMode == 0)
        return texture(uTex, uv);
    float lod = uLodMode == 1 ? -log2(max(abs(vQ), 1e-30)) * uLodScale + uLodBias : uLodBias;
    return textureLod(uTex, uv, lod);
}
void main()
{
    vec4 vertex = uUseQ != 0 ? vColScreen : vCol;
    vec4 colour = vertex * (255.0 / 128.0);
    if (uTextured == 0 && uDirectColour != 0)
        colour.rgb = vertex.rgb;
    vec4 texel = uTextured != 0 ? sampled() * vec4(1.0, 1.0, 1.0, 255.0 / 128.0) : vec4(1.0); // GS alpha
    if(uImageDepth != 0) {
        float depth=texture(uImageDepthTex,gl_FragCoord.xy/uImageSize.xy).r;
        int index=int(clamp(floor((1.0-depth)*uImageSize.z),0.0,255.0));
        texel=vec4(vec3(uImageWeights[index/4][index%4]),1.0);
    }
    if (uTarget == 1)
        texel.a = 1.0;
    else if (uTarget == 2)
        texel.a = texture(uTex, atlasUv(vUv)).a;
    if (uTextureFunction == 1)
        fragColor = texel;
    else if (uTextureFunction >= 2)
        fragColor = vec4(texel.rgb * colour.rgb + vertex.a, uTextureFunction == 2 ? texel.a + colour.a : texel.a);
    else
        fragColor = texel * colour;
    if (uTextured != 0 && uTextureAlpha == 0)
        fragColor.a = colour.a;
    fragColor = clamp(fragColor, 0.0, 1.0);
    if (uAlphaTest != 0)
    {
        float a = floor(fragColor.a * 128.0 + 0.5), r = floor(uAlphaRef * 128.0 + 0.5);
        bool pass = uAlphaFunc == 1 || (uAlphaFunc == 2 && a < r) || (uAlphaFunc == 3 && a <= r) || (uAlphaFunc == 4 && a == r) ||
                    (uAlphaFunc == 5 && a >= r) || (uAlphaFunc == 6 && a > r) || (uAlphaFunc == 7 && a != r);
        if (!pass)
            discard;
    }
    if (uFog != 0)
        fragColor.rgb = mix(uFogColour, fragColor.rgb, clamp(uUseQ != 0 ? vFogScreen : vFog, 0.0, 1.0));
    if (uLevels != 0)
        fragColor.rgb = clamp(uLevelGain * max(vec3(0.0), min(fragColor.rgb, uLevelHigh) - uLevelLow), 0.0, 1.0);
    if (uOpaque != 0)
        fragColor.a = 1.0;
    if(uImageAlpha != 0)
        fragColor.a*=clamp(texture(uImageAlphaTex,gl_FragCoord.xy/uImageSize.xy).g*uImageSize.w,0.0,1.0);
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
            AdditiveByDestination,
            DestinationBySource,
            AlphaOnly,
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
            const bool alphaOnly=blend==FlatBlend::AlphaOnly;
            g.ColorMask(alphaOnly?gl::kFalse:gl::kTrue,alphaOnly?gl::kFalse:gl::kTrue,alphaOnly?gl::kFalse:gl::kTrue,gl::kTrue);
            g.Enable(gl::kBlend);
            g.BlendEquation(gl::kFuncAdd);
            switch (blend)
            {
            case FlatBlend::Alpha: g.BlendFunc(gl::kSrcAlpha, gl::kOneMinusSrcAlpha); break;
            case FlatBlend::Additive: g.BlendFunc(gl::kSrcAlpha, gl::kOne); break;
            case FlatBlend::AdditiveByDestination: g.BlendFunc(gl::kDstAlpha,gl::kOne); break;
            case FlatBlend::DestinationBySource: g.BlendFunc(gl::kOne,gl::kSrcAlpha); break;
            case FlatBlend::AlphaOnly: g.BlendFunc(gl::kOne,gl::kZero); break;
            case FlatBlend::Premultiplied: g.BlendFunc(gl::kOne, gl::kOneMinusSrcAlpha); break;
            case FlatBlend::Replace: g.BlendFunc(gl::kOne, gl::kZero); break;
            }
        }
    }

    struct Renderer::Gpu
    {
        gl::GLuint shader = 0;
        gl::GLint locM = -1, locScreen = -1, locDepthMax = -1, locUvMap = -1, locUvBounds = -1, locDirectColour = -1, locTex = -1, locTextured = -1, locTarget = -1,
                  locAlphaTest = -1, locAlphaFunc = -1, locUseQ = -1, locAlphaRef = -1, locOpaque = -1, locFog = -1, locTextureFunction = -1, locTextureAlpha = -1, locFogColour = -1,
                  locLevels = -1, locLevelGain = -1, locLevelHigh = -1, locLevelLow = -1,
                  locImageRadius=-1,locImageStep=-1,locImageKernel=-1,locImageDepth=-1,locImageDepthTex=-1,
                  locImageWeights=-1,locImageSize=-1,locImageAlpha=-1,locImageAlphaTex=-1,
                  locLodMode = -1, locLodScale = -1, locLodBias = -1;
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
        gl::GLuint imageDepth=0;
        int imageDepthWidth=0,imageDepthHeight=0;
        int targetWidth = 0, targetHeight = 0;
        bool targetFiltered = false; // bilinear (render scale other than 1) rather than point
        gl::GLuint targetCopy = 0;   // Material::sampleTarget: the target's colour, copied per batch
        int copyWidth = 0, copyHeight = 0;
        struct Target {
            gl::GLuint fbo = 0, colour = 0, depth = 0;
            int width = 0, height = 0;
        };
        Target auxiliary[8];
        gl::GLuint previousColour = 0;
        int previousWidth = 0, previousHeight = 0;
        uint64_t previousSerial = 0;
        bool auxiliaryTarget(unsigned n, int width, int height, bool shared) {
            const auto &g = gl::api();
            auto &t = auxiliary[n];
            if (!t.fbo) { g.GenFramebuffers(1,&t.fbo); g.GenRenderbuffers(1,&t.depth); }
            if (t.width != width || t.height != height) {
                if (t.colour) g.DeleteTextures(1,&t.colour);
                t.colour = makeTexture(nullptr,width,height,gl::kClampToEdge);
                g.BindRenderbuffer(gl::kRenderbuffer,t.depth);
                g.RenderbufferStorage(gl::kRenderbuffer,gl::kDepthComponent24,width,height);
                t.width=width; t.height=height;
            }
            g.BindFramebuffer(gl::kFramebuffer,t.fbo);
            g.FramebufferTexture2D(gl::kFramebuffer,gl::kColorAttachment0,gl::kTexture2D,t.colour,0);
            g.FramebufferRenderbuffer(gl::kFramebuffer,gl::kDepthAttachment,gl::kRenderbuffer,shared ? depth : t.depth);
            return g.CheckFramebufferStatus(gl::kFramebuffer)==gl::kFramebufferComplete;
        }
        struct GpuTexture
        {
            gl::GLuint id = 0;
            std::shared_ptr<const Texture> source; // held, so its address is not reused while compared
            int levels = 0;          // mip levels past 0 on the GPU
            bool generated = false;  // made from level 0 (the texture had none)
        };
        std::unordered_map<uint64_t, GpuTexture> textures;
        // The overlay's textures, their alpha from the GS's (0x80 is 1.0) to 255. By the texture's key when it has one
        // (a picture that changes: one slot, its image replaced), else its address.
        std::unordered_map<uint64_t, GpuTexture> overlayTextures;
        gl::GLuint displayPicture = 0; // the display's circuit 2 picture (bozo::render::displayPicture), its version
        uint64_t displayPictureVersion = 0;
        // The depth tiles read into a ring of pixel-pack buffers, each read fenced and taken at a later present once its
        // fence has passed, never waited for. (One buffer, mapped at the present after its read, waited there for the
        // GPU to finish that whole frame: in a 1P match the window thread blocked there ~70% of the time and the
        // presents fell to 15-45 a second while the game kept 60.)
        // A run of tiles along a tile row (tx0 to tx1), read as one rectangle: width x height depth words at offset.
        struct DepthRun
        {
            int ty, tx0, tx1, width, height;
            size_t offset;
        };
        struct DepthRead
        {
            gl::GLuint pbo = 0;
            size_t bytes = 0;
            gl::GLsync fence = nullptr;  // set while the read is in flight
            std::vector<DepthRun> runs;  // what the buffer holds, run by run
            size_t used = 0;             // its bytes
            int width = 0, height = 0;
            uint64_t serial = 0;         // which read it was (the newest completed one is taken)
        };
        static constexpr int kDepthReads = 3;
        DepthRead depthReads[kDepthReads];
        int depthNext = 0; // the slot the next read tries first
        uint64_t depthSerial = 0;

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
                slot.levels = 0;
                slot.generated = false;
                const gl::Api &g = gl::api();
                for (const std::vector<uint32_t> &level : source->levels)
                {
                    const int n = slot.levels + 1;
                    const int width = std::max(1, static_cast<int>(source->width >> n)), height = std::max(1, static_cast<int>(source->height >> n));
                    if (level.size() < static_cast<size_t>(width) * static_cast<size_t>(height))
                        break;
                    g.TexImage2D(gl::kTexture2D, n, static_cast<gl::GLint>(gl::kRgba8), width, height, 0, gl::kRgba, gl::kUnsignedByte, level.data());
                    slot.levels = n;
                    if (width == 1 && height == 1)
                        break;
                }
                g.TexParameteri(gl::kTexture2D, gl::kTextureMaxLevel, slot.levels);
            }
            return slot.id;
        }

        // The texture bound, its mip levels and filters as the material samples them: the uniforms that choose the level.
        void sampleLevels(uint64_t key, const Material &m)
        {
            const gl::Api &g = gl::api();
            const Material::Mipmap &mip = m.mipmap;
            GpuTexture &slot = textures[key];
            if (mip.levels == 0u)
            {
                g.TexParameteri(gl::kTexture2D, gl::kTextureMinFilter, m.nearest ? gl::kNearest : gl::kLinear);
                g.Uniform1i(locLodMode, 0);
                return;
            }
            if (slot.levels == 0 && !slot.generated && slot.source && slot.source->levels.empty())
            {
                g.TexParameteri(gl::kTexture2D, gl::kTextureMaxLevel, 1000);
                g.GenerateMipmap(gl::kTexture2D);
                uint32_t size = std::max(slot.source->width, slot.source->height);
                while (size > 1u)
                    size >>= 1, ++slot.levels;
                slot.generated = true;
            }
            g.TexParameteri(gl::kTexture2D, gl::kTextureMaxLevel, std::min<int>(mip.levels, slot.levels));
            static constexpr gl::GLenum kFilters[2][2] = {{gl::kNearestMipmapNearest, gl::kNearestMipmapLinear},
                                                         {gl::kLinearMipmapNearest, gl::kLinearMipmapLinear}};
            g.TexParameteri(gl::kTexture2D, gl::kTextureMinFilter, kFilters[mip.linearTexel][mip.linearLevels]);
            g.TexParameterf(gl::kTexture2D, gl::kTextureLodBias, mip.lod == Material::Mipmap::Lod::Screen ? mip.bias : 0.0f);
            g.Uniform1i(locLodMode, static_cast<int>(mip.lod));
            g.Uniform1f(locLodScale, static_cast<float>(1u << mip.shift));
            g.Uniform1f(locLodBias, mip.bias);
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
            locUvBounds = g.GetUniformLocation(shader, "uUvBounds");
            locTex = g.GetUniformLocation(shader, "uTex");
            locTextured = g.GetUniformLocation(shader, "uTextured");
            locDirectColour = g.GetUniformLocation(shader, "uDirectColour");
            locTarget = g.GetUniformLocation(shader, "uTarget");
            locAlphaTest = g.GetUniformLocation(shader, "uAlphaTest");
            locAlphaRef = g.GetUniformLocation(shader, "uAlphaRef");
            locAlphaFunc = g.GetUniformLocation(shader, "uAlphaFunc");
            locUseQ = g.GetUniformLocation(shader, "uUseQ");
            locOpaque = g.GetUniformLocation(shader, "uOpaque");
            locFog = g.GetUniformLocation(shader, "uFog");
            locFogColour = g.GetUniformLocation(shader, "uFogColour");
            locLevels = g.GetUniformLocation(shader, "uLevels");
            locLevelGain = g.GetUniformLocation(shader, "uLevelGain");
            locLevelHigh = g.GetUniformLocation(shader, "uLevelHigh");
            locLevelLow = g.GetUniformLocation(shader, "uLevelLow");
            locImageRadius=g.GetUniformLocation(shader,"uImageRadius");
            locImageStep=g.GetUniformLocation(shader,"uImageStep");
            locImageKernel=g.GetUniformLocation(shader,"uImageKernel[0]");
            locImageDepth=g.GetUniformLocation(shader,"uImageDepth");
            locImageDepthTex=g.GetUniformLocation(shader,"uImageDepthTex");
            locImageWeights=g.GetUniformLocation(shader,"uImageWeights[0]");
            locImageSize=g.GetUniformLocation(shader,"uImageSize");
            locImageAlpha=g.GetUniformLocation(shader,"uImageAlpha");
            locImageAlphaTex=g.GetUniformLocation(shader,"uImageAlphaTex");
            locTextureFunction = g.GetUniformLocation(shader, "uTextureFunction");
            locTextureAlpha = g.GetUniformLocation(shader, "uTextureAlpha");
            locLodMode = g.GetUniformLocation(shader, "uLodMode");
            locLodScale = g.GetUniformLocation(shader, "uLodScale");
            locLodBias = g.GetUniformLocation(shader, "uLodBias");
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
            for (auto &t : auxiliary) {
                if (t.colour) g.DeleteTextures(1,&t.colour);
                if (t.depth) g.DeleteRenderbuffers(1,&t.depth);
                if (t.fbo) g.DeleteFramebuffers(1,&t.fbo);
                t=Target{};
            }
            if (previousColour) g.DeleteTextures(1,&previousColour);
            previousColour=0;
            textures.clear();
            overlayTextures.clear();
            for (gl::GLuint *t : {&white, &colour, &targetCopy, &displayPicture})
                if (*t != 0)
                    g.DeleteTextures(1, t);
            if(imageDepth)g.DeleteTextures(1,&imageDepth);
            imageDepth=0;imageDepthWidth=imageDepthHeight=0;
            if (depth != 0)
                g.DeleteRenderbuffers(1, &depth);
            if (fbo != 0)
                g.DeleteFramebuffers(1, &fbo);
            for (gl::GLuint *b : {&vbo, &flatVbo})
                if (*b != 0)
                    g.DeleteBuffers(1, b);
            for (DepthRead &r : depthReads)
            {
                if (r.fence)
                    g.DeleteSync(r.fence);
                if (r.pbo != 0)
                    g.DeleteBuffers(1, &r.pbo);
                r = DepthRead{};
            }
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
            // Depth with stencil: the format the hardware keeps depth in, so readDepthTiles' reads of it
            // (DEPTH_STENCIL, UNSIGNED_INT_24_8) are plain copies. A 24-bit depth read as floats went through the
            // driver's conversion and waited for the GPU at every present.
            g.RenderbufferStorage(gl::kRenderbuffer, gl::kDepth24Stencil8, width, height);
            g.BindFramebuffer(gl::kFramebuffer, fbo);
            g.FramebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment0, gl::kTexture2D, colour, 0);
            g.FramebufferRenderbuffer(gl::kFramebuffer, gl::kDepthStencilAttachment, gl::kRenderbuffer, depth);
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

    namespace
    {
        // Interpolation's maths (moved from the fork's native_renderer.cpp). Matrices are columns (m[4c + r]), as
        // draw() takes them.
        void lerpMatrix(const float *a, const float *b, float t, float *out)
        {
            for (int i = 0; i < 16; ++i)
                out[i] = a[i] + (b[i] - a[i]) * t;
        }

        void multiply(const float *a, const float *b, float *out) // out = a x b
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        }

        // The upper 3 x 3 as a rotation (unit quaternion w, x, y, z) times a scale per column; false when it is not
        // one (a shear, a mirror, a degenerate scale): such a matrix is interpolated element by element.
        bool rotationOf(const float *m, float q[4], float scale[3])
        {
            float r[3][3]; // r[column][row]
            for (int c = 0; c < 3; ++c)
            {
                const float *col = m + c * 4;
                scale[c] = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
                if (!(scale[c] > 1e-6f))
                    return false;
                for (int k = 0; k < 3; ++k)
                    r[c][k] = col[k] / scale[c];
            }
            const auto dot = [&](int a, int b) { return r[a][0] * r[b][0] + r[a][1] * r[b][1] + r[a][2] * r[b][2]; };
            if (std::fabs(dot(0, 1)) > 0.02f || std::fabs(dot(0, 2)) > 0.02f || std::fabs(dot(1, 2)) > 0.02f)
                return false;
            const float det = r[2][0] * (r[0][1] * r[1][2] - r[0][2] * r[1][1]) + r[2][1] * (r[0][2] * r[1][0] - r[0][0] * r[1][2]) +
                              r[2][2] * (r[0][0] * r[1][1] - r[0][1] * r[1][0]);
            if (det < 0.9f)
                return false;
            const float m00 = r[0][0], m11 = r[1][1], m22 = r[2][2];
            const float trace = m00 + m11 + m22;
            if (trace > 0.0f)
            {
                const float s = std::sqrt(trace + 1.0f) * 2.0f;
                q[0] = 0.25f * s;
                q[1] = (r[1][2] - r[2][1]) / s;
                q[2] = (r[2][0] - r[0][2]) / s;
                q[3] = (r[0][1] - r[1][0]) / s;
            }
            else if (m00 > m11 && m00 > m22)
            {
                const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                q[0] = (r[1][2] - r[2][1]) / s;
                q[1] = 0.25f * s;
                q[2] = (r[1][0] + r[0][1]) / s;
                q[3] = (r[2][0] + r[0][2]) / s;
            }
            else if (m11 > m22)
            {
                const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                q[0] = (r[2][0] - r[0][2]) / s;
                q[1] = (r[1][0] + r[0][1]) / s;
                q[2] = 0.25f * s;
                q[3] = (r[2][1] + r[1][2]) / s;
            }
            else
            {
                const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                q[0] = (r[0][1] - r[1][0]) / s;
                q[1] = (r[2][0] + r[0][2]) / s;
                q[2] = (r[2][1] + r[1][2]) / s;
                q[3] = 0.25f * s;
            }
            return true;
        }

        // An object's matrix between a and b: its rotation along the arc (slerp), its scale and translation
        // linearly; element by element when either is not a rotation and scale.
        void interpolateObject(const float *a, const float *b, float t, float *out)
        {
            float qa[4], qb[4], sa[3], sb[3];
            if (!rotationOf(a, qa, sa) || !rotationOf(b, qb, sb))
            {
                lerpMatrix(a, b, t, out);
                return;
            }
            float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
            if (d < 0.0f)
            {
                d = -d;
                for (float &v : qb)
                    v = -v;
            }
            d = std::min(d, 1.0f);
            const float theta = std::acos(d);
            float q[4];
            if (theta < 1e-3f)
                for (int i = 0; i < 4; ++i)
                    q[i] = qa[i] + (qb[i] - qa[i]) * t;
            else
            {
                const float s = std::sin(theta), wa = std::sin((1.0f - t) * theta) / s, wb = std::sin(t * theta) / s;
                for (int i = 0; i < 4; ++i)
                    q[i] = qa[i] * wa + qb[i] * wb;
            }
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (float &v : q)
                v /= n;
            const float w = q[0], x = q[1], y = q[2], z = q[3];
            const float rot[3][3] = {// rot[column][row]
                                     {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
                                     {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
                                     {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}};
            lerpMatrix(a, b, t, out); // the translation and the fourth row, linearly
            for (int c = 0; c < 3; ++c)
            {
                const float scale = sa[c] + (sb[c] - sa[c]) * t;
                for (int k = 0; k < 3; ++k)
                    out[c * 4 + k] = rot[c][k] * scale;
            }
        }

        // The turn between two matrices' rotations (degrees; 0 when either is not a rotation and scale).
        float turnBetween(const float *a, const float *b)
        {
            float qa[4], qb[4], sa[3], sb[3];
            if (!rotationOf(a, qa, sa) || !rotationOf(b, qb, sb))
                return 0.0f;
            const float d = std::min(std::fabs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]), 1.0f);
            return std::acos(d) * 2.0f * 57.29578f;
        }

        // An affine matrix's inverse (its fourth row 0, 0, 0, 1); false when singular.
        bool invertAffine(const float *m, float *out)
        {
            const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
            const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
            if (std::fabs(det) < 1e-12f)
                return false;
            const float r = 1.0f / det;
            const float inv[9] = {(e * k - f * h) * r, (c * h - b * k) * r, (b * f - c * e) * r,
                                  (f * g - d * k) * r, (a * k - c * g) * r, (c * d - a * f) * r,
                                  (d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r}; // rows
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    out[col * 4 + row] = inv[row * 3 + col];
            for (int row = 0; row < 3; ++row)
                out[12 + row] = -(inv[row * 3] * m[12] + inv[row * 3 + 1] * m[13] + inv[row * 3 + 2] * m[14]);
            out[3] = out[7] = out[11] = 0.0f;
            out[15] = 1.0f;
            return true;
        }

        void transformPoint(const float *m, const float *p, float *out)
        {
            for (int r = 0; r < 3; ++r)
                out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
        }

        // Where a matrix puts the object's origin on the screen (GS coordinates); false behind the camera.
        bool originOnScreen(const float *m, float &x, float &y)
        {
            if (!(m[15] > 1e-3f))
                return false;
            x = m[12] / m[15];
            y = m[13] / m[15];
            return true;
        }

        // A view: the batches sharing a clip (a split screen's).
        uint64_t viewOf(const float clip[4])
        {
            uint64_t h = 1469598103934665603ull;
            uint32_t words[4];
            std::memcpy(words, clip, sizeof(words));
            for (uint32_t w : words)
                h = (h ^ w) * 1099511628211ull;
            return h;
        }
    }

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
            const FlatBlend want = m.blend == Material::Blend::Additive ? FlatBlend::Additive :
                m.blend==Material::Blend::AdditiveByDestination ? FlatBlend::AdditiveByDestination :
                m.blend==Material::Blend::DestinationBySource ? FlatBlend::DestinationBySource :
                m.blend==Material::Blend::AlphaOnly ? FlatBlend::AlphaOnly :
                m.blend==Material::Blend::Opaque ? FlatBlend::Replace : FlatBlend::Alpha;
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
        g.ColorMask(gl::kTrue,gl::kTrue,gl::kTrue,gl::kTrue);
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

    bool Renderer::pickInterpolated(std::shared_ptr<const Frame> &from, std::shared_ptr<const Frame> &to, float &alpha, double now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_history.empty())
            return false;
        // The window is a game frame (and the frames' recent lateness) behind the newest: the clock's time now lies
        // between two published frames, drawn that far between them.
        const double delay = m_interval + std::clamp(m_late, 0.0, 0.025) + 0.002;
        const double t = now - delay;
        const size_t n = m_history.size();
        alpha = 1.0f;
        from = nullptr;
        if (t >= m_history[n - 1]->pts)
        {
            to = m_history[n - 1];
            if (n >= 2 && to->continuous)
                from = m_history[n - 2];
            return true;
        }
        size_t i = n - 1;
        while (i > 0 && m_history[i - 1]->pts > t)
            --i;
        to = m_history[i];
        if (i == 0 || !to->continuous)
            return true;
        from = m_history[i - 1];
        const double span = to->pts - from->pts;
        alpha = span > 0.0 ? static_cast<float>(std::clamp((t - from->pts) / span, 0.0, 1.0)) : 1.0f;
        return true;
    }

    void Renderer::matchFrames(const Frame &from, const Frame &to)
    {
        Match &m = m_match;
        m.from = from.serial;
        m.to = to.serial;
        const size_t n = to.batches.size();
        m.previous.assign(n, -1);
        m.kind.assign(n, kUnmatched);
        m.cut.assign(n, 0u);
        m.cutViews.clear();
        // A teleport: the origin moved more than 30% of the screen's width, or, in the camera's space, by more than half
        // its distance (a respawn, the next of several alike objects). A cut: in a view (a split screen's: its clip),
        // 60% or more of the matched objects teleported or moved over 7.5% of the width (the camera cut away).
        const float jumpPixels = 0.3f * to.screen.width, cutPixels = 0.075f * to.screen.width;
        struct View
        {
            uint32_t matched = 0, moved = 0;
        };
        std::unordered_map<uint64_t, View> views;
        std::unordered_map<uint32_t, bool> used; // the older frame's objects (first batches) taken
        const auto translation = [](const Batch &batch, float t[3])
        {
            if (batch.hasParts)
                for (int k = 0; k < 3; ++k)
                    t[k] = batch.object[12 + k];
            else // the origin on the screen and its depth
            {
                const float w = std::fabs(batch.matrix[15]) > 1e-3f ? batch.matrix[15] : 1e-3f;
                t[0] = batch.matrix[12] / w;
                t[1] = batch.matrix[13] / w;
                t[2] = batch.matrix[15];
            }
        };
        // How a matched object is drawn between the two frames (teleport, spin or between), counted for its view.
        const auto classify = [&](size_t i, int32_t match)
        {
            const Batch &b = to.batches[i];
            used[static_cast<uint32_t>(match)] = true;
            const Batch &p = from.batches[static_cast<size_t>(match)];
            const bool parts = b.hasParts && p.hasParts;
            // The origin's move on the screen, where it says something: in front of the camera, not near its plane (a
            // large object's origin beside the camera projects anywhere).
            float moved = 0.0f, ax, ay, bx, by, step = 0.0f, distance = 0.0f;
            if (parts)
            {
                float ta[3], tb[3];
                translation(p, ta);
                translation(b, tb);
                step = std::hypot(ta[0] - tb[0], ta[1] - tb[1], ta[2] - tb[2]);
                distance = std::max(std::hypot(ta[0], ta[1], ta[2]), std::hypot(tb[0], tb[1], tb[2]));
            }
            const float nearPlane = parts ? 0.2f * distance : 1e-3f;
            if (p.matrix[15] > nearPlane && b.matrix[15] > nearPlane && originOnScreen(p.matrix, ax, ay) && originOnScreen(b.matrix, bx, by))
                moved = std::hypot(bx - ax, by - ay);
            // With the parts, a teleport is told in the camera's space (a step over half the distance); without, on the
            // screen.
            const bool teleport = parts ? step > 0.5f * distance + 1.0f : moved > jumpPixels;
            uint8_t kind = kLerp;
            if (parts && turnBetween(p.object, b.object) > 90.0f)
                kind = kSpin;
            if (teleport)
                kind = kTeleport;
            m.previous[i] = match;
            m.kind[i] = kind;
            View &view = views[viewOf(b.clip)];
            ++view.matched;
            view.moved += teleport || moved > cutPixels ? 1u : 0u;
        };
        // First the objects alone of their kind in their view (a split screen's: its clip): matched as they are. The
        // alike ones (the same model placed several times, no instance of its own) wait for the camera's move.
        std::vector<size_t> alike;
        for (size_t i = 0; i < n; ++i)
        {
            const Batch &b = to.batches[i];
            if (b.key == 0u)
                continue;
            if (const auto own = to.keys.find(b.key); own != to.keys.end() && own->second != i)
                continue; // the object's next batch (below)
            const auto group = from.bases.find(b.base);
            if (group == from.bases.end())
                continue; // new: where it is now
            const uint64_t view = viewOf(b.clip);
            int32_t only = -1;
            uint32_t count = 0;
            for (const uint32_t candidate : group->second)
                if (viewOf(from.batches[candidate].clip) == view)
                {
                    only = static_cast<int32_t>(candidate);
                    ++count;
                }
            if (count == 1u)
                classify(i, only);
            else if (count > 1u)
                alike.push_back(i);
        }
        // The camera's move in each view, from the objects alone of their kind: an object fixed in the world goes from
        // A to A' = D A in the camera's space (D: the camera's move); D taken from the object most others agree with
        // (the scenery), none when too few do (a cut, a view of moving cars).
        struct Move
        {
            bool known = false;
            float d[16];
        };
        std::unordered_map<uint64_t, Move> moves;
        if (!alike.empty())
        {
            std::unordered_map<uint64_t, std::vector<size_t>> unique; // view -> objects matched alone, between
            for (size_t i = 0; i < n; ++i)
                if (m.kind[i] == kLerp && to.batches[i].hasParts && from.batches[static_cast<size_t>(m.previous[i])].hasParts &&
                    to.keys.find(to.batches[i].key)->second == i)
                    unique[viewOf(to.batches[i].clip)].push_back(i);
            for (auto &[view, list] : unique)
            {
                if (list.size() < 4u)
                    continue;
                const size_t tries = std::min<size_t>(list.size(), 12u), checks = std::min<size_t>(list.size(), 40u);
                uint32_t bestScore = 0;
                Move &move = moves[view];
                for (size_t t = 0; t < tries; ++t)
                {
                    const size_t i = list[t * list.size() / tries];
                    const Batch &b = to.batches[i], &p = from.batches[static_cast<size_t>(m.previous[i])];
                    float inverse[16], d[16];
                    if (!invertAffine(p.object, inverse))
                        continue;
                    multiply(b.object, inverse, d);
                    uint32_t score = 0;
                    for (size_t c = 0; c < checks; ++c)
                    {
                        const size_t j = list[c * list.size() / checks];
                        const Batch &bj = to.batches[j], &pj = from.batches[static_cast<size_t>(m.previous[j])];
                        float predicted[3];
                        transformPoint(d, pj.object + 12, predicted);
                        const float error = std::hypot(predicted[0] - bj.object[12], predicted[1] - bj.object[13], predicted[2] - bj.object[14]);
                        score += error < 0.02f * std::hypot(bj.object[12], bj.object[13], bj.object[14]) + 0.5f ? 1u : 0u;
                    }
                    if (score > bestScore)
                    {
                        bestScore = score;
                        std::memcpy(move.d, d, sizeof(d));
                    }
                }
                move.known = bestScore >= 3u && bestScore * 2u >= checks;
            }
        }
        // The alike ones: the nearest of the older frame's in the same view, where it was either moved by the camera's
        // move (the scenery) or not (riding with the camera: a car's parts under a chase camera), whichever tells them
        // apart better. Ambiguous when another one lies within twice that distance (a row of alike objects passing
        // about one spacing a frame: the nearest is the wrong one, the row would seem to run backwards): drawn where it
        // is now, as an unmatched one.
        for (const size_t i : alike)
        {
            const Batch &b = to.batches[i];
            float tb[3];
            translation(b, tb);
            const uint64_t view = viewOf(b.clip);
            const auto group = from.bases.find(b.base);
            const auto found = moves.find(view);
            const Move *move = found != moves.end() && found->second.known && b.hasParts ? &found->second : nullptr;
            int32_t choice = -1;
            float choiceRatio = 0.0f;
            for (int way = 0; way < (move ? 2 : 1); ++way)
            {
                int32_t match = -1;
                float best = 0.0f, second = -1.0f;
                for (const uint32_t candidate : group->second)
                {
                    const Batch &c = from.batches[candidate];
                    if (viewOf(c.clip) != view)
                        continue;
                    float ta[3];
                    translation(c, ta);
                    if (way == 1 && c.hasParts)
                    {
                        float moved[3];
                        transformPoint(move->d, c.object + 12, moved);
                        std::memcpy(ta, moved, sizeof(ta));
                    }
                    const float d = std::hypot(ta[0] - tb[0], ta[1] - tb[1], ta[2] - tb[2]);
                    if (used.count(candidate))
                    {
                        if (second < 0.0f || d < second)
                            second = d;
                        continue;
                    }
                    if (match < 0 || d < best)
                    {
                        if (match >= 0 && (second < 0.0f || best < second))
                            second = best;
                        best = d;
                        match = static_cast<int32_t>(candidate);
                    }
                    else if (second < 0.0f || d < second)
                        second = d;
                }
                if (match < 0)
                    continue;
                const float ratio = second < 0.0f ? 1e30f : second / std::max(best, 1e-6f);
                if (ratio > choiceRatio)
                {
                    choiceRatio = ratio;
                    choice = match;
                }
            }
            if (choice < 0)
                continue; // new: where it is now
            if (choiceRatio < 2.0f)
            {
                used[static_cast<uint32_t>(choice)] = true;
                continue;
            }
            classify(i, choice);
        }
        // Each object's other batches as its first.
        for (size_t i = 0; i < n; ++i)
            if (to.batches[i].key != 0u)
                if (const auto own = to.keys.find(to.batches[i].key); own != to.keys.end() && own->second != i)
                {
                    m.previous[i] = m.previous[own->second];
                    m.kind[i] = m.kind[own->second];
                }
        for (const auto &[id, view] : views)
            if (view.matched >= 6u && view.moved * 10u >= view.matched * 6u)
                m.cutViews[id] = true;
        if (!m.cutViews.empty())
            for (size_t i = 0; i < n; ++i)
                m.cut[i] = m.cutViews.count(viewOf(to.batches[i].clip)) != 0u ? 1u : 0u;
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
        // Interpolating: the two frames the window's clock lies between (pickInterpolated), the newer drawn with its
        // keyed objects and tracked triangles that far back toward the older. Otherwise the newest, once.
        const double presentNow = secondsNow();
        const bool interpolate = interpolating();
        std::shared_ptr<const Frame> previous;
        float alpha = 1.0f;
        if (interpolate)
        {
            std::shared_ptr<const Frame> to;
            if (pickInterpolated(previous, to, alpha, presentNow))
                m_shown = std::move(to);
        }
        else if (published && published != m_shown && (!m_shown || published->time > m_shown->time))
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

        // Keep history stable when the same game frame is presented more than once.
        if (gpu.previousWidth != w || gpu.previousHeight != h) {
            if (gpu.previousColour) g.DeleteTextures(1,&gpu.previousColour);
            std::vector<uint32_t> black(size_t(w)*h,0);
            gpu.previousColour=gpu.makeTexture(black.data(),w,h,gl::kClampToEdge);
            gpu.previousWidth=w; gpu.previousHeight=h; gpu.previousSerial=frame->serial;
        } else if (gpu.previousSerial != frame->serial) {
            g.BindFramebuffer(gl::kFramebuffer,gpu.fbo);
            g.BindTexture(gl::kTexture2D,gpu.previousColour);
            g.CopyTexSubImage2D(gl::kTexture2D,0,0,0,0,0,w,h);
            gpu.previousSerial=frame->serial;
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
            g.VertexAttribPointer(4, 1, gl::kFloat, gl::kFalse, sizeof(Vertex), reinterpret_cast<const void *>(28));
            g.EnableVertexAttribArray(0);
            g.EnableVertexAttribArray(1);
            g.EnableVertexAttribArray(2);
            g.EnableVertexAttribArray(3);
            g.EnableVertexAttribArray(4);
        }
        if (bytes != 0u)
        {
            g.BindBuffer(gl::kArrayBuffer, gpu.vbo);
            g.BufferSubData(gl::kArrayBuffer, 0, static_cast<gl::GLsizeiptr>(bytes), frame->vertices.data());
        }

        // Interpolating: the keyed batches' matrices between the two frames, and the tracked triangles' corners.
        const bool lerping = interpolate && previous && alpha < 1.0f;
        struct Lerped
        {
            float matrix[16];
            bool use = false;
        };
        static thread_local std::vector<Lerped> s_lerped;
        s_lerped.assign(lerping ? frame->batches.size() : 0u, Lerped{});
        PresentInfo info;
        info.time = presentNow;
        info.from = previous ? previous->serial : 0u;
        info.to = frame->serial;
        info.alpha = lerping ? alpha : 1.0f;
        if (lerping)
        {
            if (m_match.from != previous->serial || m_match.to != frame->serial)
                matchFrames(*previous, *frame); // once per pair of frames
            const size_t n = frame->batches.size();
            size_t last = SIZE_MAX;
            for (size_t i = 0; i < n; ++i)
            {
                const Batch &b = frame->batches[i];
                if (b.key == 0u)
                    continue;
                ++info.keyedBatches;
                const int32_t from = m_match.previous[i];
                const uint8_t kind = m_match.kind[i];
                if (from < 0 || kind == kTeleport || m_match.cut[i])
                    continue;
                if (last != SIZE_MAX && frame->batches[last].key == b.key && m_match.previous[last] == from) // the object's next batch
                {
                    s_lerped[i] = s_lerped[last];
                    info.interpolatedBatches += s_lerped[i].use ? 1u : 0u;
                    continue;
                }
                last = i;
                const Batch &p = previous->batches[static_cast<size_t>(from)];
                if (b.hasParts && p.hasParts)
                {
                    float camera[16], object[16];
                    lerpMatrix(p.camera, b.camera, alpha, camera);
                    if (kind == kSpin)
                    {
                        std::memcpy(object, b.object, sizeof(object));
                        for (int k = 12; k < 16; ++k)
                            object[k] = p.object[k] + (b.object[k] - p.object[k]) * alpha;
                    }
                    else
                        interpolateObject(p.object, b.object, alpha, object);
                    multiply(camera, object, s_lerped[i].matrix);
                }
                else if (kind == kLerp)
                    lerpMatrix(p.matrix, b.matrix, alpha, s_lerped[i].matrix);
                else
                    continue;
                s_lerped[i].use = true;
                ++info.interpolatedBatches;
            }
            // Tracked triangles: each corner between where the older frame had it and where the newer has it.
            const float jumpPixels = 0.3f * frame->screen.width;
            for (size_t i = 0; i < n; ++i)
            {
                const Batch &b = frame->batches[i];
                if (b.idFirst < 0)
                    continue;
                const bool viewCut = m_match.cutViews.count(viewOf(b.clip)) != 0u;
                m_trackedScratch.assign(frame->vertices.begin() + b.first, frame->vertices.begin() + b.first + b.count);
                bool moved = false;
                for (uint32_t t = 0; t < b.count / 3u; ++t)
                {
                    const uint64_t id = frame->triangleIds[static_cast<size_t>(b.idFirst) + t];
                    if (id == 0u)
                        continue;
                    ++info.trackedTriangles;
                    const auto found = previous->tracked.find(id);
                    if (viewCut || found == previous->tracked.end() || found->second + 3u > previous->vertices.size())
                        continue;
                    const Vertex *pv = &previous->vertices[found->second];
                    Vertex *cv = &m_trackedScratch[3u * t];
                    const float dx = (cv[0].x + cv[1].x + cv[2].x - pv[0].x - pv[1].x - pv[2].x) / 3.0f;
                    const float dy = (cv[0].y + cv[1].y + cv[2].y - pv[0].y - pv[1].y - pv[2].y) / 3.0f;
                    if (std::hypot(dx, dy) > jumpPixels)
                        continue;
                    for (int k = 0; k < 3; ++k)
                    {
                        cv[k].x = pv[k].x + (cv[k].x - pv[k].x) * alpha;
                        cv[k].y = pv[k].y + (cv[k].y - pv[k].y) * alpha;
                        cv[k].z = pv[k].z + (cv[k].z - pv[k].z) * alpha;
                    }
                    moved = true;
                    ++info.trackedInterpolated;
                }
                if (moved)
                    g.BufferSubData(gl::kArrayBuffer, static_cast<gl::GLintptr>(b.first * sizeof(Vertex)),
                                    static_cast<gl::GLsizeiptr>(b.count * sizeof(Vertex)), m_trackedScratch.data());
            }
        }
        m_lastPresent = info;

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
        g.ColorMask(gl::kTrue,gl::kTrue,gl::kTrue,gl::kTrue);
        g.Clear(gl::kColorBufferBit | gl::kDepthBufferBit | gl::kStencilBufferBit);
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
        int drawWidth=w, drawHeight=h;
        const auto clipOf = [&](const Batch &batch)
        {
            Rect rect;
            const float *own = batch.clip;
            if (own[2] <= own[0])
                return rect;
            const float sx = static_cast<float>(drawWidth) / screen.width, sy = static_cast<float>(drawHeight) / screen.height;
            rect.x0 = std::clamp(static_cast<int>(std::lround((own[0] - screen.originX) * sx)), 0, drawWidth);
            rect.x1 = std::clamp(static_cast<int>(std::lround((own[2] - screen.originX) * sx)), 0, drawWidth);
            rect.y0 = std::clamp(static_cast<int>(std::lround(drawHeight - (own[3] - screen.originY) * sy)), 0, drawHeight);
            rect.y1 = std::clamp(static_cast<int>(std::lround(drawHeight - (own[1] - screen.originY) * sy)), 0, drawHeight);
            if (rect.x0 == 0 && rect.y0 == 0 && rect.x1 == drawWidth && rect.y1 == drawHeight)
                rect = Rect{}; // the whole target: no clip
            else if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                rect = Rect{0, 0, 0, -1}; // nothing shows
            return rect;
        };
        bool scissored = false;
        bool touched[8] = {};
        uint8_t drawingTarget = 0;
        for (size_t index = 0; index < frame->batches.size(); ++index)
        {
            const Batch &batch = frame->batches[index];
            const float *matrix = !s_lerped.empty() && s_lerped[index].use ? s_lerped[index].matrix : batch.matrix;
            const Material &m = batch.material;
            if (m.target >= 8 || m.source > Material::kPreviousFrame)
                throw std::runtime_error("native render target index outside supported range");
            const bool explicitResize=m.target && m.targetWidth &&
                (gpu.auxiliary[m.target].width!=m.targetWidth || gpu.auxiliary[m.target].height!=m.targetHeight);
            if (m.target != drawingTarget || explicitResize) {
                drawingTarget=m.target;
                drawWidth=w; drawHeight=h;
                if (drawingTarget) {
                    const uint32_t size=m_targetSize[drawingTarget].load();
                    const bool shared=(m_sharedDepth.load() & (1u << drawingTarget)) != 0;
                    if(shared && m.targetWidth && (m.targetWidth!=w || m.targetHeight!=h))
                        throw std::runtime_error("native shared depth requires matching image dimensions");
                    if (size && !shared) { drawWidth=size >> 16; drawHeight=size & 0xffff; }
                    if(m.targetWidth){drawWidth=m.targetWidth;drawHeight=m.targetHeight;}
                    if (!gpu.auxiliaryTarget(drawingTarget,drawWidth,drawHeight,shared))
                        throw std::runtime_error("native render target framebuffer incomplete");
                    if (!touched[drawingTarget] || explicitResize) {
                        g.Disable(gl::kScissorTest); scissored=false;
                        g.ColorMask(gl::kTrue,gl::kTrue,gl::kTrue,gl::kTrue);
                        g.DepthMask(gl::kTrue); g.ClearColor(0,0,0,0);
                        g.Clear(gl::kColorBufferBit | (shared ? 0 : gl::kDepthBufferBit));
                        touched[drawingTarget]=true;
                    }
                } else g.BindFramebuffer(gl::kFramebuffer,gpu.fbo);
                g.Viewport(0,0,drawWidth,drawHeight);
            }
            const Rect clip = clipOf(batch);
            if (clip.y1 < 0)
                continue;
            const bool alphaOnly=m.blend==Material::Blend::AlphaOnly;
            const auto channel = [&](uint32_t bit) { return (m.colourMask >> bit) & 1u ? gl::kTrue : gl::kFalse; };
            g.ColorMask(alphaOnly ? gl::kFalse : channel(0),alphaOnly ? gl::kFalse : channel(1),
                        alphaOnly ? gl::kFalse : channel(2),channel(3));
            if (m.gsBlend)
                applyGsBlend(g, m);
            else if (m.blend == Material::Blend::Opaque || alphaOnly)
                g.Disable(gl::kBlend);
            else
            {
                g.Enable(gl::kBlend);
                g.BlendFuncSeparate(m.blend==Material::Blend::DestinationBySource ? gl::kOne :
                                    m.blend==Material::Blend::AdditiveByDestination ? gl::kDstAlpha : gl::kSrcAlpha,
                                    m.blend==Material::Blend::DestinationBySource ? gl::kSrcAlpha :
                                    m.blend == Material::Blend::Alpha ? gl::kOneMinusSrcAlpha : gl::kOne, gl::kOne,
                                    gl::kOneMinusSrcAlpha);
                g.BlendEquation(gl::kFuncAdd);
            }
            g.DepthMask(m.depthWrite ? gl::kTrue : gl::kFalse);
            // The GS's depth test is GEQUAL (larger nearer): here LEQUAL on 1 - z, so what lies at the far end (a sky at
            // Z 0, a clear) passes against the cleared depth. OpenGL disables
            // writes as well when the test is disabled, so an independent
            // depthWrite request needs an enabled, unconditional test.
            if (m.depthTest || m.depthWrite)
            {
                g.Enable(gl::kDepthTest);
                g.DepthFunc(m.depthTest ? (m.depthGreater ? gl::kLess : gl::kLequal) : gl::kAlways);
            }
            else
                g.Disable(gl::kDepthTest);
            if (m.stencilClear)
                g.Clear(gl::kStencilBufferBit);
            if (m.stencilOp != Material::StencilOp::None)
            {
                g.Enable(gl::kStencilTest);
                g.StencilFunc(gl::kAlways, 0, 0xFFu);
                g.StencilOp(gl::kKeep, gl::kKeep, m.stencilOp == Material::StencilOp::IncrementWrap ? gl::kIncrWrap : gl::kDecrWrap);
            }
            else if (m.stencilNotZero)
            {
                g.Enable(gl::kStencilTest);
                g.StencilFunc(gl::kNotEqual, 0, 0xFFu);
                g.StencilOp(gl::kKeep, gl::kKeep, gl::kKeep);
            }
            else
                g.Disable(gl::kStencilTest);
            g.Uniform4fv(gpu.locM, 4, matrix);
            g.Uniform1i(gpu.locUseQ, m.useQ ? 1 : 0);
            g.Uniform1i(gpu.locAlphaFunc, static_cast<int>(m.alphaFunc));
            const std::shared_ptr<const Texture> &texture = batch.texture;
            const int textured = m.textured && texture ? 1 : 0, alphaTest = m.alphaTest ? 1 : 0;
            const int opaque = !m.gsBlend && m.blend == Material::Blend::Opaque ? 1 : 0, fog = m.fog ? 1 : 0;
            const float fogColour[3] = {static_cast<float>(m.fogColour & 0xffu) / 255.0f, static_cast<float>((m.fogColour >> 8) & 0xffu) / 255.0f,
                                        static_cast<float>((m.fogColour >> 16) & 0xffu) / 255.0f};
            g.Uniform1i(gpu.locOpaque, opaque);
            g.Uniform1i(gpu.locDirectColour, m.directColour ? 1 : 0);
            g.Uniform1i(gpu.locFog, fog);
            g.Uniform4fv(gpu.locUvBounds, 1, m.uvBounds);
            g.Uniform1i(gpu.locTextureFunction, static_cast<int>(m.textureFunction));
            g.Uniform1i(gpu.locTextureAlpha, m.textureAlpha ? 1 : 0);
            g.Uniform1i(gpu.locLodMode, 0);
            g.Uniform3fv(gpu.locFogColour, 1, fogColour);
            g.Uniform1i(gpu.locLevels, m.levels.enabled);
            g.Uniform3fv(gpu.locLevelGain, 1, m.levels.gain);
            g.Uniform3fv(gpu.locLevelHigh, 1, m.levels.high);
            g.Uniform3fv(gpu.locLevelLow, 1, m.levels.low);
            g.Uniform1i(gpu.locAlphaTest, alphaTest);
            g.Uniform1f(gpu.locAlphaRef, m.alphaRef / 128.0f);
            const auto *processing=m.imageProcessing.get();
            g.Uniform1i(gpu.locImageRadius,processing?processing->radius:0);
            g.Uniform1i(gpu.locImageDepth,processing && processing->depthLookup);
            g.Uniform1i(gpu.locImageAlpha,processing && processing->alphaSource);
            if(processing){
                float kernel[20]={};std::copy(processing->weights.begin(),processing->weights.end(),kernel);
                const float step[4]={processing->step[0],processing->step[1],0,0};
                const float dimensions[4]={float(drawWidth),float(drawHeight),processing->depthScale,processing->alphaScale};
                g.Uniform4fv(gpu.locImageKernel,5,kernel);g.Uniform4fv(gpu.locImageStep,1,step);
                g.Uniform4fv(gpu.locImageSize,1,dimensions);
                if(processing->depthLookup){
                    g.ActiveTexture(gl::kTexture0+1);
                    if(!gpu.imageDepth)g.GenTextures(1,&gpu.imageDepth);
                    g.BindTexture(gl::kTexture2D,gpu.imageDepth);
                    if(gpu.imageDepthWidth!=w || gpu.imageDepthHeight!=h){
                        g.TexImage2D(gl::kTexture2D,0,gl::kDepthComponent24,w,h,0,gl::kDepthComponent,gl::kUnsignedInt,nullptr);
                        gpu.imageDepthWidth=w;gpu.imageDepthHeight=h;
                    }
                    g.TexParameteri(gl::kTexture2D,gl::kTextureMinFilter,gl::kNearest);
                    g.TexParameteri(gl::kTexture2D,gl::kTextureMagFilter,gl::kNearest);
                    g.TexParameteri(gl::kTexture2D,gl::kTextureWrapS,gl::kClampToEdge);
                    g.TexParameteri(gl::kTexture2D,gl::kTextureWrapT,gl::kClampToEdge);
                    g.BindFramebuffer(gl::kFramebuffer,gpu.fbo);
                    g.CopyTexSubImage2D(gl::kTexture2D,0,0,0,0,0,w,h);
                    g.BindFramebuffer(gl::kFramebuffer,drawingTarget?gpu.auxiliary[drawingTarget].fbo:gpu.fbo);
                    g.Uniform1i(gpu.locImageDepthTex,1);
                    g.Uniform4fv(gpu.locImageWeights,64,processing->depthWeights.data());
                }
                if(processing->alphaSource){
                    const auto &alpha=gpu.auxiliary[processing->alphaSource];
                    if(!alpha.colour || !touched[processing->alphaSource])throw std::runtime_error("native alpha image has no producer this frame");
                    g.ActiveTexture(gl::kTexture0+2);g.BindTexture(gl::kTexture2D,alpha.colour);
                    g.TexParameteri(gl::kTexture2D,gl::kTextureMinFilter,gl::kLinear);
                    g.TexParameteri(gl::kTexture2D,gl::kTextureMagFilter,gl::kLinear);
                    g.Uniform1i(gpu.locImageAlphaTex,2);
                }
                g.ActiveTexture(gl::kTexture0);
            }
            if (m.fromFrame) {
                gl::GLuint source=0;
                if (m.source==Material::kPreviousFrame) source=gpu.previousColour;
                else if (m.source && m.source!=drawingTarget) {
                    auto &t=gpu.auxiliary[m.source];
                    const bool shared=(m_sharedDepth.load() & (1u << m.source)) != 0;
                    const uint32_t size=m_targetSize[m.source].load();
                    const int sw=touched[m.source] ? t.width : size && !shared ? int(size >> 16) : w;
                    const int sh=touched[m.source] ? t.height : size && !shared ? int(size & 0xffff) : h;
                    const bool fresh=!t.colour || t.width!=sw || t.height!=sh;
                    if (!gpu.auxiliaryTarget(m.source,sw,sh,shared))
                        throw std::runtime_error("native source framebuffer incomplete");
                    if (fresh) {
                        // A source may first be sampled by a clipped or alpha-only draw. Initialize its
                        // whole image independently of that draw's write masks, without clearing shared depth.
                        g.Disable(gl::kScissorTest);
                        g.ColorMask(gl::kTrue,gl::kTrue,gl::kTrue,gl::kTrue);
                        g.DepthMask(gl::kTrue);
                        g.ClearColor(0,0,0,0); g.Clear(gl::kColorBufferBit | (shared ? 0 : gl::kDepthBufferBit));
                        g.ColorMask(alphaOnly ? gl::kFalse : gl::kTrue,alphaOnly ? gl::kFalse : gl::kTrue,
                                    alphaOnly ? gl::kFalse : gl::kTrue,gl::kTrue);
                        g.DepthMask(m.depthWrite ? gl::kTrue : gl::kFalse);
                        if (scissored) g.Enable(gl::kScissorTest);
                    }
                    source=t.colour;
                } else {
                    const int sw=m.source ? drawWidth : w, sh=m.source ? drawHeight : h;
                    g.BindFramebuffer(gl::kFramebuffer,m.source ? gpu.auxiliary[m.source].fbo : gpu.fbo);
                    if (!gpu.targetCopy || gpu.copyWidth!=sw || gpu.copyHeight!=sh) {
                        if (gpu.targetCopy) g.DeleteTextures(1,&gpu.targetCopy);
                        gpu.targetCopy=gpu.makeTexture(nullptr,sw,sh,gl::kClampToEdge);
                        gpu.copyWidth=sw; gpu.copyHeight=sh;
                    }
                    source=gpu.targetCopy;
                    g.BindTexture(gl::kTexture2D,source);
                    g.CopyTexSubImage2D(gl::kTexture2D,0,0,0,0,0,sw,sh);
                }
                g.BindFramebuffer(gl::kFramebuffer,drawingTarget ? gpu.auxiliary[drawingTarget].fbo : gpu.fbo);
                g.BindTexture(gl::kTexture2D,source);
                g.TexParameteri(gl::kTexture2D,gl::kTextureMinFilter,m.nearest ? gl::kNearest : gl::kLinear);
                g.TexParameteri(gl::kTexture2D,gl::kTextureMagFilter,m.nearest ? gl::kNearest : gl::kLinear);
                if(m.source==0 && !m.nearest) {
                    g.GenerateMipmap(gl::kTexture2D);
                    g.TexParameteri(gl::kTexture2D,gl::kTextureMinFilter,gl::kLinearMipmapLinear);
                    g.TexParameterf(gl::kTexture2D,gl::kTextureLodBias,float(m.fromFrame));
                } else g.TexParameterf(gl::kTexture2D,gl::kTextureLodBias,0);
                const float uv[4]={1,0,-1,1};
                g.Uniform4fv(gpu.locUvMap,1,uv);
                g.Uniform1i(gpu.locTextured,1);
                g.Uniform1i(gpu.locTarget,m.source && m.source!=Material::kPreviousFrame ? 2 : 1);
            }
            else if (m.sampleTarget && sampleTargetEnabled())
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
                g.TexParameteri(gl::kTexture2D,gl::kTextureMinFilter,gl::kLinear);
                g.TexParameterf(gl::kTexture2D,gl::kTextureLodBias,0);
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
                    const uint64_t key = m.tex0 ^ (m.texa * 0x9e3779b97f4a7c15ull) ^ (m.region * 0xc2b2ae3d27d4eb4full);
                    const gl::GLuint id = gpu.texture(key, texture);
                    g.BindTexture(gl::kTexture2D, id);
                    gpu.sampleLevels(key, m);
                    g.TexParameteri(gl::kTexture2D,gl::kTextureMagFilter,m.nearest ? gl::kNearest : gl::kLinear);
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
        g.Disable(gl::kStencilTest);
        g.ColorMask(gl::kTrue,gl::kTrue,gl::kTrue,gl::kTrue);
        g.BindFramebuffer(gl::kFramebuffer,gpu.fbo);
        g.Viewport(0,0,w,h);
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

    // The depth tiles depthAt asked for in the last half second: read from the target (bound) into one of a ring of
    // pixel-pack buffers and fenced; the newest read whose fence has passed is taken at a later present (the next one
    // or the one after, as the GPU goes). Nothing here waits for the GPU: with every buffer still in flight, this
    // present reads nothing and depthAt keeps the tiles it has. The tiles are read by runs along their rows (one
    // ReadPixels a run, neighbours a gap of up to two tiles apart joined): a night arena's headlights want hundreds of
    // tiles a present, and a ReadPixels each (up to 1,400 a present) held the window thread for most of the frame.
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
        // The reads completed by now: the newest is taken, the older ones dropped.
        int done = -1;
        for (int i = 0; i < Gpu::kDepthReads; ++i)
        {
            Gpu::DepthRead &r = gpu.depthReads[i];
            if (!r.fence)
                continue;
            const gl::GLenum state = g.ClientWaitSync(r.fence, 0, 0);
            if (state != gl::kAlreadySignaled && state != gl::kConditionSatisfied)
                continue;
            g.DeleteSync(r.fence);
            r.fence = nullptr;
            if (r.width != w || r.height != h || r.runs.empty() || (done >= 0 && gpu.depthReads[done].serial > r.serial))
            {
                r.runs.clear();
                continue;
            }
            if (done >= 0)
                gpu.depthReads[done].runs.clear();
            done = i;
        }
        if (done >= 0)
        {
            Gpu::DepthRead &r = gpu.depthReads[done];
            std::unordered_map<uint64_t, std::vector<float>> depth;
            g.BindBuffer(gl::kPixelPackBuffer, r.pbo);
            const auto *mapped = static_cast<const uint32_t *>(
                g.MapBufferRange(gl::kPixelPackBuffer, 0, static_cast<gl::GLsizeiptr>(r.used), gl::kMapReadBit));
            if (mapped)
            {
                for (const Gpu::DepthRun &run : r.runs)
                {
                    const uint32_t *src = mapped + run.offset / sizeof(uint32_t);
                    for (int tx = run.tx0; tx <= run.tx1; ++tx)
                    {
                        const int x0 = (tx - run.tx0) * kDepthTile;
                        const int cw = std::min(kDepthTile, run.width - x0);
                        if (cw <= 0)
                            break;
                        std::vector<float> &tile = depth[static_cast<uint64_t>(run.ty) << 32 | static_cast<uint32_t>(tx)];
                        tile.assign(static_cast<size_t>(kDepthTile) * kDepthTile, 1.0f); // past the edge: far
                        for (int row = 0; row < run.height; ++row) // read bottom-up, kept top-down
                        {
                            // Depth in the top 24 bits (stencil below), as the window's [0, 1]: d / (2^24 - 1).
                            const uint32_t *in = src + static_cast<size_t>(row) * run.width + x0;
                            float *out = &tile[static_cast<size_t>(run.height - 1 - row) * kDepthTile];
                            for (int x = 0; x < cw; ++x)
                                out[x] = static_cast<float>(in[x] >> 8) / 16777215.0f;
                        }
                    }
                }
                g.UnmapBuffer(gl::kPixelPackBuffer);
            }
            g.BindBuffer(gl::kPixelPackBuffer, 0);
            r.runs.clear();
            std::lock_guard<std::mutex> lock(m_depthMutex);
            m_depthTiles.swap(depth);
        }
        if (tiles.empty())
            return;
        // A free slot; none: every read still in flight, nothing read at this present.
        int slot = -1;
        for (int k = 0; k < Gpu::kDepthReads && slot < 0; ++k)
        {
            const int i = (gpu.depthNext + k) % Gpu::kDepthReads;
            if (!gpu.depthReads[i].fence)
                slot = i;
        }
        if (slot < 0)
            return;
        gpu.depthNext = (slot + 1) % Gpu::kDepthReads;
        Gpu::DepthRead &r = gpu.depthReads[slot];
        r.runs.clear();
        // Runs along each tile row (keys sort by row, then column).
        std::sort(tiles.begin(), tiles.end());
        size_t bytes = 0;
        for (size_t i = 0; i < tiles.size();)
        {
            const int ty = static_cast<int>(tiles[i] >> 32), tx0 = static_cast<int>(tiles[i] & 0xffffffffu);
            int tx1 = tx0;
            size_t j = i + 1;
            while (j < tiles.size() && static_cast<int>(tiles[j] >> 32) == ty && static_cast<int>(tiles[j] & 0xffffffffu) - tx1 <= 3)
                tx1 = static_cast<int>(tiles[j++] & 0xffffffffu);
            i = j;
            const int x0 = tx0 * kDepthTile, top = ty * kDepthTile;
            const int cw = std::min((tx1 + 1) * kDepthTile, w) - x0, ch = std::min(kDepthTile, h - top);
            if (cw <= 0 || ch <= 0)
                continue;
            r.runs.push_back(Gpu::DepthRun{ty, tx0, tx1, cw, ch, bytes});
            bytes += static_cast<size_t>(cw) * ch * sizeof(uint32_t);
        }
        if (r.runs.empty())
            return;
        if (r.pbo == 0)
            g.GenBuffers(1, &r.pbo);
        g.BindBuffer(gl::kPixelPackBuffer, r.pbo);
        if (bytes > r.bytes)
        {
            r.bytes = std::max(bytes, r.bytes * 2u);
            g.BufferData(gl::kPixelPackBuffer, static_cast<gl::GLsizeiptr>(r.bytes), nullptr, gl::kStreamRead);
        }
        g.PixelStorei(gl::kPackAlignment, 4);
        for (const Gpu::DepthRun &run : r.runs)
            g.ReadPixels(run.tx0 * kDepthTile, h - (run.ty * kDepthTile + run.height), run.width, run.height, gl::kDepthStencil,
                         gl::kUnsignedInt248, reinterpret_cast<void *>(static_cast<uintptr_t>(run.offset)));
        g.BindBuffer(gl::kPixelPackBuffer, 0);
        r.used = bytes;
        r.width = w;
        r.height = h;
        r.serial = ++gpu.depthSerial;
        r.fence = g.FenceSync(gl::kSyncGpuCommandsComplete, 0);
    }
}
