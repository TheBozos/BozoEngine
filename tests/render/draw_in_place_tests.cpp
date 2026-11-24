// SPDX-License-Identifier: MIT
// Draws assembled in place (Renderer::appendVertices, drawRange, continueFrame): the same frame as draw()'s copies, two
// batches over one range, a range past the vertices refused, and a refused draw() keeping none of its vertices.
#include "bozo/render/renderer.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace bozo::render;
int main()
{
    auto &r = Renderer::instance();
    r.setEnabled(true);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    unsigned failures = 0;
    Vertex tri[6]{};
    for (int i = 0; i < 6; ++i)
    {
        tri[i].x = float(i * 10);
        tri[i].y = float(i * 7);
        tri[i].rgba = 0x80808080u + i;
    }
    Material a, b;
    b.depthTest = true;

    // draw(): two batches, the second over the same three vertices again (a copy)
    r.beginFrame(Screen{});
    r.draw(a, identity, tri, 6);
    r.draw(b, identity, tri, 3);
    r.endFrame();
    const auto copied = r.takeFrameDraws().hash;

    // in place: the vertices appended before the frame starts (continueFrame keeps them), the same batches
    Vertex *at = r.appendVertices(6);
    std::memcpy(at, tri, sizeof(tri));
    at = r.appendVertices(3);
    std::memcpy(at, tri, 3 * sizeof(Vertex));
    if (r.builtVertices() != 9)
        ++failures;
    r.continueFrame(Screen{});
    if (r.builtVertices() != 9)
        ++failures;
    r.drawRange(a, identity, 0, 6, nullptr);
    r.drawRange(b, identity, 6, 3, nullptr);
    r.endFrame();
    if (r.takeFrameDraws().hash != copied)
        ++failures;

    // two batches over one range (a two-pass draw), and a range past the vertices refused
    r.appendVertices(3);
    r.continueFrame(Screen{});
    r.drawRange(a, identity, 0, 3, nullptr);
    r.drawRange(b, identity, 0, 3, nullptr);
    bool refused = false;
    try
    {
        r.drawRange(a, identity, 1, 3, nullptr);
    }
    catch (const std::invalid_argument &)
    {
        refused = true;
    }
    if (!refused)
        ++failures;
    r.endFrame();
    r.takeFrameDraws();
    if (r.published()->batches.size() != 2)
        ++failures;

    // beginFrame drops what was appended; a draw() refused keeps none of its vertices
    r.appendVertices(3);
    r.beginFrame(Screen{});
    if (r.builtVertices() != 0)
        ++failures;
    Material bad;
    bad.lines = true;
    refused = false;
    try
    {
        r.draw(bad, identity, tri, 3); // lines: an odd count
    }
    catch (const std::invalid_argument &)
    {
        refused = true;
    }
    if (!refused || r.builtVertices() != 0)
        ++failures;
    r.endFrame();
    std::printf("native draws in place: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
