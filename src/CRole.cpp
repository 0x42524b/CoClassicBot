#include "CRole.h"
#include "game.h"

void CRole::SetCommand(CCommand* cmd)
{
    auto* vtable = *reinterpret_cast<GameCall::CRole_SetCommandFn* const*>(this);
    if (!vtable || !vtable[GameVtableIndex::CRole_SetCommand] || !cmd)
        return;

    vtable[GameVtableIndex::CRole_SetCommand](this, cmd);
}
