#pragma once
#include "base.h"

enum
{
    MAP_NONE = 0,
    MAP_TERRAIN = 1,
    MAP_TERRAIN_PART = 2,
    MAP_SCENE = 3,
    MAP_COVER = 4,
    MAP_ROLE = 5,
    MAP_HERO = 6,
    MAP_PLAYER = 7,
    MAP_PUZZLE = 8,
    MAP_3DSIMPLE = 9,
    MAP_3DEFFECT = 10,
    MAP_2DITEM = 11,
    MAP_3DNPC = 12,
    MAP_3DOBJ = 13,
    MAP_3DTRACE = 14,
    MAP_SOUND = 15,
    MAP_2DREGION = 16,
    MAP_3DMAGICMAPITEM = 17,
    MAP_3DITEM = 18,
    MAP_3DEFFECTNEW = 19,
};

// =====================================================================
// CMapObj — base class for all map objects
//
// In the game, CMapObj inherits std::enable_shared_from_this<CMapObj>
// which adds a weak_ptr at +0x08 (16 bytes). We pad over it here
// to avoid RAII issues with memory-overlaid classes.
// =====================================================================
#pragma pack(push, 1)
class CMapObj
{
public:
    virtual ~CMapObj() = default;

    virtual void Show(void* pInfo) {}
    virtual void Process(void* pInfo) {}
    virtual int  GetObjType() { return m_nType; }
    virtual void GetWorldPos(Position& posWorld) {}
    virtual void GetPos(Position& posCell) {}
    virtual void SetPos(Position posCell) {}
    virtual void GetBase(Size& infoSize) { infoSize.iWidth = 1; infoSize.iHeight = 1; }
    virtual void SetBase(Size infoSize) {}
    virtual BOOL IsFocus() { return false; }

protected:
    BYTE _pad08[0x10];     // +0x08: weak_ptr from enable_shared_from_this

public:
    int   m_nType;          // +0x18
    DWORD m_dwARGB;         // +0x1C
};
#pragma pack(pop)

static_assert(sizeof(CMapObj) == 0x20, "CMapObj size mismatch");
