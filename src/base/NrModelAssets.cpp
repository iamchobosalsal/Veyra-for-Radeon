// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/base/NrModelAssets.h"
#include <array>
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
namespace veyra::base {
namespace {
namespace fs=std::filesystem;
struct Tensor { const char* name; uint64_t elements; };
// Source metadata: lmxxf 4764e4f1a24f5234b8e29952dc41f493b2d77c90,
// small_model_assets.h + hip_reference_network.h WeightElements/RunGraph.
// These sizes contain no weights. Keep f32-first precedence identical to runtime.
constexpr Tensor smallTensors[]={
    {"block0-attention",8225u},
    {"block0-ffn",8736u},
    {"block1-attention",8225u},
    {"block1-ffn",8736u},
    {"block14-attention",82052u},
    {"block14-ds",32768u},
    {"block14-ffn",147584u},
    {"block15-attention",295176u},
    {"block15-ffn",590080u},
    {"block22-attention",295176u},
    {"block22-ds",131072u},
    {"block22-ffn",590080u},
    {"block23-attention",1114640u},
    {"block23-ffwd",524288u},
    {"block23-ffwd-projection",262656u},
    {"block30-attention",1114640u},
    {"block30-ffwd",524288u},
    {"block30-ffwd-projection",262656u},
    {"block31-contract",4195328u},
    {"block31-expand",4194304u},
    {"block31-projection",1049600u},
    {"block31-qkv",3145760u},
    {"block32-contract",4195328u},
    {"block32-expand",4194304u},
    {"block32-projection",1049600u},
    {"block32-qkv",3145760u},
    {"block33-contract",4195328u},
    {"block33-expand",4194304u},
    {"block33-projection",1049600u},
    {"block33-qkv",3145760u},
    {"block34-contract",4195328u},
    {"block34-expand",4194304u},
    {"block34-projection",1049600u},
    {"block34-qkv",3145760u},
    {"block35-contract",4195328u},
    {"block35-expand",4194304u},
    {"block35-projection",1049600u},
    {"block35-qkv",3145760u},
    {"block36-contract",4195328u},
    {"block36-expand",4194304u},
    {"block36-projection",1049600u},
    {"block36-qkv",3145760u},
    {"block37-contract",4195328u},
    {"block37-expand",4194304u},
    {"block37-projection",1049600u},
    {"block37-qkv",3145760u},
    {"block38-contract",4195328u},
    {"block38-expand",4194304u},
    {"block38-projection",1049600u},
    {"block38-qkv",3145760u},
    {"block4-attention",8225u},
    {"block4-ds",2048u},
    {"block4-ffn",8736u},
    {"block40-attention",1114640u},
    {"block40-ffwd",524288u},
    {"block40-ffwd-projection",262656u},
    {"block47-attention",1114640u},
    {"block47-ffwd",524288u},
    {"block47-ffwd-projection",262656u},
    {"block48-attention",295176u},
    {"block48-ffn",590080u},
    {"block48-weights",131328u},
    {"block5-attention",24642u},
    {"block5-ffn",36928u},
    {"block55-attention",295176u},
    {"block55-ffn",590080u},
    {"block56-attention",82052u},
    {"block56-ffn",147584u},
    {"block56-weights",32896u},
    {"block61-attention",82052u},
    {"block61-ffn",147584u},
    {"block62-attention",24642u},
    {"block62-ffn",36928u},
    {"block62-weights",8256u},
    {"block65-attention",24642u},
    {"block65-ffn",36928u},
    {"block66-attention",8225u},
    {"block66-ffn",8736u},
    {"block66-weights",2080u},
    {"block69-attention",8225u},
    {"block69-ffn",8736u},
    {"block8-attention",24642u},
    {"block8-ds",8192u},
    {"block8-ffn",36928u},
    {"block9-attention",82052u},
    {"block9-ffn",147584u},
    {"decoder39-weights",524800u},
    {"head-matrix",524288u},
    {"post70-attention",8225u},
    {"post70-ffn",8736u},
    {"post70-head",96u},
    {"post70-scales",64u},
};
constexpr Tensor originalTensors[]={
    {"block0-attention",8225u},
    {"block0-ffn",8736u},
    {"block1-attention",8225u},
    {"block1-ffn",8736u},
    {"block10-attention",82052u},
    {"block10-ffn",147584u},
    {"block11-attention",82052u},
    {"block11-ffn",147584u},
    {"block12-attention",82052u},
    {"block12-ffn",147584u},
    {"block13-attention",82052u},
    {"block13-ffn",147584u},
    {"block14-attention",82052u},
    {"block14-ds",32768u},
    {"block14-ffn",147584u},
    {"block15-attention",295176u},
    {"block15-ffn",590080u},
    {"block16-attention",295176u},
    {"block16-ffn",590080u},
    {"block17-attention",295176u},
    {"block17-ffn",590080u},
    {"block18-attention",295176u},
    {"block18-ffn",590080u},
    {"block19-attention",295176u},
    {"block19-ffn",590080u},
    {"block2-attention",8225u},
    {"block2-ffn",8736u},
    {"block20-attention",295176u},
    {"block20-ffn",590080u},
    {"block21-attention",295176u},
    {"block21-ffn",590080u},
    {"block22-attention",295176u},
    {"block22-ds",131072u},
    {"block22-ffn",590080u},
    {"block23-attention",1114640u},
    {"block23-ffwd",524288u},
    {"block23-ffwd-projection",262656u},
    {"block24-attention",1114640u},
    {"block24-ffwd",524288u},
    {"block24-ffwd-projection",262656u},
    {"block25-attention",1114640u},
    {"block25-ffwd",524288u},
    {"block25-ffwd-projection",262656u},
    {"block26-attention",1114640u},
    {"block26-ffwd",524288u},
    {"block26-ffwd-projection",262656u},
    {"block27-attention",1114640u},
    {"block27-ffwd",524288u},
    {"block27-ffwd-projection",262656u},
    {"block28-attention",1114640u},
    {"block28-ffwd",524288u},
    {"block28-ffwd-projection",262656u},
    {"block29-attention",1114640u},
    {"block29-ffwd",524288u},
    {"block29-ffwd-projection",262656u},
    {"block3-attention",8225u},
    {"block3-ffn",8736u},
    {"block30-attention",1114640u},
    {"block30-ffwd",524288u},
    {"block30-ffwd-projection",262656u},
    {"block31-contract",4195328u},
    {"block31-expand",4194304u},
    {"block31-projection",1049600u},
    {"block31-qkv",3145760u},
    {"block32-contract",4195328u},
    {"block32-expand",4194304u},
    {"block32-projection",1049600u},
    {"block32-qkv",3145760u},
    {"block33-contract",4195328u},
    {"block33-expand",4194304u},
    {"block33-projection",1049600u},
    {"block33-qkv",3145760u},
    {"block34-contract",4195328u},
    {"block34-expand",4194304u},
    {"block34-projection",1049600u},
    {"block34-qkv",3145760u},
    {"block35-contract",4195328u},
    {"block35-expand",4194304u},
    {"block35-projection",1049600u},
    {"block35-qkv",3145760u},
    {"block36-contract",4195328u},
    {"block36-expand",4194304u},
    {"block36-projection",1049600u},
    {"block36-qkv",3145760u},
    {"block37-contract",4195328u},
    {"block37-expand",4194304u},
    {"block37-projection",1049600u},
    {"block37-qkv",3145760u},
    {"block38-contract",4195328u},
    {"block38-expand",4194304u},
    {"block38-projection",1049600u},
    {"block38-qkv",3145760u},
    {"block4-attention",8225u},
    {"block4-ds",2048u},
    {"block4-ffn",8736u},
    {"block40-attention",1114640u},
    {"block40-ffwd",524288u},
    {"block40-ffwd-projection",262656u},
    {"block41-attention",1114640u},
    {"block41-ffwd",524288u},
    {"block41-ffwd-projection",262656u},
    {"block42-attention",1114640u},
    {"block42-ffwd",524288u},
    {"block42-ffwd-projection",262656u},
    {"block43-attention",1114640u},
    {"block43-ffwd",524288u},
    {"block43-ffwd-projection",262656u},
    {"block44-attention",1114640u},
    {"block44-ffwd",524288u},
    {"block44-ffwd-projection",262656u},
    {"block45-attention",1114640u},
    {"block45-ffwd",524288u},
    {"block45-ffwd-projection",262656u},
    {"block46-attention",1114640u},
    {"block46-ffwd",524288u},
    {"block46-ffwd-projection",262656u},
    {"block47-attention",1114640u},
    {"block47-ffwd",524288u},
    {"block47-ffwd-projection",262656u},
    {"block48-attention",295176u},
    {"block48-ffn",590080u},
    {"block48-weights",131328u},
    {"block49-attention",295176u},
    {"block49-ffn",590080u},
    {"block5-attention",24642u},
    {"block5-ffn",36928u},
    {"block50-attention",295176u},
    {"block50-ffn",590080u},
    {"block51-attention",295176u},
    {"block51-ffn",590080u},
    {"block52-attention",295176u},
    {"block52-ffn",590080u},
    {"block53-attention",295176u},
    {"block53-ffn",590080u},
    {"block54-attention",295176u},
    {"block54-ffn",590080u},
    {"block55-attention",295176u},
    {"block55-ffn",590080u},
    {"block56-attention",82052u},
    {"block56-ffn",147584u},
    {"block56-weights",32896u},
    {"block57-attention",82052u},
    {"block57-ffn",147584u},
    {"block58-attention",82052u},
    {"block58-ffn",147584u},
    {"block59-attention",82052u},
    {"block59-ffn",147584u},
    {"block6-attention",24642u},
    {"block6-ffn",36928u},
    {"block60-attention",82052u},
    {"block60-ffn",147584u},
    {"block61-attention",82052u},
    {"block61-ffn",147584u},
    {"block62-attention",24642u},
    {"block62-ffn",36928u},
    {"block62-weights",8256u},
    {"block63-attention",24642u},
    {"block63-ffn",36928u},
    {"block64-attention",24642u},
    {"block64-ffn",36928u},
    {"block65-attention",24642u},
    {"block65-ffn",36928u},
    {"block66-attention",8225u},
    {"block66-ffn",8736u},
    {"block66-weights",2080u},
    {"block67-attention",8225u},
    {"block67-ffn",8736u},
    {"block68-attention",8225u},
    {"block68-ffn",8736u},
    {"block69-attention",8225u},
    {"block69-ffn",8736u},
    {"block7-attention",24642u},
    {"block7-ffn",36928u},
    {"block8-attention",24642u},
    {"block8-ds",8192u},
    {"block8-ffn",36928u},
    {"block9-attention",82052u},
    {"block9-ffn",147584u},
    {"decoder39-weights",524800u},
    {"head-matrix",524288u},
    {"post70-attention",8225u},
    {"post70-ffn",8736u},
    {"post70-head",96u},
    {"post70-scales",64u},
};

std::string text(const fs::path& p) { auto s=p.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()}; }
bool regular(const fs::path& p) { std::error_code e;return fs::is_regular_file(p,e); }
bool directory(const fs::path& p) { std::error_code e;return fs::is_directory(p,e); }
NrModelAssetStatus fail(std::string s) { return {false,std::move(s)}; }
NrModelAssetStatus tensor(const fs::path& root,const Tensor& t) {
    const auto f32=root/(std::string(t.name)+".f32");
    std::error_code e;
    const bool has32=fs::exists(f32,e); // an invalid existing f32 must not be hidden by f16
    const auto selected=has32?f32:root/(std::string(t.name)+".f16");
    const auto bytes=fs::file_size(selected,e);
    if(e)return fail("Missing or unreadable tensor: "+text(selected.filename()));
    const auto expected=t.elements*(has32?4u:2u);
    if(bytes!=expected)return fail("Wrong tensor size: "+text(selected.filename())+" (expected "+std::to_string(expected)+" bytes)");
    std::ifstream stream(selected,std::ios::binary);
    if(!stream)return fail("Unreadable tensor: "+text(selected.filename()));
    return {true,{}};
}
template<size_t N> NrModelAssetStatus tensors(const fs::path& root,const Tensor(&table)[N]) {
    for(const auto& t:table){auto status=tensor(root,t);if(!status.ready)return status;}
    return {true,{}};
}
bool readSmallText(const fs::path& p,std::string& contents,uintmax_t limit=1024*1024) {
    std::error_code e;const auto size=fs::file_size(p,e);
    if(e||size>limit)return false;
    std::ifstream file(p,std::ios::binary);if(!file)return false;
    contents.assign(std::istreambuf_iterator<char>(file),{});return !file.bad();
}
NrModelAssetStatus modules(const fs::path& root) {
    constexpr const char* required[]={"c32_prefix_reference.hsaco","multihead-reference.hsaco","deep_reference.hsaco","boundary_reference.hsaco"};
    std::string manifest;
    if(!readSmallText(root/"SHA256SUMS",manifest))return fail("Missing or unreadable HIP/SHA256SUMS");
    std::istringstream lines(manifest);std::string line;std::set<fs::path> listed;
    while(std::getline(lines,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::istringstream fields(line);std::string digest,name;
        if(!(fields>>digest))continue;
        std::getline(fields,name);const auto first=name.find_first_not_of(" \t");
        if(first==std::string::npos)continue;name.erase(0,first);
        const auto rel=fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(name.data()),name.size()));
        if(rel.is_absolute()||rel.has_root_name())continue;
        bool escapes=false;for(const auto& part:rel)if(part=="..")escapes=true;
        if(escapes||rel.extension()!=".hsaco")continue;
        if(!regular(root/rel))return fail("Missing listed HIP module: "+text(rel));
        listed.insert(rel.lexically_normal());
    }
    if(listed.size()<24)return fail("HIP/SHA256SUMS must list at least 24 existing .hsaco modules");
    std::vector<fs::path> architectures;
    for(const auto* arch:{"gfx1200","gfx1201"})if(directory(root/arch))architectures.push_back(root/arch);
    if(architectures.empty())architectures.push_back(root);
    for(const auto& arch:architectures){
        for(const auto* name:required){
            std::error_code e;const auto size=fs::file_size(arch/name,e);
            if(e||size==0)return fail("Missing required HIP kernel: "+text(arch.filename()/name));
        }
    }
    return {true,{}};
}
NrModelAssetStatus shared(const fs::path& root) {
    // FindWeightsDir in the pinned runtime requires this Original sentinel even
    // for Small. The complete Original prewarm dependency is checked below,
    // after the shared files and individual model tensors.
    const auto sentinel=std::find_if(std::begin(originalTensors),std::end(originalTensors),[](const auto& t){return std::string_view(t.name)=="block0-ffn";});
    auto status=tensor(root,*sentinel);if(!status.ready)return status;
    const auto shaders=regular(root/"shaders/native_codec_encode.hlsl")?root/"shaders":root;
    for(const auto* name:{"native_codec_encode.hlsl","native_codec_decode.hlsl","native_game_rgb_input.hlsl"}){
        std::string contents;
        if(!readSmallText(shaders/name,contents)||contents.empty())return fail(std::string("Missing codec shader: ")+name);
    }
    for(const auto& p:{root,root/"HIP",root/"modules",root/"native-game-tiled-assets"/"HIP"}){
        if(regular(p/"SHA256SUMS"))return modules(p);
    }
    return fail("Missing HIP module directory and SHA256SUMS");
}
}
NrModelAssets inspectNrModelAssets(const std::filesystem::path& folder) {
    NrModelAssets out;out.directory=folder.lexically_normal();
    if(folder.empty()||!folder.is_absolute()||!directory(folder))out.shared=fail("Select an existing absolute NR model folder");
    else out.shared=shared(folder);
    if(!out.shared.ready){out.smallModel=out.originalModel=out.shared;return out;}
    std::string manifest;
    if(!readSmallText(folder/"small"/"model-manifest.json",manifest)||
        manifest.find("\"model\": \"small\"")==std::string::npos||
        manifest.find("2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f")==std::string::npos)
        out.smallModel=fail("Missing or incompatible small/model-manifest.json (lmxxf 0.44 Small required)");
    else out.smallModel=tensors(folder/"small",smallTensors);
    out.originalModel=tensors(folder,originalTensors);
    // Pinned hip_d3d12_bridge.h:243-246 prewarms both networks for the normal
    // 1920x1080 Small session (padded W=1920,H=1152), even with hotkeys disabled.
    // A Small-only folder would pass the DLL's Small guard and then fail on its
    // first real frame. Require this startup dependency before admitting it.
    if(out.smallModel.ready&&!out.originalModel.ready)
        out.smallModel=fail("Small 1080p startup also requires Original weights: "+out.originalModel.issue);
    return out;
}
NrModelAssets findNrModelAssets(const std::filesystem::path& selectedFolder) {
    if(selectedFolder.empty()||!selectedFolder.is_absolute()||!directory(selectedFolder))return inspectNrModelAssets(selectedFolder);
    std::vector<fs::path> candidates{selectedFolder};
    if(selectedFolder.filename()=="small"||selectedFolder.filename()=="weights")candidates.push_back(selectedFolder.parent_path());
    for(const auto& relative:{"weights","assets","runtime_local/radeon/nr-assets","runtime/amd-nr/assets","runtime_local/amd-nr/assets","amd-nr/assets"})candidates.push_back(selectedFolder/relative);
    auto best=inspectNrModelAssets(selectedFolder);
    for(const auto& path:candidates){
        auto result=inspectNrModelAssets(path);
        if(result.anyReady())return result;
        if(result.shared.ready||(!best.shared.ready&&regular(path/"small/model-manifest.json")))best=std::move(result);
    }
    return best;
}
std::filesystem::path readNrModelAssetsReference(const std::filesystem::path& file,std::string* issue) {
    if(issue)issue->clear();
    std::error_code e;if(!fs::exists(file,e)&&!e)return {};
    std::string contents;
    if(!readSmallText(file,contents,32768)){if(issue)*issue="Cannot read saved NR model folder reference";return {};}
    while(!contents.empty()&&(contents.back()=='\r'||contents.back()=='\n'))contents.pop_back();
    if(contents.empty()||contents.find_first_of("\r\n")!=std::string::npos||contents.find('\0')!=std::string::npos){if(issue)*issue="Saved NR model folder reference is invalid; select the folder again";return {};}
    try{auto path=fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(contents.data()),contents.size()));if(path.is_absolute())return path.lexically_normal();}catch(const std::exception&){}
    if(issue)*issue="Saved NR model folder must be an absolute path; select the folder again";
    return {};
}
}
