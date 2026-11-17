// M43A: rec/ isolation shim — the recording layer only ever sees the nx2android ABI copies in nvn_vk2/abi/,
// never third_party/d2r_gfx/ (the op enums diverged: OP_COUNT_ 50 vs 43).
#pragma once
#include "../abi/nvn_hmap.h"
