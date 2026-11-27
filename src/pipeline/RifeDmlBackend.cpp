// SPDX-License-Identifier: GPL-3.0-only
// RIFE 4.6 graph reader for the upstream NCNN export. No model data is
// embedded.
#include "veyra/pipeline/RifeDmlBackend.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <DirectML.h>
#include <span>
#ifdef __MINGW32__
// MinGW does not consume the SDK's MSVC uuid attributes. These are the public
// COM interface IDs, identical to DirectML.h; no runtime symbols are redefined.
__CRT_UUID_DECL(IDMLDevice, 0x6dbd6437, 0x96fd, 0x423f, 0xa9, 0x8c, 0xae, 0x5e,
                0x7c, 0x2a, 0x57, 0x3f)
__CRT_UUID_DECL(IDMLDevice1, 0xa0884f9a, 0xd2be, 0x4355, 0xaa, 0x5d, 0x59, 0x01,
                0x28, 0x1a, 0xd1, 0xd2)
__CRT_UUID_DECL(IDMLOperator, 0x26caae7a, 0x3081, 0x4633, 0x95, 0x81, 0x22,
                0x6f, 0xbe, 0x57, 0x69, 0x5d)
__CRT_UUID_DECL(IDMLCompiledOperator, 0x6b15e56a, 0xbf5c, 0x4902, 0x92, 0xd8,
                0xda, 0x3a, 0x65, 0x0a, 0xfe, 0xa4)
__CRT_UUID_DECL(IDMLOperatorInitializer, 0x427c1113, 0x435c, 0x469c, 0x86, 0x76,
                0x4d, 0x5d, 0xd0, 0x72, 0xf8, 0x13)
__CRT_UUID_DECL(IDMLBindingTable, 0x29c687dc, 0xde74, 0x4e3b, 0xab, 0x00, 0x11,
                0x68, 0xf2, 0xfc, 0x3c, 0xfc)
__CRT_UUID_DECL(IDMLCommandRecorder, 0xe6857a76, 0x2e3e, 0x4fdd, 0xbf, 0xf4,
                0x5d, 0x2b, 0xa1, 0x0f, 0xb4, 0x53)
#endif
#include <DirectMLX.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace veyra::pipeline {
namespace {
void check(bool ok, const std::string &s) {
  if (!ok)
    throw std::runtime_error(s);
}
void checked(HRESULT h, const char *s) {
  if (FAILED(h))
    throw std::runtime_error(std::format("{}: 0x{:08x}", s, unsigned(h)));
}
DML_BINDING_PROPERTIES bindingProperties(IDMLDispatchable *op) {
#ifdef __MINGW32__
  // The SDK's struct-return method uses MSVC's this-before-return-buffer ABI.
  // MinGW's default C++ ABI orders them differently. Other COM methods use
  // scalar returns and require no adapter.
  DML_BINDING_PROPERTIES result{};
  using Get =
      void(STDMETHODCALLTYPE *)(IDMLDispatchable *, DML_BINDING_PROPERTIES *);
  reinterpret_cast<Get>((*reinterpret_cast<void ***>(op))[8])(op, &result);
  return result;
#else
  return op->GetBindingProperties();
#endif
}
using Dimensions = dml::TensorDimensions;
ComPtr<ID3D12Resource> buffer(ID3D12Device *d, uint64_t bytes) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC r{};
  r.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  r.Width = (bytes + 3) & ~uint64_t(3);
  r.Height = r.DepthOrArraySize = r.MipLevels = r.SampleDesc.Count = 1;
  r.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  r.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> p;
  checked(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &r,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                     nullptr, IID_PPV_ARGS(&p)),
          "RIFE buffer");
  return p;
}
void barrier(ID3D12GraphicsCommandList *l) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  l->ResourceBarrier(1, &b);
}
float fromHalf(uint16_t h) {
  int e = (h >> 10) & 31, m = h & 1023;
  float v = e ? std::ldexp(float(1024 + m), e - 25) : std::ldexp(float(m), -24);
  return h & 0x8000 ? -v : v;
}
uint16_t toHalf(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, 4);
  uint32_t sign = (bits >> 16) & 0x8000;
  int e = int((bits >> 23) & 255) - 127;
  uint32_t m = bits & 0x7fffff;
  check(std::isfinite(value) && std::abs(value) <= 65504,
        "RIFE weight outside FP16 range");
  if (e < -25)
    return uint16_t(sign);
  if (e < -14) {
    m |= 0x800000;
    int shift = -e - 1;
    uint32_t rounded =
        (m + ((1u << (shift - 1)) - 1) + ((m >> shift) & 1)) >> shift;
    return uint16_t(sign | rounded);
  }
  uint32_t rounded = m + 0xfff + ((m >> 13) & 1);
  return uint16_t(sign | (uint32_t(e + 15) << 10) + (rounded >> 13));
}
struct Node {
  std::string type, name;
  std::vector<std::string> in, out;
  std::map<int, std::vector<float>> p;
  std::vector<float> weights, bias;
  float value(int key, float fallback = 0) const {
    auto i = p.find(key);
    return i == p.end() ? fallback : i->second.at(0);
  }
};
std::vector<Node> loadModel(const std::filesystem::path &root) {
  std::ifstream f(root / L"flownet.param"),
      b(root / L"flownet.bin", std::ios::binary);
  check(bool(f) && bool(b), "RIFE model missing");
  int magic = 0, count = 0, blobs = 0;
  f >> magic >> count >> blobs;
  check(magic == 7767517 &&
            ((count == 215 && blobs == 276) || (count == 315 && blobs == 391)),
        "expected RIFE 4.6 or 4.22 Lite graph");
  std::string line;
  std::getline(f, line);
  std::vector<Node> nodes;
  while (std::getline(f, line)) {
    if (line.empty())
      continue;
    std::istringstream s(line);
    Node n;
    int ni = 0, no = 0;
    s >> n.type >> n.name >> ni >> no;
    check(ni >= 0 && ni <= 8 && no > 0 && no <= 8, "RIFE arity");
    n.in.resize(ni);
    n.out.resize(no);
    for (auto &x : n.in)
      s >> x;
    for (auto &x : n.out)
      s >> x;
    std::string p;
    while (s >> p) {
      auto eq = p.find('=');
      check(eq != std::string::npos, "RIFE parameter");
      int k = std::stoi(p.substr(0, eq));
      std::string values = p.substr(eq + 1);
      std::replace(values.begin(), values.end(), ',', ' ');
      std::istringstream v(values);
      float a = 0;
      std::vector<float> vs;
      while (v >> a)
        vs.push_back(a);
      if (k <= -23300) {
        check(!vs.empty() && int(vs[0]) == int(vs.size()) - 1, "RIFE array");
        k = -23300 - k;
        vs.erase(vs.begin());
      }
      n.p.emplace(k, std::move(vs));
    }
    if (n.type == "MemoryData") {
      size_t length =
          size_t(n.value(0, 1)) * size_t(n.value(1, 1)) * size_t(n.value(2, 1));
      check(length > 0 && length <= 192 && n.value(21, 1) == 1,
            "RIFE constant shape");
      n.weights.resize(length);
      b.read(reinterpret_cast<char *>(n.weights.data()), length * 4);
      check(bool(b), "truncated RIFE constant");
    } else if (n.type == "Convolution" || n.type == "Deconvolution") {
      uint32_t tag = 0;
      b.read(reinterpret_cast<char *>(&tag), 4);
      size_t length = size_t(n.value(6));
      check(length > 0 && length <= 400000 && tag == 0x01306B47,
            "RIFE FP16 weights");
      std::vector<uint16_t> packed(length);
      b.read(reinterpret_cast<char *>(packed.data()), length * 2);
      if (length % 2)
        b.seekg(2, std::ios::cur);
      n.weights.resize(length);
      std::transform(packed.begin(), packed.end(), n.weights.begin(), fromHalf);
      check(n.value(5) == 1, "RIFE convolution bias");
      n.bias.resize(size_t(n.value(0)));
      b.read(reinterpret_cast<char *>(n.bias.data()), n.bias.size() * 4);
      check(bool(b), "truncated RIFE weights");
    }
    nodes.push_back(std::move(n));
  }
  check(nodes.size() == size_t(count) &&
            b.peek() == std::char_traits<char>::eof(),
        "RIFE model length mismatch");
  return nodes;
}
struct Tensor {
  Dimensions dims;
  ComPtr<ID3D12Resource> gpu, upload;
  bool constant = false;
  uint64_t bytes() const {
    uint64_t n = 2;
    for (auto d : dims)
      n *= d;
    return n;
  }
};
using T = std::shared_ptr<Tensor>;
struct Stage {
  bool warp = false;
  std::vector<T> inputs, outputs;
  ComPtr<IDMLCompiledOperator> op;
  ComPtr<IDMLOperatorInitializer> init;
  ComPtr<IDMLBindingTable> table, initTable;
  ComPtr<ID3D12DescriptorHeap> heap, initHeap;
  ComPtr<ID3D12Resource> temp, persistent, initTemp;
};
const char *warpShader = R"(
cbuffer C:register(b0){uint W,H,Channels,Unused;}
RWByteAddressBuffer A:register(u0);RWByteAddressBuffer Flow:register(u1);RWByteAddressBuffer O:register(u2);
float readValue(RWByteAddressBuffer src,uint i){return f16tof32(src.Load((i>>1)*4)>>((i&1)*16));}
float at(int x,int y,uint c){return readValue(A,(c*H+clamp(y,0,int(H)-1))*W+clamp(x,0,int(W)-1));}
float warped(uint2 p,uint c){uint i=p.y*W+p.x;float2 q=float2(p)+float2(readValue(Flow,i),readValue(Flow,W*H+i));int2 a=int2(floor(q));float2 t=q-a;return lerp(lerp(at(a.x,a.y,c),at(a.x+1,a.y,c),t.x),lerp(at(a.x,a.y+1,c),at(a.x+1,a.y+1,c),t.x),t.y);}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){p.x*=2;if(p.x>=W||p.y>=H)return;uint i=p.y*W+p.x;
 for(uint c=0;c<Channels;c++){float v0=warped(p.xy,c),v1=warped(p.xy+uint2(1,0),c);O.Store(2*(c*W*H+i),f32tof16(v0)|(f32tof16(v1)<<16));}}
)";
const char *imageShader = R"(
cbuffer C:register(b0){uint W,H,MW,MH,PW,PH,Mode,Encoding;}
Texture2D<float4>A:register(t0);Texture2D<float4>B:register(t1);RWTexture2D<float4>O:register(u0);
RWByteAddressBuffer T0:register(u1);RWByteAddressBuffer T1:register(u2);RWByteAddressBuffer T2:register(u3);
// Encoding: 0 = unchanged SDR, 1 = HDR10/PQ BT.2020, 2 = scRGB/BT.709.
// HDR is decoded BEFORE spatial/visibility interpolation. Only motion sees a
// fixed SDR proxy; output samples the original HDR textures, retaining peaks.
float4 decode(float4 v){if(Encoding!=1)return v;float3 p=pow(saturate(v.rgb),32.0/2523.0);float3 c=125.0*pow(max(p-3424.0/4096.0,0)/max(2413.0/128.0-2392.0/128.0*p,1e-6),16384.0/2610.0);return float4(mul(float3x3(1.660491,-.587641,-.072850,-.124550,1.132900,-.008349,-.018151,-.100579,1.118730),c),v.a);}
float3 encode(float3 c){if(Encoding!=1)return c;c=mul(float3x3(.627404,.329283,.043313,.069097,.919540,.011362,.016391,.088013,.895595),c);float3 p=pow(saturate(c*.008),2610.0/16384.0);return pow((3424.0/4096.0+(2413.0/128.0)*p)/(1+(2392.0/128.0)*p),2523.0/32.0);}
float srgb(float c){return c<=.0031308?12.92*c:1.055*pow(c,1.0/2.4)-.055;}
float4 proxy(float4 v){if(Encoding==0)return v;float3 c=max(v.rgb*(80.0/203.0),0);c=min(c,.75)+.25*(1-exp(-5.778*max(c-.75,0)));return float4(srgb(c.r),srgb(c.g),srgb(c.b),v.a);}
float4 sampleA(float2 p){int2 a=int2(floor(p));float2 t=p-a;int2 lim=int2(W,H)-1;return lerp(lerp(decode(A.Load(int3(clamp(a,0,lim),0))),decode(A.Load(int3(clamp(a+int2(1,0),0,lim),0))),t.x),lerp(decode(A.Load(int3(clamp(a+int2(0,1),0,lim),0))),decode(A.Load(int3(clamp(a+1,0,lim),0))),t.x),t.y);}
float4 sampleB(float2 p){int2 a=int2(floor(p));float2 t=p-a;int2 lim=int2(W,H)-1;return lerp(lerp(decode(B.Load(int3(clamp(a,0,lim),0))),decode(B.Load(int3(clamp(a+int2(1,0),0,lim),0))),t.x),lerp(decode(B.Load(int3(clamp(a+int2(0,1),0,lim),0))),decode(B.Load(int3(clamp(a+1,0,lim),0))),t.x),t.y);}
float tensor(uint c,int2 p,bool mask){p=clamp(p,0,int2(PW,PH)-1);uint i=(c*PH+p.y)*PW+p.x;uint packed=mask?T1.Load((i>>1)*4):T0.Load((i>>1)*4);return f16tof32(packed>>((i&1)*16));}
float sampleT(uint c,float2 p,bool mask){int2 a=int2(floor(p));float2 t=p-a;return lerp(lerp(tensor(c,a,mask),tensor(c,a+int2(1,0),mask),t.x),lerp(tensor(c,a+int2(0,1),mask),tensor(c,a+1,mask),t.x),t.y);}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){p.x*=2;
 if(Mode==0){if(p.x>=PW||p.y>=PH)return;float2 q=(p.xy+.5)*float2(W,H)/float2(MW,MH)-.5;float4 a=proxy(sampleA(q)),b=proxy(sampleB(q)),a1=proxy(sampleA(q+float2(float(W)/MW,0))),b1=proxy(sampleB(q+float2(float(W)/MW,0)));uint i=p.y*PW+p.x;
  if(p.y>=MH||p.x>=MW){a=b=0;}if(p.y>=MH||p.x+1>=MW){a1=b1=0;}
  for(uint c=0;c<3;c++){T0.Store(2*(c*PW*PH+i),f32tof16(a[c])|(f32tof16(a1[c])<<16));T1.Store(2*(c*PW*PH+i),f32tof16(b[c])|(f32tof16(b1[c])<<16));}T2.Store(2*i,0x38003800);
 }else{for(uint j=0;j<2;j++){if(p.x>=W||p.y>=H)return;float2 q=(p.xy+.5)*float2(MW,MH)/float2(W,H)-.5;float2 scale=float2(W,H)/float2(MW,MH);
  float2 f0=float2(sampleT(0,q,false),sampleT(1,q,false))*scale,f1=float2(sampleT(2,q,false),sampleT(3,q,false))*scale;float m=saturate(sampleT(0,q,true));
  O[p.xy]=float4(encode(lerp(sampleB(p.xy+f1).rgb,sampleA(p.xy+f0).rgb,m)),1);p.x++;
 }}}
)";
struct Shader {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pso;
  void open(ID3D12Device *d, const char *code, bool images) {
    D3D12_ROOT_PARAMETER p[6]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, images ? 8u : 4u};
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    if (images) {
      for (unsigned i = 0; i < 2; i++) {
        p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1 + i].DescriptorTable = {1, &ranges[i]};
      }
      for (unsigned i = 0; i < 3; i++) {
        p[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[3 + i].Descriptor = {i + 1, 0};
      }
    } else
      for (unsigned i = 0; i < 3; i++) {
        p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[1 + i].Descriptor = {i, 0};
      }
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = images ? 6 : 4;
    rd.pParameters = p;
    ComPtr<ID3DBlob> blob, err;
    checked(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1,
                                        &blob, &err),
            "RIFE root serialize");
    checked(d->CreateRootSignature(0, blob->GetBufferPointer(),
                                   blob->GetBufferSize(), IID_PPV_ARGS(&root)),
            "RIFE root");
    HRESULT c =
        D3DCompile(code, strlen(code), nullptr, nullptr, nullptr, "main",
                   "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(c) && err)
      throw std::runtime_error(static_cast<char *>(err->GetBufferPointer()));
    checked(c, "RIFE shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
    checked(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)),
            "RIFE pipeline");
  }
};
} // namespace
struct RifeDmlBackend::Impl {
  HMODULE module = nullptr;
  ComPtr<ID3D12Device> device;
  ComPtr<IDMLDevice> dml;
  ComPtr<IDMLCommandRecorder> recorder;
  std::vector<Stage> stages;
  std::vector<T> tensors;
  std::map<std::string, T> globals;
  Shader warp, image;
  ComPtr<ID3D12DescriptorHeap> imageHeap, stagingHeap;
  unsigned increment = 0, w = 0, h = 0, mw = 0, mh = 0, pw = 0, ph = 0,
           descriptorCursor = 0;
  T flow, mask;
  ~Impl() {
    stages.clear();
    recorder.Reset();
    dml.Reset();
    if (module)
      FreeLibrary(module);
  }
  T tensor(Dimensions dims, bool constant = false) {
    auto t = std::make_shared<Tensor>();
    t->dims = std::move(dims);
    t->constant = constant;
    t->gpu = buffer(device.Get(), t->bytes());
    tensors.push_back(t);
    return t;
  }
  T weights(Dimensions dims, const std::vector<float> &v,
            ID3D12GraphicsCommandList *l) {
    auto t = tensor(std::move(dims), true);
    check(v.size() * 2 == t->bytes(), "RIFE weight shape");
    t->upload = makeUploadBuffer(device.Get(), t->bytes());
    check(bool(t->upload), "RIFE weight upload");
    void *p = nullptr;
    checked(t->upload->Map(0, nullptr, &p), "RIFE map");
    for (size_t i = 0; i < v.size(); i++)
      static_cast<uint16_t *>(p)[i] = toHalf(v[i]);
    t->upload->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {t->gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_DEST};
    l->ResourceBarrier(1, &b);
    l->CopyBufferRegion(t->gpu.Get(), 0, t->upload.Get(), 0, t->bytes());
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    return t;
  }
  void bindings(Stage &s, ID3D12GraphicsCommandList *l) {
    IDMLCompiledOperator *ops[]{s.op.Get()};
    checked(dml->CreateOperatorInitializer(1, ops, IID_PPV_ARGS(&s.init)),
            "RIFE initializer");
    auto allocTable = [&](IDMLDispatchable *op,
                          ComPtr<ID3D12DescriptorHeap> &heap,
                          ComPtr<IDMLBindingTable> &table) {
      auto prop = bindingProperties(op);
      D3D12_DESCRIPTOR_HEAP_DESC hd{};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      hd.NumDescriptors = std::max(1u, prop.RequiredDescriptorCount);
      checked(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
              "RIFE descriptor heap");
      DML_BINDING_TABLE_DESC bd{op, heap->GetCPUDescriptorHandleForHeapStart(),
                                heap->GetGPUDescriptorHandleForHeapStart(),
                                hd.NumDescriptors};
      checked(dml->CreateBindingTable(&bd, IID_PPV_ARGS(&table)),
              "RIFE binding table");
      return prop;
    };
    auto initProp = allocTable(s.init.Get(), s.initHeap, s.initTable),
         prop = allocTable(s.op.Get(), s.heap, s.table);
    auto bindBuffer = [&](uint64_t size, ComPtr<ID3D12Resource> &p,
                          IDMLBindingTable *table, bool persistent) {
      if (!size)
        return;
      p = buffer(device.Get(), size);
      DML_BUFFER_BINDING bb{p.Get(), 0, size};
      DML_BINDING_DESC bd{DML_BINDING_TYPE_BUFFER, &bb};
      if (persistent)
        table->BindPersistentResource(&bd);
      else
        table->BindTemporaryResource(&bd);
    };
    bindBuffer(prop.TemporaryResourceSize, s.temp, s.table.Get(), false);
    bindBuffer(initProp.TemporaryResourceSize, s.initTemp, s.initTable.Get(),
               false);
    bindBuffer(prop.PersistentResourceSize, s.persistent, s.table.Get(), true);
    if (s.persistent) {
      DML_BUFFER_BINDING bb{s.persistent.Get(), 0, prop.PersistentResourceSize};
      DML_BINDING_DESC bd{DML_BINDING_TYPE_BUFFER, &bb};
      s.initTable->BindOutputs(1, &bd);
    }
    std::vector<DML_BUFFER_BINDING> buffers(s.inputs.size());
    std::vector<DML_BINDING_DESC> desc(s.inputs.size());
    for (size_t i = 0; i < s.inputs.size(); i++) {
      auto t = s.inputs[i];
      buffers[i] = {t->constant ? t->gpu.Get() : nullptr, 0,
                    t->constant ? t->bytes() : 0};
    }
    DML_BUFFER_ARRAY_BINDING arr{unsigned(buffers.size()), buffers.data()};
    DML_BINDING_DESC ab{DML_BINDING_TYPE_BUFFER_ARRAY, &arr};
    s.initTable->BindInputs(1, &ab);
    for (size_t i = 0; i < s.inputs.size(); i++) {
      auto t = s.inputs[i];
      buffers[i] = {t->gpu.Get(), 0, t->bytes()};
      desc[i] = {t->constant ? DML_BINDING_TYPE_NONE : DML_BINDING_TYPE_BUFFER,
                 t->constant ? nullptr : &buffers[i]};
    }
    s.table->BindInputs(unsigned(desc.size()), desc.data());
    buffers.resize(s.outputs.size());
    desc.resize(s.outputs.size());
    for (size_t i = 0; i < s.outputs.size(); i++) {
      buffers[i] = {s.outputs[i]->gpu.Get(), 0, s.outputs[i]->bytes()};
      desc[i] = {DML_BINDING_TYPE_BUFFER, &buffers[i]};
    }
    s.table->BindOutputs(unsigned(desc.size()), desc.data());
    ID3D12DescriptorHeap *heaps[]{s.initHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    recorder->RecordDispatch(l, s.init.Get(), s.initTable.Get());
    barrier(l);
  }
  void compile(const std::filesystem::path &root,
               ID3D12GraphicsCommandList *list) {
    auto nodes = loadModel(root);
    fprintf(stderr, "RIFE model read\n");
    globals["in0"] = tensor({1, 3, ph, pw});
    globals["in1"] = tensor({1, 3, ph, pw});
    globals["in2"] = tensor({1, 1, ph, pw});
    // Outputs before final warps: four displacement channels and visibility.
    // Composition is deliberately done on original-resolution images.
    const bool lite = nodes.size() == 315;
    const std::string flowName = lite ? "572" : "327",
                      maskName = lite ? "585" : "332";
    const std::set<std::string> finalNames{flowName, maskName};
    std::unique_ptr<dml::Graph> graph;
    std::map<std::string, dml::Expression> expr;
    Stage stage;
    std::unordered_map<void *, T> imported;
    auto fresh = [&] {
      graph = std::make_unique<dml::Graph>(dml.Get());
      expr.clear();
      imported.clear();
      stage = Stage{};
    };
    fresh();
    auto input = [&](const std::string &name) {
      auto i = expr.find(name);
      if (i != expr.end())
        return i->second;
      auto t = globals.at(name);
      dml::TensorDesc td(DML_TENSOR_DATA_TYPE_FLOAT16,
                         t->constant ? DML_TENSOR_FLAG_OWNED_BY_DML
                                     : DML_TENSOR_FLAG_NONE,
                         t->dims);
      auto x = dml::InputTensor(*graph, unsigned(stage.inputs.size()), td);
      stage.inputs.push_back(t);
      expr[name] = x;
      imported[x.Impl()] = t;
      return x;
    };
    auto weightExpr = [&](T t) {
      dml::TensorDesc td(DML_TENSOR_DATA_TYPE_FLOAT16,
                         DML_TENSOR_FLAG_OWNED_BY_DML, t->dims);
      auto x = dml::InputTensor(*graph, unsigned(stage.inputs.size()), td);
      stage.inputs.push_back(t);
      return x;
    };
    auto flush = [&](size_t next) {
      std::set<std::string> needed = finalNames;
      for (size_t i = next; i < nodes.size(); i++)
        for (auto &n : nodes[i].in)
          needed.insert(n);
      std::vector<dml::Expression> outputs;
      std::unordered_map<void *, T> saved = imported;
      for (auto &name : needed) {
        auto i = expr.find(name);
        if (i == expr.end())
          continue;
        auto x = i->second;
        auto it = saved.find(x.Impl());
        if (it != saved.end())
          globals[name] = it->second;
        else {
          auto t = tensor(x.GetOutputDesc().sizes);
          saved[x.Impl()] = t;
          globals[name] = t;
          outputs.push_back(x);
          stage.outputs.push_back(t);
        }
      }
      if (!outputs.empty()) {
        fprintf(stderr, "RIFE compile next=%zu inputs=%zu outputs=%zu\n", next,
                stage.inputs.size(), outputs.size());
        stage.op =
            graph->Compile(DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION,
                           outputs, unsigned(stage.inputs.size()));
        fprintf(stderr, "RIFE compiled\n");
        bindings(stage, list);
        fprintf(stderr, "RIFE bound\n");
        stages.push_back(std::move(stage));
      }
      fresh();
    };
    for (size_t index = 0; index < nodes.size(); index++) {
      const auto &n = nodes[index];
      if (n.name == "warp_6") {
        flush(index);
        break;
      }
      if (n.type == "Input")
        continue;
      if (lite && (n.name == "Slice_407" || n.name == "Slice_413" ||
                   n.name == "warp_408" || n.name == "warp_414"))
        continue;
      if (n.type == "MemoryData") {
        globals[n.out[0]] =
            weights({1, unsigned(n.value(2)), unsigned(n.value(1)),
                     unsigned(n.value(0))},
                    n.weights, list);
        continue;
      }
      if (n.type == "rife.Warp") {
        flush(index);
        Stage s;
        s.warp = true;
        s.inputs = {globals.at(n.in[0]), globals.at(n.in[1])};
        auto t = tensor(s.inputs[0]->dims);
        s.outputs = {t};
        globals[n.out[0]] = t;
        stages.push_back(std::move(s));
        continue;
      }
      try {
        auto a = input(n.in[0]);
        dml::Expression x;
        if (n.type == "Split") {
          for (auto &o : n.out)
            expr[o] = a;
          continue;
        }
        if (n.type == "Concat") {
          std::vector<dml::Expression> ins;
          for (auto &v : n.in)
            ins.push_back(input(v));
          x = dml::Join(ins, 1);
        } else if (n.type == "Interp") {
          auto dims = a.GetOutputDesc().sizes;
          dims[2] = unsigned(std::round(dims[2] * n.value(1, 1)));
          dims[3] = unsigned(std::round(dims[3] * n.value(2, 1)));
          x = dml::Resample(a, dims, DML_INTERPOLATION_MODE_LINEAR);
        } else if (n.type == "Crop") {
          auto sizes = a.GetOutputDesc().sizes;
          unsigned begin = unsigned(n.p.at(9).at(0)),
                   end = std::min(sizes[1], unsigned(n.p.at(10).at(0)));
          check(n.p.at(11).at(0) == 0 && end > begin, "RIFE channel slice");
          sizes[1] = end - begin;
          std::array<uint32_t, 4> offsets{0, begin, 0, 0};
          std::array<int32_t, 4> strides{1, 1, 1, 1};
          x = dml::Slice(a, offsets, sizes, strides);
        } else if (n.type == "BinaryOp") {
          int op = int(n.value(0));
          bool scalar = n.value(1) == 1;
          float v = n.value(2);
          if (scalar) {
            if (op == 0)
              x = a + v;
            else if (op == 2)
              x = a * v;
            else if (op == 3)
              x = a / v;
            else if (op == 7)
              x = v - a;
            else
              throw std::runtime_error("unsupported RIFE scalar op");
          } else {
            auto b = input(n.in[1]);
            auto ad = a.GetOutputDesc().sizes, bd = b.GetOutputDesc().sizes;
            if (ad != bd) {
              check(bd == Dimensions({1, ad[1], 1, 1}),
                    "RIFE channel broadcast");
              b = dml::Reinterpret(b, ad, Dimensions{0, 1, 0, 0});
            }
            if (op == 0)
              x = a + b;
            else if (op == 2)
              x = a * b;
            else
              throw std::runtime_error("unsupported RIFE binary op");
          }
        } else if (n.type == "Eltwise") {
          check(n.value(0) == 1 && n.in.size() == 2 && n.p.at(1).size() == 2,
                "RIFE weighted sum");
          x = a * n.p.at(1)[0] + input(n.in[1]) * n.p.at(1)[1];
        } else if (n.type == "ReLU")
          x = dml::ActivationLeakyRelu(a, n.value(0));
        else if (n.type == "Sigmoid")
          x = dml::ActivationSigmoid(a);
        else if (n.type == "PixelShuffle")
          x = dml::DepthToSpace(a, unsigned(n.value(0)),
                                DML_DEPTH_SPACE_ORDER_COLUMN_ROW_DEPTH);
        else if (n.type == "Convolution" || n.type == "Deconvolution") {
          auto shape = a.GetOutputDesc().sizes;
          unsigned oc = unsigned(n.value(0)), ic = shape[1],
                   k = unsigned(n.value(1)), stride = unsigned(n.value(3, 1)),
                   pad = unsigned(n.value(4));
          bool backward = n.type == "Deconvolution";
          auto wgt = n.weights;
          Dimensions filter{oc, ic, k, k};
          if (backward) {
            filter = {ic, oc, k, k};
            for (unsigned o = 0; o < oc; o++)
              for (unsigned i = 0; i < ic; i++)
                for (unsigned j = 0; j < k * k; j++)
                  wgt[(i * oc + o) * k * k + j] =
                      n.weights[(o * ic + i) * k * k + j];
          }
          auto wt = weightExpr(weights(filter, wgt, list)),
               bias = weightExpr(weights({1, oc, 1, 1}, n.bias, list));
          Dimensions out;
          if (backward)
            out = {1, oc, (shape[2] - 1) * stride + k - 2 * pad,
                   (shape[3] - 1) * stride + k - 2 * pad};
          std::array<uint32_t, 2> strides{stride, stride}, dilation{1, 1},
              padding{pad, pad}, outputPadding{0, 0};
          x = dml::Convolution(
              a, wt, bias, DML_CONVOLUTION_MODE_CROSS_CORRELATION,
              backward ? DML_CONVOLUTION_DIRECTION_BACKWARD
                       : DML_CONVOLUTION_DIRECTION_FORWARD,
              strides, dilation, padding, padding, outputPadding, 1,
              n.value(9) == 2 ? dml::FusedActivation::LeakyRelu(.2f)
                              : dml::FusedActivation::None(),
              out);
        } else
          throw std::runtime_error("unsupported RIFE node " + n.type);
        expr[n.out[0]] = x;
        if (lite && n.out[0] == maskName) {
          flush(nodes.size());
          break;
        }
      } catch (const std::exception &e) {
        throw std::runtime_error(n.name + ": " + e.what());
      }
    }
    flow = globals.at(flowName);
    mask = globals.at(maskName);
    check(flow->dims == Dimensions({1, 4, ph, pw}) &&
              mask->dims == Dimensions({1, 1, ph, pw}),
          "RIFE output shape");
  }
};
RifeDmlBackend::RifeDmlBackend() = default;
RifeDmlBackend::~RifeDmlBackend() = default;
void RifeDmlBackend::open(ID3D12Device *device, ID3D12GraphicsCommandList *list,
                          const std::filesystem::path &root, unsigned width,
                          unsigned height, float scale) {
  check(!impl_ && device && list && root.is_absolute() && width >= 64 &&
            height >= 64 && width <= 3840 && height <= 2160 &&
            std::isfinite(scale) && scale >= .25f && scale <= 1,
        "RIFE initialization arguments");
  auto p = std::make_unique<Impl>();
  p->device = device;
  p->w = width;
  p->h = height;
  p->mw = unsigned(std::ceil(width * scale));
  p->mh = unsigned(std::ceil(height * scale));
  p->pw = (p->mw + 31) & ~31u;
  p->ph = (p->mh + 31) & ~31u;
  p->module = LoadLibraryExW((root / L"DirectML.dll").c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  check(p->module != nullptr, "RIFE DirectML runtime missing");
  auto create = reinterpret_cast<HRESULT(WINAPI *)(
      ID3D12Device *, DML_CREATE_DEVICE_FLAGS, DML_FEATURE_LEVEL, REFIID,
      void **)>(GetProcAddress(p->module, "DMLCreateDevice1"));
  check(create != nullptr, "DMLCreateDevice1");
  checked(create(device, DML_CREATE_DEVICE_FLAG_NONE, DML_FEATURE_LEVEL_5_0,
                 IID_PPV_ARGS(&p->dml)),
          "RIFE DML device");
  checked(p->dml->CreateCommandRecorder(IID_PPV_ARGS(&p->recorder)),
          "RIFE recorder");
  fprintf(stderr, "RIFE device ready\n");
  p->warp.open(device, warpShader, false);
  p->image.open(device, imageShader, true);
  fprintf(stderr, "RIFE shaders ready\n");
  p->compile(root, list);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 96;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  checked(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->imageHeap)),
          "RIFE image heap");
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  hd.NumDescriptors = 3;
  checked(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->stagingHeap)),
          "RIFE staging heap");
  p->increment = device->GetDescriptorHandleIncrementSize(hd.Type);
  impl_ = std::move(p);
}
unsigned RifeDmlBackend::graphCount() const {
  return impl_ ? unsigned(impl_->stages.size()) : 0;
}
void RifeDmlBackend::record(ID3D12GraphicsCommandList *l, ID3D12Resource *a,
                            ID3D12Resource *b, ID3D12Resource *out) {
  check(impl_ && l && a && b && out, "RIFE record arguments");
  auto &p = *impl_;
  const auto format=a->GetDesc().Format;
  check(format==DXGI_FORMAT_R8G8B8A8_UNORM||format==DXGI_FORMAT_R10G10B10A2_UNORM||format==DXGI_FORMAT_R16G16B16A16_FLOAT,"RIFE unsupported color encoding");
  for (auto r : {a, b, out}) {
    auto d = r->GetDesc();
    check(d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == p.w &&
              d.Height == p.h && d.Format == format,
          "RIFE texture format/extent");
  }
  // 32 descriptor triplets bound the caller's maximum three in-flight lists.
  // Descriptors differ per output slot; no descriptors are rewritten in flight.
  unsigned slot = (p.descriptorCursor++ % 32) * 3;
  auto cpu = p.stagingHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = format;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  p.device->CreateShaderResourceView(a, &sd, cpu);
  cpu.ptr += p.increment;
  p.device->CreateShaderResourceView(b, &sd, cpu);
  cpu.ptr += p.increment;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = sd.Format;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  p.device->CreateUnorderedAccessView(out, nullptr, &ud, cpu);
  auto dst = p.imageHeap->GetCPUDescriptorHandleForHeapStart();
  dst.ptr += slot * p.increment;
  p.device->CopyDescriptorsSimple(
      3, dst, p.stagingHeap->GetCPUDescriptorHandleForHeapStart(),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto images = [&](bool final) {
    ID3D12DescriptorHeap *heaps[]{p.imageHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(p.image.root.Get());
    l->SetPipelineState(p.image.pso.Get());
    unsigned c[]{p.w, p.h, p.mw, p.mh, p.pw, p.ph, unsigned(final),
      format==DXGI_FORMAT_R10G10B10A2_UNORM?1u:format==DXGI_FORMAT_R16G16B16A16_FLOAT?2u:0u};
    l->SetComputeRoot32BitConstants(0, 8, c, 0);
    auto gpu = p.imageHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += slot * p.increment;
    l->SetComputeRootDescriptorTable(1, gpu);
    gpu.ptr += 2 * p.increment;
    l->SetComputeRootDescriptorTable(2, gpu);
    l->SetComputeRootUnorderedAccessView(
        3, (final ? p.flow : p.globals.at("in0"))->gpu->GetGPUVirtualAddress());
    l->SetComputeRootUnorderedAccessView(
        4, (final ? p.mask : p.globals.at("in1"))->gpu->GetGPUVirtualAddress());
    l->SetComputeRootUnorderedAccessView(
        5, p.globals.at("in2")->gpu->GetGPUVirtualAddress());
    l->Dispatch(((final ? p.w : p.pw) + 15) / 16,
                ((final ? p.h : p.ph) + 7) / 8, 1);
    barrier(l);
  };
  images(false);
  for (auto &s : p.stages) {
    if (s.warp) {
      l->SetComputeRootSignature(p.warp.root.Get());
      l->SetPipelineState(p.warp.pso.Get());
      auto d = s.outputs[0]->dims;
      unsigned c[]{d[3], d[2], d[1], 0};
      l->SetComputeRoot32BitConstants(0, 4, c, 0);
      l->SetComputeRootUnorderedAccessView(
          1, s.inputs[0]->gpu->GetGPUVirtualAddress());
      l->SetComputeRootUnorderedAccessView(
          2, s.inputs[1]->gpu->GetGPUVirtualAddress());
      l->SetComputeRootUnorderedAccessView(
          3, s.outputs[0]->gpu->GetGPUVirtualAddress());
      l->Dispatch((d[3] + 15) / 16, (d[2] + 7) / 8, 1);
    } else {
      ID3D12DescriptorHeap *heaps[]{s.heap.Get()};
      l->SetDescriptorHeaps(1, heaps);
      p.recorder->RecordDispatch(l, s.op.Get(), s.table.Get());
    }
    barrier(l);
  }
  images(true);
}
} // namespace veyra::pipeline
