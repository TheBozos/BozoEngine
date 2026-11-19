// SPDX-License-Identifier: MIT
#include "bozo/render/renderer.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace bozo::render;
int main(){
    auto &r=Renderer::instance();r.setEnabled(true);
    const float id[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};Vertex v[3]{};
    const auto publish=[&](Material m){r.beginFrame(Screen{});r.draw(m,id,v,3);r.endFrame();return r.takeFrameDraws().hash;};
    Material m;m.textured=false;m.fromFrame=1;m.target=1;m.targetWidth=32;m.targetHeight=16;
    const auto basic=publish(m);auto p=std::make_shared<Material::ImageProcessing>();m.imageProcessing=p;
    const auto filtered=publish(m);unsigned failures=filtered==basic;
    const auto held=r.published();p->alphaScale=0.25f;
    if(held->batches[0].material.imageProcessing->alphaScale!=1 || publish(m)==filtered)++failures;
    const auto reject=[&](Material bad){r.beginFrame(Screen{});bool caught=false;try{r.draw(bad,id,v,3);}catch(const std::invalid_argument &){caught=true;}
        r.endFrame();r.takeFrameDraws();if(!caught || !r.published()->batches.empty())++failures;};
    for(unsigned which=0;which<19;++which){
        Material bad=m;auto b=std::make_shared<Material::ImageProcessing>(*p);bad.imageProcessing=b;
        const auto nan=std::numeric_limits<float>::quiet_NaN();
        switch(which){case 0:b->radius=17;break;case 1:b->alphaSource=8;break;case 2:b->alphaSource=1;break;
        case 3:b->alphaScale=nan;break;case 4:b->depthScale=-1;break;case 5:b->step[0]=nan;break;
        case 6:b->weights[0]=-1;break;case 7:b->radius=1;bad.fromFrame=0;break;
        case 8:b->depthLookup=true;b->radius=1;break;case 9:b->depthLookup=true;b->depthWeights[200]=1.1f;break;
        case 10:bad.targetHeight=0;break;case 11:bad.targetWidth=5000;break;case 12:bad.target=0;break;
        // Lerp chains: off-axis step, an offset off the 1/16 grid, a fractional alpha, too many taps.
        case 13:b->chainTaps=1;b->step={0.5f,0.5f};break;case 14:b->chainTaps=1;b->step={1,0};b->chainOffset[0]=0.01f;break;
        case 15:b->chainTaps=1;b->step={1,0};b->chainAlpha[0]=0.5f;break;case 16:b->chainTaps=33;b->step={1,0};break;
        // Depth colours: without a depth lookup; an alpha above 1 (GS 0x80).
        case 17:b->depthColours=true;break;case 18:b->depthLookup=true;b->depthColours=true;b->depthWeights[7]=1.5f;break;}
        reject(bad);
    }
    // Depth colours: the mode is part of the digest.
    {Material lookup=m;lookup.fromFrame=0;auto grey=std::make_shared<Material::ImageProcessing>();grey->depthLookup=true;lookup.imageProcessing=grey;
     const auto greyHash=publish(lookup);auto colours=std::make_shared<Material::ImageProcessing>(*grey);colours->depthColours=true;
     lookup.imageProcessing=colours;const auto colourHash=publish(lookup);
     auto plainGrey=std::make_shared<Material::ImageProcessing>(*grey);lookup.imageProcessing=plainGrey;
     if(colourHash==greyHash || colourHash==publish(lookup) || r.published()->batches.empty())++failures;}
    bool caught=false;try{r.drawOverlay(m,v,3,nullptr);}catch(const std::invalid_argument &){caught=true;}if(!caught)++failures;
    std::printf("native image publication, immutable parameters, digests and 19 admission guards: %u failures\n",failures);return failures?1:0;
}
