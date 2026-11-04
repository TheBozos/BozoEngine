// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// Native PC rendering: a port's adapter reads its game's own 3D and 2D data (meshes, matrices, materials, its HUD and
// menus) and submits it here; the platform layer draws the published frames with the PC GPU at the window's
// resolution. Moved from the fork's native_renderer.h (phase4-inventory.md section 1.4); the GS-stream drawing (groups
// drawn inside the GS front end's stream, GS-context batches, depth probes in GS targets) stayed behind with the
// emulation.
//
// Game thread: beginFrame(), draw(...) per mesh, endFrame() publishes the frame; drawOverlay(...) and publishOverlay()
// the 2D; setBackdrop() a movie's picture. Milestone M0 (phase4-inventory.md section 5) has no GPU: the published lists
// are recorded and folded into the per-frame draw-list hash of the reference record (drawsOfFrame); M1 brings the
// present thread on SDL3 and OpenGL 3.3.
#pragma once

#include "bozo/render/texture.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bozo::render
{
    struct Vertex
    {
        float x = 0, y = 0, z = 0; // the object's space: the draw's matrix takes it to PS2 screen space
        float u = 0, v = 0;        // texture coordinates, 0..1 across the texture (the GS's S / Q, T / Q)
        uint32_t rgba = 0;         // lit colour, R in the low byte; 0x80 per channel is 1.0, as the GS modulates
        float fog = 1.0f;          // the GS's F / 255: 1 keeps the colour, 0 is all fog colour (Material::fog)
        float q = 1.0f;            // the GS's Q (U / V across the triangle in perspective)
    };

    // How a draw blends and tests, from the GS registers a game sets (ALPHA, TEST) reduced to what a PC GPU does.
    struct Material
    {
        uint64_t tex0 = 0; // keys the texture's GPU copy (a port gives each asset its own value)
        uint64_t texa = 0;
        bool textured = true;
        enum class Blend : uint8_t
        {
            Opaque,                // no blending
            Alpha,                 // (Cs - Cd) * As + Cd
            Additive,              // Cs * As + Cd
            AdditiveByDestination, // Cs * Ad + Cd, Ad the target's alpha (0x80 is 1.0)
        } blend = Blend::Alpha;
        uint8_t alphaRef = 0; // texels at or below are discarded when alphaTest
        bool alphaTest = false;
        bool depthTest = true; // GS depth: larger is nearer (GEQUAL)
        bool depthWrite = true;
        bool clampU = false, clampV = false; // CLAMP's WMS / WMT: the texture's edge past 0..1 (else it repeats)
        uint64_t region = 0;                 // CLAMP, for a REGION_REPEAT window
        bool fog = false;                    // PRIM.FGE: the colour goes toward fogColour by 1 - Vertex::fog
        // The texture is the target being drawn into, as it is when this batch is drawn (a game's screen distortion:
        // heat haze sampling the frame); u, v in the target's GS pixels.
        bool sampleTarget = false;
        uint32_t fogColour = 0; // FOGCOL, R in the low byte
        // Kept for the draw-list hash's layout (the reference's GS-context batches); a native port never sets it.
        bool fromGsContext = false;
        bool alphaBlend = true;
        uint8_t context = 0;
    };

    // The blend of a draw with PRIM.ABE set, from the GS's ALPHA register.
    Material::Blend blendOf(uint64_t alpha);

    // PS2 screen space as the game draws to it: the screen point at the window's top-left corner and the size that
    // fills the window (the GS XYOFFSET and the frame size).
    struct Screen
    {
        float originX = 2048.0f - 320.0f, originY = 2048.0f - 256.0f;
        float width = 640.0f, height = 512.0f;
        float depthMax = 16777215.0f; // the Z buffer's range (24-bit)
        float gsZScale = 1.0f;        // GS Z per unit of the matrices' z / w
    };

    // The draw lists published during one game frame, folded as the reference record's `draws` field: per list (kind
    // 1 the 3D frame, 2 the 2D overlay, 3 a movie picture) h = XXH64 over the list, then d = XXH64({d, h}).
    struct FrameDraws
    {
        uint64_t hash = 0;
        uint32_t batches = 0, vertices = 0, lists = 0;
    };

    class Renderer
    {
    public:
        static Renderer &instance();

        // ---- Game thread ----
        void beginFrame(const Screen &screen);
        // Triangles (three vertices each) with an object-to-screen matrix: column c is matrix[4c..4c+3], screen point =
        // M * (x, y, z, 1), then divided by w (as a VU's projection).
        void draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count);
        // The same with the texture handed over, decoded by the port from its game's own data.
        void draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count,
                  std::shared_ptr<const Texture> texture);
        // A port's 2D drawn natively: screen-space triangles (the GS's primitive coordinates over 16) over the frame, in
        // submission order, without depth; publishOverlay at the game frame's end shows them until the next publish.
        void drawOverlay(const Material &material, const Vertex *vertices, uint32_t count, std::shared_ptr<const Texture> texture);
        void publishOverlay();
        // The overlay's GS coordinates mapped to the window by this screen instead of the frame being built's.
        void publishOverlay(const Screen &screen);
        // A picture under the overlay (a movie frame), at (x, y, width, height) in the screen's GS coordinates; held:
        // until clearBackdrop or the next one.
        void setBackdrop(std::shared_ptr<const Texture> picture, const Screen &screen, float x, float y, float width,
                         float height, bool held = false);
        void clearBackdrop();
        void endFrame();
        // Depth a game reads back (TMB's lens flares): asked for with wantDepth, given at a GS screen point as GS Z from
        // the frame presented last. False while no GPU presents (M0).
        void wantDepth();
        bool depthAt(float gsX, float gsY, uint32_t &gsZ);
        // The frame being built's GS Z per unit of z / w.
        float gsZScale() const { return m_building.screen.gsZScale; }
        // A port's numbering of its draw groups (kept: the platform layer may sort by it).
        void setStreamDraws(bool on) { m_streamDraws = on; }
        void beginGroup(uint64_t group) { m_group = group; }
        // The port's frame is the whole picture (the window shows only what is presented: its frame, a movie's
        // backdrop, the 2D).
        void setPresentsAlone(bool on) { m_presentsAlone = on; }
        bool presentsAlone() const { return m_enabled && m_presentsAlone; }
        // The rectangle the next draws are clipped to, in GS primitive coordinates, until set again; clearClip: none.
        void setClip(float x0, float y0, float x1, float y1)
        {
            m_clip[0] = x0;
            m_clip[1] = y0;
            m_clip[2] = x1;
            m_clip[3] = y1;
        }
        void clearClip() { setClip(0, 0, 0, 0); }
        // What the frame starts from (the game's clear: a background or fog colour, R in the low byte).
        void setClearColour(uint32_t rgb) { m_clearColour.store(rgb, std::memory_order_relaxed); }
        uint32_t clearColour() const { return m_clearColour.load(std::memory_order_relaxed); }
        // Frames faster than the game's: each object's batches keyed by something stable from one game frame to the
        // next (setKey), drawn between the two frames' matrices. base: what the object is without telling alike ones
        // apart (0: the key alone). setKey also ends the parts setParts gave.
        void setKey(uint64_t key, uint64_t base = 0)
        {
            m_key = key;
            m_keyBase = base != 0u ? base : key;
            m_hasParts = false;
        }
        // The keyed batches that follow are camera x object (columns, as draw()'s matrix, which must be their product).
        void setParts(const float camera[16], const float object[16])
        {
            std::copy(camera, camera + 16, m_camera);
            std::copy(object, object + 16, m_object);
            m_hasParts = true;
        }
        // Screen-space triangles whose vertices move from frame to frame: triangle i is ids[i] (0: none).
        void drawTracked(const Material &material, const Vertex *vertices, uint32_t count, std::shared_ptr<const Texture> texture,
                         const uint64_t *ids);
        // The window's frame rate: 0 the game's (the default), -1 the monitor's, n > 0 that many a second.
        void setFrameRate(int rate) { m_frameRate.store(rate, std::memory_order_relaxed); }
        int frameRate() const { return m_frameRate.load(std::memory_order_relaxed); }
        bool interpolating() const { return frameRate() != 0 && m_presentsAlone && m_enabled; }
        bool enabled() const { return m_enabled; }
        void setEnabled(bool on) { m_enabled = on; }

        // ---- What a frame is ----
        struct Batch
        {
            Material material;
            float matrix[16] = {};
            uint32_t first = 0, count = 0;
            uint64_t group = 0;
            uint64_t key = 0;
            uint64_t base = 0;
            bool hasParts = false;
            float camera[16] = {}, object[16] = {};
            int32_t idFirst = -1; // drawTracked: its triangles' ids from Frame::triangleIds[idFirst]
            float clip[4] = {0, 0, 0, 0}; // setClip's (x0, y0, x1, y1); x1 <= x0: none
            std::shared_ptr<const Texture> texture;
        };
        struct Frame
        {
            Screen screen;
            std::vector<Vertex> vertices;
            std::vector<Batch> batches;
            std::vector<uint64_t> triangleIds;
            uint64_t serial = 0;
        };

        // ---- The published frames (the platform layer's side; M0: recorded) ----
        // The latest published 3D frame, overlay and backdrop (null: none yet).
        std::shared_ptr<const Frame> published() const;
        std::shared_ptr<const Frame> publishedOverlay() const;
        std::shared_ptr<const Frame> backdrop() const;
        uint64_t publishes() const;
        // The draw lists published since the last call, folded (the reference record's `draws`), and restarted: the
        // host takes it at each game frame's end.
        FrameDraws takeFrameDraws();

    private:
        Renderer();
        ~Renderer();
        void startBuilding(const Screen &screen);
        void noteDrawList(const Frame &frame, uint64_t kind);

        Frame m_building;
        Frame m_overlayBuilding;
        bool m_enabled = false;
        bool m_presentsAlone = false;
        bool m_streamDraws = false;
        uint64_t m_group = 0;
        float m_clip[4] = {0, 0, 0, 0};
        std::atomic<uint32_t> m_clearColour{0};
        std::atomic<int> m_frameRate{0};
        uint64_t m_key = 0, m_keyBase = 0;
        bool m_hasParts = false;
        float m_camera[16] = {}, m_object[16] = {};
        mutable std::mutex m_mutex;
        std::shared_ptr<const Frame> m_published, m_overlayPublished, m_backdrop;
        uint64_t m_publishes = 0, m_serial = 0;
        FrameDraws m_draws;
    };
}
