// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The native renderer's game-thread side (bozo/render/renderer.h), moved from the fork's native_renderer.cpp: frames,
// batches, the overlay and the backdrop, published for the platform layer. Milestone M0 has no GPU: published lists are
// recorded and hashed as the reference harness does (the `draws` field: TMB_Recomp docs/REFERENCE_HARNESS.md).
#include "bozo/render/renderer.h"

#include "bozo/hash.h"

#include <cstring>

namespace bozo::render
{
    namespace
    {
        // A texture's identity in the draw-list hash: XXH64 of its RGBA texels seeded with XXH64 of {width, height}
        // (0 for none), remembered per texture object while it lives.
        uint64_t textureIdentity(const std::shared_ptr<const Texture> &texture)
        {
            if (!texture)
                return 0u;
            struct Known
            {
                std::weak_ptr<const Texture> texture;
                uint64_t hash = 0;
            };
            static std::mutex s_mutex;
            static std::unordered_map<const Texture *, Known> s_known;
            std::lock_guard<std::mutex> lock(s_mutex);
            Known &known = s_known[texture.get()];
            if (known.texture.expired() || known.texture.lock() != texture)
            {
                const uint32_t size[2] = {texture->width, texture->height};
                known.texture = texture;
                known.hash = hash::xxh64(texture->rgba.data(), texture->rgba.size() * sizeof(uint32_t),
                                         hash::xxh64(size, sizeof(size), 0));
                if (s_known.size() > 65536u) // a long run: start again rather than grow
                {
                    const uint64_t h = known.hash;
                    s_known.clear();
                    s_known[texture.get()] = Known{texture, h};
                    return h;
                }
            }
            return known.hash;
        }

        constexpr float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    }

    Material::Blend blendOf(uint64_t alpha)
    {
        const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
        if (a == b && d == 0u)
            return Material::Blend::Opaque;
        if (a == 0u && b == 2u && d == 1u)
            return c == 1u ? Material::Blend::AdditiveByDestination : Material::Blend::Additive;
        return Material::Blend::Alpha;
    }

    Renderer::Renderer() = default;
    Renderer::~Renderer() = default;

    Renderer &Renderer::instance()
    {
        static Renderer renderer;
        return renderer;
    }

    void Renderer::noteDrawList(const Frame &frame, uint64_t kind)
    {
        static_assert(sizeof(Vertex) == 32u, "Vertex: eight 4-byte fields, no padding");
        uint64_t h = hash::xxh64(&kind, sizeof(kind), 0);
        for (const Batch &batch : frame.batches)
        {
            const Material &m = batch.material;
            const uint64_t words[8] = {m.tex0, m.texa, m.region,
                                       static_cast<uint64_t>(m.textured) | static_cast<uint64_t>(m.blend) << 8 |
                                           static_cast<uint64_t>(m.alphaRef) << 16 | static_cast<uint64_t>(m.alphaTest) << 24 |
                                           static_cast<uint64_t>(m.depthTest) << 25 | static_cast<uint64_t>(m.depthWrite) << 26 |
                                           static_cast<uint64_t>(m.clampU) << 27 | static_cast<uint64_t>(m.clampV) << 28 |
                                           static_cast<uint64_t>(m.fog) << 29 | static_cast<uint64_t>(m.sampleTarget) << 30 |
                                           static_cast<uint64_t>(m.fromGsContext) << 31 | static_cast<uint64_t>(m.alphaBlend) << 32 |
                                           static_cast<uint64_t>(m.context) << 40,
                                       m.fogColour, textureIdentity(batch.texture), batch.count, 0u};
            h = hash::xxh64(words, sizeof(words), h);
            h = hash::xxh64(batch.matrix, sizeof(batch.matrix), h);
            h = hash::xxh64(batch.clip, sizeof(batch.clip), h);
            if (batch.first + static_cast<size_t>(batch.count) <= frame.vertices.size())
                h = hash::xxh64(frame.vertices.data() + batch.first, batch.count * sizeof(Vertex), h);
        }
        const uint64_t fold[2] = {m_draws.hash, h};
        m_draws.hash = hash::xxh64(fold, sizeof(fold), 0);
        m_draws.batches += static_cast<uint32_t>(frame.batches.size());
        m_draws.vertices += static_cast<uint32_t>(frame.vertices.size());
        ++m_draws.lists;
    }

    FrameDraws Renderer::takeFrameDraws()
    {
        const FrameDraws d = m_draws;
        m_draws = FrameDraws{};
        return d;
    }

    void Renderer::beginFrame(const Screen &screen) { startBuilding(screen); }

    void Renderer::startBuilding(const Screen &screen)
    {
        std::vector<Vertex> vertices = std::move(m_building.vertices);
        std::vector<Batch> batches = std::move(m_building.batches);
        vertices.clear();
        batches.clear();
        m_building = Frame{};
        m_building.screen = screen;
        m_building.vertices = std::move(vertices);
        m_building.batches = std::move(batches);
    }

    void Renderer::draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count)
    {
        draw(material, matrix, vertices, count, nullptr);
    }

    void Renderer::draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count,
                        std::shared_ptr<const Texture> texture)
    {
        if (count == 0u)
            return;
        Batch batch;
        batch.material = material;
        std::memcpy(batch.matrix, matrix, sizeof(batch.matrix));
        batch.texture = std::move(texture);
        batch.first = static_cast<uint32_t>(m_building.vertices.size());
        batch.count = count;
        batch.key = m_key;
        batch.base = m_key != 0u ? m_keyBase : 0u;
        if (m_key != 0u && m_hasParts)
        {
            batch.hasParts = true;
            std::memcpy(batch.camera, m_camera, sizeof(batch.camera));
            std::memcpy(batch.object, m_object, sizeof(batch.object));
        }
        std::memcpy(batch.clip, m_clip, sizeof(batch.clip));
        batch.group = m_streamDraws ? m_group : 0u;
        m_building.vertices.insert(m_building.vertices.end(), vertices, vertices + count);
        m_building.batches.push_back(batch);
    }

    void Renderer::drawTracked(const Material &material, const Vertex *vertices, uint32_t count, std::shared_ptr<const Texture> texture,
                               const uint64_t *ids)
    {
        const size_t before = m_building.batches.size();
        const uint64_t key = m_key;
        const bool parts = m_hasParts;
        m_key = 0; // screen space: no matrix to interpolate
        m_hasParts = false;
        draw(material, kScreenSpace, vertices, count, std::move(texture));
        m_key = key;
        m_hasParts = parts;
        if (ids && m_building.batches.size() == before + 1u)
        {
            Batch &batch = m_building.batches.back();
            batch.idFirst = static_cast<int32_t>(m_building.triangleIds.size());
            m_building.triangleIds.insert(m_building.triangleIds.end(), ids, ids + count / 3u);
        }
    }

    void Renderer::drawOverlay(const Material &material, const Vertex *vertices, uint32_t count, std::shared_ptr<const Texture> texture)
    {
        if (count == 0u || m_overlayBuilding.vertices.size() > (1u << 20))
            return; // (never published: a port that does not publish keeps the latest million vertices' worth out)
        Batch batch;
        batch.material = material;
        batch.material.depthTest = false;
        batch.material.depthWrite = false;
        std::memcpy(batch.matrix, kScreenSpace, sizeof(batch.matrix));
        batch.texture = std::move(texture);
        batch.first = static_cast<uint32_t>(m_overlayBuilding.vertices.size());
        batch.count = count;
        m_overlayBuilding.vertices.insert(m_overlayBuilding.vertices.end(), vertices, vertices + count);
        m_overlayBuilding.batches.push_back(batch);
    }

    void Renderer::publishOverlay() { publishOverlay(m_building.screen); }

    void Renderer::publishOverlay(const Screen &screen)
    {
        noteDrawList(m_overlayBuilding, 2u);
        auto overlay = std::make_shared<Frame>(std::move(m_overlayBuilding));
        m_overlayBuilding = Frame{};
        overlay->screen = screen;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_overlayPublished = std::move(overlay);
    }

    void Renderer::setBackdrop(std::shared_ptr<const Texture> picture, const Screen &screen, float x, float y, float width,
                               float height, bool)
    {
        auto frame = std::make_shared<Frame>();
        frame->screen = screen;
        Batch batch;
        batch.material.textured = true;
        batch.material.blend = Material::Blend::Opaque;
        batch.material.depthTest = false;
        batch.material.depthWrite = false;
        std::memcpy(batch.matrix, kScreenSpace, sizeof(batch.matrix));
        batch.texture = std::move(picture);
        batch.first = 0;
        batch.count = 6;
        const float xs[2] = {x, x + width}, ys[2] = {y, y + height};
        Vertex corners[4];
        for (int i = 0; i < 4; ++i)
        {
            corners[i].x = xs[i & 1];
            corners[i].y = ys[i >> 1];
            corners[i].u = static_cast<float>(i & 1);
            corners[i].v = static_cast<float>(i >> 1);
            corners[i].rgba = 0x80808080u;
            corners[i].fog = 1.0f;
        }
        frame->vertices = {corners[0], corners[1], corners[2], corners[1], corners[3], corners[2]};
        frame->batches.push_back(batch);
        noteDrawList(*frame, 3u); // a movie's picture, by its content
        std::lock_guard<std::mutex> lock(m_mutex);
        m_backdrop = std::move(frame);
        ++m_publishes;
    }

    void Renderer::clearBackdrop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_backdrop.reset();
    }

    void Renderer::endFrame()
    {
        noteDrawList(m_building, 1u);
        auto frame = std::make_shared<Frame>(std::move(m_building));
        startBuilding(Screen{});
        m_key = 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        frame->serial = ++m_serial;
        m_published = std::move(frame);
        ++m_publishes;
    }

    void Renderer::wantDepth() {}

    bool Renderer::depthAt(float, float, uint32_t &) { return false; }

    std::shared_ptr<const Renderer::Frame> Renderer::published() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_published;
    }

    std::shared_ptr<const Renderer::Frame> Renderer::publishedOverlay() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_overlayPublished;
    }

    std::shared_ptr<const Renderer::Frame> Renderer::backdrop() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_backdrop;
    }

    uint64_t Renderer::publishes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_publishes;
    }
}
