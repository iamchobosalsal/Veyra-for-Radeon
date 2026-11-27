// SPDX-License-Identifier: GPL-3.0-only
// Compile this small translation unit with the MSVC ABI, even when the engine
// uses MinGW. Only the published parameter interface crosses the ABI boundary;
// no NVIDIA DLL, device emulation, or guessed vtable offsets are used.
#include <nvsdk_ngx_params.h>
extern "C" unsigned rifeGetUnsigned(NVSDK_NGX_Parameter *p, const char *key,
                                    unsigned *out) {
  return unsigned(p->Get(key, out));
}
extern "C" unsigned rifeGetResource(NVSDK_NGX_Parameter *p, const char *key,
                                    ID3D12Resource **out) {
  return unsigned(p->Get(key, out));
}
extern "C" void rifeSetUnsigned(NVSDK_NGX_Parameter *p, const char *key,
                                unsigned value) {
  p->Set(key, value);
}
extern "C" void rifeSetInt(NVSDK_NGX_Parameter *p, const char *key, int value) {
  p->Set(key, value);
}
