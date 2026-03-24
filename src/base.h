#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <memory>
#include <deque>

typedef unsigned int OBJID;

template<typename T>
using Ref = std::shared_ptr<T>;

struct Position
{
    int x = 0;
    int y = 0;

    Position() = default;
    Position(int x, int y) : x(x), y(y) {}

    float DistanceTo(const Position& other) const {
        float dx = (float)(x - other.x);
        float dy = (float)(y - other.y);
        return sqrtf(dx * dx + dy * dy);
    }
};

struct Size
{
    int iWidth = 0;
    int iHeight = 0;
};

extern ULONG64 g_qwModuleBase;

