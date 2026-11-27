// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/engine/FrameGenerationSelection.h"
#include <iostream>

int main() {
    using namespace veyra::engine;
    using F = FrameGenerationBackend;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    // Exhaust every packaging combination: a usable saved choice survives,
    // and a standard fallback is never overlooked (including an AMF-only build).
    for (unsigned mask = 0; mask < 128; ++mask) {
        const FrameGenerationAvailability a{bool(mask & 1), bool(mask & 2), bool(mask & 4),
            bool(mask & 8), bool(mask & 16), bool(mask & 32), bool(mask & 64)};
        for (unsigned saved = 0; saved < 7; ++saved) {
            const auto previous = F(saved), selected = restoredFrameGenerationBackend(previous, a);
            if (a.supports(previous)) check(selected == previous);
            else if (a.dlss || a.xess || a.fsr || a.fsr4 || a.amdFrc) check(a.supports(selected));
            else check(selected == previous); // VFG/ANR remain explicit opt-ins.
        }
    }
    FrameGenerationAvailability amf; amf.amdFrc = true;
    check(restoredFrameGenerationBackend(F::Fsr4, amf) == F::AmdFrc);
    check(twoXFrameGeneration(restoredFrameGenerationBackend(F::Dlss, amf)));
    amf.fsr = true;
    check(restoredFrameGenerationBackend(F::Dlss, amf) == F::Fsr);
    check(restoredFrameGenerationBackend(F::AmdFrc, amf) == F::AmdFrc);
    std::cout << "RADEON_SELECTION checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
