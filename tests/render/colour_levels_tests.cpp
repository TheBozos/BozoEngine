// SPDX-License-Identifier: MIT
#include "bozo/render/renderer.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace bozo::render;
int main(){
    auto &r=Renderer::instance();r.setEnabled(true);
    const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    Vertex v[3]{};Material m;unsigned failures=0;
    const auto publish=[&](const Material &material){r.beginFrame(Screen{});r.draw(material,identity,v,3);r.endFrame();return r.takeFrameDraws().hash;};
    const auto baseline=publish(m);m.levels.gain[0]=7;
    if(publish(m)!=baseline)++failures; // Disabled parameters do not change existing digests.
    m.levels.enabled=true;const auto active=publish(m);
    if(active==baseline)++failures;
    auto held=r.published();m.levels.gain[0]=2;
    if(held->batches[0].material.levels.gain[0]!=7 || publish(m)==active)++failures;
    for(unsigned parameter=0;parameter<9;++parameter){
        Material changed=m;float *fields=parameter<3?changed.levels.gain:parameter<6?changed.levels.high:changed.levels.low;
        fields[parameter%3]=parameter<3?3:0.25f;
        if(publish(changed)==publish(m))++failures;
    }
    for(float bad:{-1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        for(unsigned parameter=0;parameter<9;++parameter){
            Material changed=m;float *fields=parameter<3?changed.levels.gain:parameter<6?changed.levels.high:changed.levels.low;
            fields[parameter%3]=bad;bool rejected=false;r.beginFrame(Screen{});
            try{r.draw(changed,identity,v,3);}catch(const std::invalid_argument &){rejected=true;}
            if(!rejected)++failures;r.endFrame();r.takeFrameDraws();
            if(!r.published()->batches.empty())++failures;
        }
    bool rejected=false;try{r.drawOverlay(m,v,3,nullptr);}catch(const std::invalid_argument &){rejected=true;}
    if(!rejected)++failures;
    for(bool high:{false,true}){
        Material changed=m;(high?changed.levels.high:changed.levels.low)[1]=1.01f;
        rejected=false;try{r.draw(changed,identity,v,3);}catch(const std::invalid_argument &){rejected=true;}
        if(!rejected)++failures;
    }
    std::printf("native colour levels publication/digests/admission: %s\n",failures?"FAILED":"passed");return failures?1:0;
}
