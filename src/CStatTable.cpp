#include "CStatTable.h"
#include "game.h"

int CStatTable::GetValue(int statType) const
{
    return GameCall::CStatTable_GetValue()(this, statType);
}
