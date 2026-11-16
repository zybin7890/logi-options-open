#pragma once

// Read the user's installed icon pack. No vendor artwork is embedded in this source.
#include <windows.h>
#include <d3d11.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ring_icon_library {
using Microsoft::WRL::ComPtr;
struct Item { std::wstring name,path; };

inline std::filesystem::path local_data() {
    PWSTR value{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&value))) return {};
    std::filesystem::path result(value);CoTaskMemFree(value);return result;
}
inline std::filesystem::path cache_directory() {
    const auto base=local_data();
    return base.empty()?std::filesystem::path{}:base/L"LogiNative"/L"RingIcons";
}
inline std::vector<Item> enumerate() {
    const auto base=local_data();if(base.empty())return {};
    const auto directory=base/L"Logi"/L"LogiPluginService"/L"Media"/L"Icons"/L"DefaultIcons";
    std::error_code error;std::vector<Item> result;
    for(std::filesystem::directory_iterator it(directory,error),end;!error&&it!=end;it.increment(error)) {
        if(it->is_regular_file(error)&&it->path().extension()==L".svg")
            result.push_back({it->path().stem().wstring(),it->path().wstring()});
    }
    std::ranges::sort(result,{},&Item::name);return result;
}

// One instance belongs to the open icon editor; closing it releases the renderer.
// The background mouse agent neither enumerates the library nor creates a D3D device.
class Rasterizer {
    ComPtr<ID2D1DeviceContext5> context_;
    CLSID encoder_{};
    bool encoder_ready_{};
    bool initialize() {
        if(context_)return true;
        ComPtr<ID3D11Device> d3d;
        if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,nullptr)))return false;
        ComPtr<IDXGIDevice> dxgi;ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> device;ComPtr<ID2D1DeviceContext> context;
        if(FAILED(d3d.As(&dxgi))||FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
           __uuidof(ID2D1Factory1),nullptr,reinterpret_cast<void**>(factory.GetAddressOf())))||
           FAILED(factory->CreateDevice(dxgi.Get(),&device))||
           FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&context))||
           FAILED(context.As(&context_)))return false;
        UINT count{},bytes{};
        if(Gdiplus::GetImageEncodersSize(&count,&bytes)!=Gdiplus::Ok)return false;
        std::vector<unsigned char> buffer(bytes);
        auto* encoders=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        if(Gdiplus::GetImageEncoders(count,bytes,encoders)!=Gdiplus::Ok)return false;
        for(UINT i=0;i<count;++i)if(wcscmp(encoders[i].MimeType,L"image/png")==0) {
            encoder_=encoders[i].Clsid;encoder_ready_=true;break;
        }
        return encoder_ready_;
    }
public:
    std::filesystem::path rasterize(const Item& item,const std::filesystem::path& directory) {
        if(directory.empty())return {};
        static std::unordered_map<std::wstring,std::filesystem::path> cached_paths;
        const auto cache_key=directory.wstring()+L"|"+item.path;
        std::error_code error;
        if(const auto found=cached_paths.find(cache_key);found!=cached_paths.end()&&std::filesystem::is_regular_file(found->second,error))
            return found->second;
        std::ifstream input(std::filesystem::path(item.path),std::ios::binary|std::ios::ate);
        if(!input)return {};
        const auto input_size=input.tellg();
        if(input_size<=0||input_size>150*1024)return {};
        std::string xml(static_cast<std::size_t>(input_size),'\0');
        input.seekg(0);
        if(!input.read(xml.data(),input_size))return {};
        std::uint64_t hash=14695981039346656037ull;
        for(const auto byte:xml){hash^=byte;hash*=1099511628211ull;}
        const auto output=directory/(L"svg-mask-v2-256-"+std::to_wstring(hash)+L".png");
        if(std::filesystem::is_regular_file(output,error)){cached_paths[cache_key]=output;return output;}
        if(!initialize()||!encoder_ready_)return {};
        std::filesystem::create_directories(directory,error);if(error)return {};
        // Give the document a pixel viewport while keeping its original viewBox.
        // Editing the root attributes also avoids SDK-specific SVG COM overload ABI differences.
        const auto root_start=xml.find("<svg"),root_end=xml.find('>',root_start);
        if(root_start==std::string::npos||root_end==std::string::npos)return {};
        auto header=xml.substr(root_start,root_end-root_start);
        const std::regex dimension(R"re(\s+(width|height)\s*=\s*("[^"]*"|'[^']*'))re");
        header=std::regex_replace(header,dimension,"");
        header+=" width=\"256\" height=\"256\"";
        xml.replace(root_start,root_end-root_start,header);
        ComPtr<IStream> stream;stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(xml.data()),static_cast<UINT>(xml.size())));
        if(!stream)return {};
        ComPtr<ID2D1SvgDocument> svg;
        constexpr UINT side=256;
        if(FAILED(context_->CreateSvgDocument(stream.Get(),D2D1::SizeF(side,side),&svg)))return {};
        ComPtr<ID2D1Bitmap1> target,readback;
        D2D1_BITMAP_PROPERTIES1 properties{};
        properties.pixelFormat={DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED};
        properties.dpiX=properties.dpiY=96;properties.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET;
        if(FAILED(context_->CreateBitmap(D2D1::SizeU(side,side),nullptr,0,&properties,&target)))return {};
        properties.bitmapOptions=D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        if(FAILED(context_->CreateBitmap(D2D1::SizeU(side,side),nullptr,0,&properties,&readback)))return {};
        context_->SetTarget(target.Get());context_->BeginDraw();
        context_->Clear(D2D1::ColorF(0,0.f));context_->DrawSvgDocument(svg.Get());
        const auto drawn=context_->EndDraw();context_->SetTarget(nullptr);
        if(FAILED(drawn)||FAILED(readback->CopyFromBitmap(nullptr,target.Get(),nullptr)))return {};
        D2D1_MAPPED_RECT mapped{};if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped)))return {};
        std::vector<unsigned char> pixels(side*side*4);
        for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x) {
            const auto alpha=mapped.bits[y*mapped.pitch+x*4+3];
            auto* pixel=pixels.data()+(y*side+x)*4;
            pixel[0]=pixel[1]=pixel[2]=alpha?255:0;pixel[3]=alpha;
        }
        readback->Unmap();
        Gdiplus::Bitmap bitmap(side,side,side*4,PixelFormat32bppARGB,pixels.data());
        if(bitmap.GetLastStatus()!=Gdiplus::Ok||bitmap.Save(output.c_str(),&encoder_,nullptr)!=Gdiplus::Ok)return {};
        cached_paths[cache_key]=output;
        return output;
    }
};
inline std::filesystem::path rasterize(const Item& item,const std::filesystem::path& directory) {
    Rasterizer renderer;return renderer.rasterize(item,directory);
}
}
