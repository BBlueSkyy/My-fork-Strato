// SPDX-License-Identifier: MPL-2.0
#include <nce/guest.h>

static_assert(sizeof(skyline::nce::ThreadContext) == 0x2E0);
static_assert(skyline::constant::SkyTlsMagic == 0x534C54594B53ULL);
int main() {}
