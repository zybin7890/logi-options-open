#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

// Loaded only while the editor or runtime Ring is open. Original font files stay local.
struct RingFonts {
    struct Face {
        std::vector<char> bytes;
        std::unique_ptr<Gdiplus::PrivateFontCollection> collection;
        std::unique_ptr<Gdiplus::FontFamily> family;
    };
    std::map<std::wstring,Face> faces;
    const Gdiplus::FontFamily* family(const std::wstring& name) {
        auto [item,added]=faces.try_emplace(name);auto& face=item->second;
        if(!added)return face.family.get();
        constexpr std::pair<const wchar_t*,const wchar_t*> local[]{
            {L"Brown Logitech Pan Light",L"BrownLogitechPan-Light.otf"},{L"Brown Logitech Pan",L"BrownLogitechPan-Regular.otf"},
            {L"Brown Logitech Pan Medium",L"BrownLogitechPan-Medium.otf"},{L"Brown Logitech Pan Thin",L"BrownLogitechPan-Thin.otf"},
            {L"BrownPro",L"BrownPro-Regular.ttf"},{L"BrownPro Light",L"BrownPro-Light.ttf"},{L"Myriad Pro",L"MyriadPro-Regular.ttf"}
        };
        for(const auto&[family,file]:local)if(name==family){
            wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,std::size(executable));
            std::ifstream input(std::filesystem::path(executable).parent_path()/L"assets"/L"fonts"/file,std::ios::binary|std::ios::ate);
            if(input){const auto size=input.tellg();if(size>0){face.bytes.resize(static_cast<std::size_t>(size));input.seekg(0);input.read(face.bytes.data(),size);}face.collection=std::make_unique<Gdiplus::PrivateFontCollection>();
                if(!face.bytes.empty()&&face.collection->AddMemoryFont(face.bytes.data(),static_cast<INT>(face.bytes.size()))==Gdiplus::Ok){int found{};face.family=std::make_unique<Gdiplus::FontFamily>();if(face.collection->GetFamilies(1,face.family.get(),&found)!=Gdiplus::Ok||!found)face.family.reset();}}
            break;
        }
        if(!face.family){face.family=std::make_unique<Gdiplus::FontFamily>(name.c_str());if(face.family->GetLastStatus()!=Gdiplus::Ok)face.family=std::make_unique<Gdiplus::FontFamily>(L"Microsoft YaHei UI");}
        return face.family.get();
    }
};
