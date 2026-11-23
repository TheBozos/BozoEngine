// SPDX-License-Identifier: MIT
// Synthetic native image operations. Hidden video-only window, no input.
#include "bozo/render/renderer.h"
#include "bozo/render/gl.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
using namespace bozo::render;
int main(){try{
    if(!SDL_Init(SDL_INIT_VIDEO))return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    auto *window=SDL_CreateWindow("Bozo image operation tests",64,48,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);if(!window)return 1;
    const auto context=SDL_GL_CreateContext(window);
    if(!context || !gl::load([](const char *s){return reinterpret_cast<void *>(SDL_GL_GetProcAddress(s));}))return 1;
    auto &r=Renderer::instance();r.setEnabled(true);r.setPresentsAlone(true);r.setClearColour(0);
    const Screen screen{0,0,64,48};const float id[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    Material plain;plain.textured=false;plain.depthTest=false;plain.depthWrite=false;plain.blend=Material::Blend::Opaque;
    const auto quad=[&](Material m,uint32_t colour,float x=0,float y=0,float width=64,float height=48,float z=0){
        Vertex v[6];const float xy[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
        for(unsigned i=0;i<6;++i){v[i].x=x+xy[i][0]*width;v[i].y=y+xy[i][1]*height;v[i].z=z;
            v[i].u=xy[i][0];v[i].v=xy[i][1];v[i].rgba=colour;}r.draw(m,id,v,6);
    };
    unsigned failures=0,samples=0;std::array<uint8_t,64*48*4> pixels{};
    const auto present=[&](){r.endFrame();gl::api().Viewport(0,0,64,48);if(!r.present(0,0,64,48))throw std::runtime_error("no frame");
        gl::api().ReadPixels(0,0,64,48,gl::kRgba,gl::kUnsignedByte,pixels.data());};
    const auto expect=[&](unsigned x,unsigned y,int value){++samples;const auto actual=pixels[(y*64+x)*4];if(std::abs(int(actual)-value)>3){
        if(failures<12)std::printf("image pixel %u,%u got%u expected%d\n",x,y,actual,value);++failures;}};
    // Exact three-tap horizontal kernel; every source stripe is four pixels.
    r.beginFrame(screen);auto source=plain;source.target=1;source.targetWidth=16;source.targetHeight=12;
    for(unsigned x=0;x<16;++x)quad(source,(x%2?0x80808080:0x80000000),float(x*4),0,4,48);
    auto blur=plain;blur.target=2;blur.targetWidth=16;blur.targetHeight=12;blur.fromFrame=1;blur.source=1;blur.nearest=true;
    auto kernel=std::make_shared<Material::ImageProcessing>();kernel->radius=1;kernel->weights[0]=0.5f;kernel->weights[1]=0.25f;kernel->step[0]=1.f/16;
    blur.imageProcessing=kernel;quad(blur,0x80808080);
    auto copy=plain;copy.fromFrame=1;copy.source=2;copy.nearest=true;quad(copy,0x80808080);present();
    for(unsigned x=6;x<58;x+=4)expect(x,24,128);
    // Current native depth, not a previous-frame CPU probe. Upper/lower halves
    // select different table entries; alpha comes from the produced image.
    for(unsigned repeat=0;repeat<3;++repeat){
        r.beginFrame(screen);auto geometry=plain;geometry.depthWrite=true;
        quad(geometry,0x80000000,0,0,64,24,screen.depthMax*0.25f);
        quad(geometry,0x80000000,0,24,64,24,screen.depthMax*0.75f);
        auto map=plain;map.target=1;map.targetWidth=uint16_t(16+repeat*8);map.targetHeight=12;
        auto lookup=std::make_shared<Material::ImageProcessing>();lookup->depthLookup=true;lookup->depthScale=256;
        for(unsigned i=0;i<256;++i)lookup->depthWeights[i]=i<128?0.25f:0.75f;
        map.imageProcessing=lookup;quad(map,0x80808080);
        auto composite=plain;composite.blend=Material::Blend::Alpha;auto opacity=std::make_shared<Material::ImageProcessing>();opacity->alphaSource=1;
        composite.imageProcessing=opacity;quad(composite,0x80808080);present();
        for(unsigned x:{8u,24u,40u,56u}){expect(x,36,64);expect(x,12,191);}
    }
    // Depth colours: each half's entry replaces the texel (R from the table, alpha 1.0 = 0x80), and the
    // DestinationBySource blend adds it to the scene times that alpha: 0x40 + 0x80 * 0.5 and 0x10 + 0x80 * 0.75.
    {
        r.beginFrame(screen);auto geometry=plain;geometry.depthWrite=true;
        quad(geometry,0x80404040,0,0,64,24,screen.depthMax*0.25f);
        quad(geometry,0x80404040,0,24,64,24,screen.depthMax*0.75f);
        auto fog=plain;fog.blend=Material::Blend::DestinationBySource;fog.colourMask=7;
        auto lookup=std::make_shared<Material::ImageProcessing>();lookup->depthLookup=true;lookup->depthColours=true;lookup->depthScale=64;
        for(unsigned i=0;i<64;++i){const bool near=i>=32;lookup->depthWeights[4*i]=(near?0x10:0x40)/255.f;lookup->depthWeights[4*i+3]=near?0.75f:0.5f;}
        fog.imageProcessing=lookup;quad(fog,0x80808080);present();
        for(unsigned x:{8u,24u,40u,56u}){expect(x,36,0x40+64);expect(x,12,0x10+96);}
    }
    // Frame depth: a plane between the two scene depths, drawn into an image with the main frame's depth buffer,
    // passes (GEQUAL, larger nearer) only over the farther half. The main depth is unchanged (no depth write), so a
    // second plane farther than both halves passes nowhere; a later ordinary draw into the same image has its own
    // depth again and fills it.
    for(unsigned repeat=0;repeat<2;++repeat){
        r.beginFrame(screen);auto geometry=plain;geometry.depthWrite=true;
        quad(geometry,0x80000000,0,0,64,24,screen.depthMax*0.25f);
        quad(geometry,0x80000000,0,24,64,24,screen.depthMax*0.75f);
        auto plane=plain;plane.target=1;plane.frameDepth=true;plane.depthTest=true;plane.depthWrite=false;
        quad(plane,0x80808080,0,0,64,48,screen.depthMax*0.5f);
        quad(plane,0x80404040,0,0,64,48,screen.depthMax*0.125f);
        auto copy=plain;copy.fromFrame=1;copy.source=1;copy.nearest=true;quad(copy,0x80808080);present();
        for(unsigned x:{8u,24u,40u,56u}){expect(x,36,255);expect(x,12,0);} // readback rows count from the bottom
        r.beginFrame(screen);quad(geometry,0x80000000,0,0,64,48,screen.depthMax);
        auto own=plain;own.target=1;own.depthTest=true;own.depthWrite=false;quad(own,0x80606060,0,0,64,48,screen.depthMax*0.5f);
        quad(copy,0x80808080);present();
        for(unsigned x:{8u,24u,40u,56u}){expect(x,12,191);expect(x,36,191);}
    }
    // GS lerp chain: base copy then truncating 8-bit lerps at 1/16-texel offsets, edges clamped, colour 255 (x2).
    // Exact against a CPU reference of the same integer steps.
    {
        int bytes[16];for(unsigned x=0;x<16;++x)bytes[x]=int((x*53+11)%120);
        r.beginFrame(screen);auto stripes=plain;stripes.target=1;stripes.targetWidth=16;stripes.targetHeight=12;stripes.directColour=true;
        for(unsigned x=0;x<16;++x)quad(stripes,0x80000000u|uint32_t(bytes[x])*0x010101u,float(x*4),0,4,48);
        auto lerps=plain;lerps.target=2;lerps.targetWidth=16;lerps.targetHeight=12;lerps.fromFrame=1;lerps.source=1;lerps.nearest=true;
        auto p=std::make_shared<Material::ImageProcessing>();p->step={1.f/16,0};p->chainColour=255;
        const float offsets[4]={-54/16.f,54/16.f,-23/16.f,23/16.f},alphas[4]={44,33,41,31};p->chainTaps=4;
        for(unsigned i=0;i<4;++i){p->chainOffset[i]=offsets[i];p->chainAlpha[i]=alphas[i];}
        lerps.imageProcessing=p;quad(lerps,0x80808080);
        auto copy=plain;copy.fromFrame=1;copy.source=2;copy.nearest=true;quad(copy,0x80808080);present();
        const auto texel=[&](int x){return bytes[std::clamp(x,0,15)];};
        const auto modulate=[](double v){return std::min(255.0,std::floor(v*255/128));};
        for(int x=0;x<16;++x){double c=modulate(texel(x));
            for(unsigned i=0;i<4;++i){const int sixteenths=int(offsets[i]*16),whole=int(std::floor(sixteenths/16.0)),frac=sixteenths-whole*16;
                const double s=modulate((texel(x+whole)*(16-frac)+texel(x+whole+1)*frac)/16.0);c=std::floor((s-c)*alphas[i]/128)+c;}
            ++samples;const int actual=pixels[(24*64+unsigned(x)*4+2)*4];
            if(actual!=int(c)){if(failures<12)std::printf("lerp chain x=%d got%d expected%d\n",x,actual,int(c));++failures;}}
    }
    // Line batches: one horizontal segment lights its own row only (GL_LINES, one pixel wide).
    {r.beginFrame(screen);auto line=plain;line.lines=true;line.directColour=true;Vertex pair[2]{};
     pair[0].x=4;pair[0].y=20.5f;pair[1].x=60;pair[1].y=20.5f;pair[0].rgba=pair[1].rgba=0x80c8c8c8u;r.draw(line,id,pair,2);present();
     for(unsigned x:{8u,32u,56u}){expect(x,48-1-20,200);expect(x,48-1-24,0);expect(x,48-1-16,0);}}
    // Optional shader state must reset on the next ordinary draw/frame.
    r.beginFrame(screen);quad(plain,0x80202020);present();expect(32,24,64);
    r.releaseGpu();SDL_GL_DestroyContext(context);SDL_DestroyWindow(window);SDL_Quit();
    std::printf("native blur/depth lookup/depth colours/lines/alpha images/frame depth/lerp chain/per-draw resize/reset: %u samples, %u failures\n",samples,failures);return failures?1:0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
