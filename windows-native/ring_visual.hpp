#pragma once

#include "ring.hpp"
#include "native_icons.hpp"
#include "ring_fonts.hpp"

inline COLORREF ring_rgb(unsigned value) {
    return RGB((value>>16)&255,(value>>8)&255,value&255);
}

inline void draw_ring_visual(Gdiplus::Graphics& graphics,const RingIcon& icon,
                             Gdiplus::Image* image,float x,float y,float size,RingFonts* fonts=nullptr) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if(icon.background) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255,(*icon.background>>16)&255,
                                               (*icon.background>>8)&255,*icon.background&255));
        graphics.FillEllipse(&fill,x,y,size,size);
    }
    const auto state=graphics.Save();
    Gdiplus::GraphicsPath clip;clip.AddEllipse(x,y,size,size);graphics.SetClip(&clip);
    float visual_size=size*(icon.show_text&&icon.layout<2?.64f:icon.layout==3?1.f:.57f)*icon.scale/100.f;
    const float visual_x=x+(size-visual_size)/2+size*icon.offset_x/100.f;
    const float visual_y=y+(icon.show_text&&icon.layout<2?(icon.layout==1?size*.35f:0):(size-visual_size)/2)+size*icon.offset_y/100.f;
    if(icon.show_icon&&image&&image->GetLastStatus()==Gdiplus::Ok&&image->GetWidth()&&image->GetHeight()) {
        const float scale=visual_size/(icon.layout==3?std::min(image->GetWidth(),image->GetHeight()):std::max(image->GetWidth(),image->GetHeight()));
        const float width=image->GetWidth()*scale,height=image->GetHeight()*scale;
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        if(icon.glyph==L"original-svg") {
            Gdiplus::ColorMatrix matrix{{{(icon.color>>16&255)/255.f,0,0,0,0},{0,(icon.color>>8&255)/255.f,0,0,0},{0,0,(icon.color&255)/255.f,0,0},{0,0,0,1,0},{0,0,0,0,1}}};
            Gdiplus::ImageAttributes attributes;attributes.SetColorMatrix(&matrix);
            graphics.DrawImage(image,Gdiplus::RectF(visual_x+(visual_size-width)/2,visual_y+(visual_size-height)/2,width,height),0,0,static_cast<float>(image->GetWidth()),static_cast<float>(image->GetHeight()),Gdiplus::UnitPixel,&attributes);
        } else graphics.DrawImage(image,visual_x+(visual_size-width)/2,visual_y+(visual_size-height)/2,width,height);
    } else if(icon.show_icon&&icon.glyph==L"stop") {
        const auto color=ring_rgb(icon.color);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
        graphics.FillRectangle(&fill,visual_x+visual_size/4,visual_y+visual_size/4,visual_size/2,visual_size/2);
    } else if(icon.show_icon)draw_native_icon(graphics,icon.glyph.empty()?L"keyboard":icon.glyph,visual_x,visual_y,visual_size,ring_rgb(icon.color));
    if(icon.show_text&&!icon.text.empty()) {
        const auto style=icon.bold?Gdiplus::FontStyleBold:Gdiplus::FontStyleRegular;
        Gdiplus::FontFamily fallback(icon.font_family.c_str());
        const auto family=fonts?fonts->family(icon.font_family):fallback.GetLastStatus()==Gdiplus::Ok?&fallback:Gdiplus::FontFamily::GenericSansSerif();
        Gdiplus::Font font(family,size*icon.font_size/256.f,family->IsStyleAvailable(style)?style:Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255,(icon.text_color>>16)&255,(icon.text_color>>8)&255,icon.text_color&255));
        Gdiplus::StringFormat format;format.SetAlignment(Gdiplus::StringAlignmentCenter);format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        const auto rect=icon.layout<2?Gdiplus::RectF(x,y+(icon.layout==0?size*.65f:0),size,size*.35f):Gdiplus::RectF(x,y,size,size);
        graphics.DrawString(icon.text.c_str(),-1,&font,rect,&format,&brush);
    }
    graphics.Restore(state);
}
