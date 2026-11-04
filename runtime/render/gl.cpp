// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The OpenGL function table (bozo/render/gl.h).
#include "bozo/render/gl.h"

#include <string>

namespace bozo::render::gl
{
    namespace
    {
        Api g_api;
        bool g_loaded = false;
    }

    const Api &api() { return g_api; }
    bool loaded() { return g_loaded; }

    bool load(ProcLoader getProc, const char **missing)
    {
        Api table;
        const char *absent = nullptr;
#define BOZO_GL_LOAD(ret, name, args)                                                                                  \
    table.name = reinterpret_cast<ret(BOZO_GL_CALL *) args>(getProc("gl" #name));                                     \
    if (!table.name && !absent)                                                                                        \
        absent = "gl" #name;
        BOZO_GL_FUNCTIONS(BOZO_GL_LOAD)
#undef BOZO_GL_LOAD
        if (missing)
            *missing = absent;
        if (absent)
            return false;
        g_api = table;
        g_loaded = true;
        return true;
    }
}
