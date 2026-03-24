#pragma once
#include "base.h"

namespace Gfx
{
    void Init(uintptr_t moduleBase);

    void FillRect(int x1, int y1, int x2, int y2, uint32_t colorARGB);
    void DrawCross(int cx, int cy, int size, int thickness, uint32_t colorARGB);
}
