// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/gfx/AmfFrcBackend.h"
#include "veyra/gfx/FsrSrBackend.h"
#include "veyra/gfx/FsrFgPresenter.h"
#include "veyra/guidance/AmdOpticalFlow.h"
#include <iostream>

int main() {
    using namespace veyra;
    // This executable deliberately links backends compiled without SDKs,
    // even on machines with AMD runtime DLLs installed.
    const bool ok = !gfx::AmfFrcBackend::compiledIn() && !gfx::FsrSrBackend::compiledIn() &&
        !gfx::FsrFgPresenter::compiledIn() && !guidance::AmdOpticalFlow::compiledIn();
    std::cout << "NO_SDK_CAPABILITIES pass=" << ok
              << " driverAmfPresent=" << gfx::AmfFrcBackend::runtimeInstalled() << '\n';
    return ok ? 0 : 1;
}
