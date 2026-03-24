#pragma once
#include "base.h"

class CRole;

// Find the NPC closest to expectedPos whose name matches (case-insensitive).
// Returns the closest NPC within radius tiles, or nullptr if none found.
// Skips players and monsters; only considers NPC-type entities.
CRole* FindNpcByName(const char* name, const Position& expectedPos, int radius);
