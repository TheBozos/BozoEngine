#include "bozo/libs/libraries.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/recomp.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <stdexcept>
#include <cstdio>
using namespace bozo;
namespace {
void check(bool v){if(!v)throw std::runtime_error("native CD callback assertion failed");}
struct Done {};
struct TestHost:ee::Host {
    libs::Libraries l;unsigned callbacks=0;
    void libraryCall(ee::Dispatcher &d,R5900Context *c,uint32_t pc) override {
        check(pc==0x300020);
        libs::Guest g(d.runtime(),c);l.find("sceCdRead")->handler(l,g);
        check(c->r[2].sd[0]==1 && callbacks==0);
        l.find("sceCdGetReadPos")->handler(l,g);
        check(c->r[2].uw[0]==2048);
        d.setNow(c,d.now(c)+g.cycles);
    }
    void threadEnded(ee::Dispatcher &,ee::Thread &) override {throw Done{};}
    void vu0Microprogram(ee::Dispatcher &,R5900Context *,uint32_t) override {throw std::runtime_error("VU");}
    void ioRead(ee::Dispatcher &,R5900Context *,uint32_t,uint32_t,uint64_t[2]) override {throw std::runtime_error("IO");}
    void ioWrite(ee::Dispatcher &,R5900Context *,uint32_t,uint32_t,const uint64_t[2]) override {throw std::runtime_error("IO");}
    void fault(ee::Dispatcher &,R5900Context *,const std::string &s) override {throw std::runtime_error(s);}
};
void callback(uint8_t *ram,R5900Context *c,Runtime *rt) {
    auto &d=dynamic_cast<ee::Dispatcher &>(*rt->services);
    auto &host=dynamic_cast<TestHost &>(d.host());
    check(c->r[4].uw[0]==1 && c->r[28].uw[0]==0x123456 && c->r[29].uw[0]==0x221000);
    check(d.now(c)>=libs::kSectorCycles && ram[0x110000]==0x5a);
    ++host.callbacks;c->pc=0;
}
void mainThread(uint8_t *ram,R5900Context *c,Runtime *rt) {
    if(c->pc==0x100000) {
        c->r[4].uw[0]=22;c->r[5].uw[0]=1;c->r[6].uw[0]=0x110000;
        c->r[31].uw[0]=0x100008;
        bozo_call_indirect(ram,c,rt,0x300020);
        if(bozo::checkpoint(ram,c,rt,8,0x100008))return;
    }
    auto &d=dynamic_cast<ee::Dispatcher &>(*rt->services);
    check(dynamic_cast<TestHost &>(d.host()).callbacks==1);c->pc=0;
}
}
int main() {
 try {
    const auto path=std::filesystem::temp_directory_path()/("bozo-cd-callback-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove(p,e);}} cleanup{path};
    std::vector<uint8_t> image(24*2048);auto *pvd=image.data()+16*2048;
    pvd[0]=1;std::memcpy(pvd+1,"CD001",5);pvd[6]=1;
    std::memset(image.data()+22*2048,0x5a,2048);
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char *>(image.data()),image.size());}
    TestHost host;ee::Dispatcher d(host);host.l.attach(d);host.l.cd.imagePath=path.string();
    d.boot(0x100000,0x1fff000);
    R5900Context c;c.r[28].uw[0]=0x123456;
    auto call=[&](const char *name,std::initializer_list<uint32_t> args) {
        unsigned i=4;for(auto arg:args)c.r[i++].uw[0]=arg;
        libs::Guest g(d.runtime(),&c);auto *f=host.l.find(name);check(f);f->handler(host.l,g);
        return c.r[2].sw[0];
    };
    check(call("sceCdStatus",{})==0);
    check(call("sceCdGetReadPos",{})==0);
    check(call("sceCdInit",{0})==1 && call("sceCdStatus",{})==0x0a);
    host.l.cd.readEnd=100;check(call("sceCdStatus",{})==6);
    host.l.cd.readEnd=0;host.l.cd.streaming=true;check(call("sceCdStatus",{})==6);
    host.l.cd.streaming=false;host.l.cd.error=-1;check(call("sceCdStatus",{})==0x20);
    check(call("sceCdInit",{2})==1 && call("sceCdStatus",{})==0);
    check(call("sceCdInit",{0})==1 && call("sceCdStatus",{})==0x0a);
    check(call("sceCdInitEeCB",{128,0x220000,0x1000})==-1 && !host.l.cd.callbackConfigured);
    check(call("sceCdInitEeCB",{32,0xfffffff0,0x1000})==-1 && !host.l.cd.callbackConfigured);
    check(call("sceCdInitEeCB",{32,0x1fffff0,0x1000})==-1 && !host.l.cd.callbackConfigured);
    check(call("sceCdInitEeCB",{32,0x220000,0x1000})==1);
    check(call("sceCdInitEeCB",{32,0x230000,0x1000})==0 && host.l.cd.callbackStack==0x221000);
    check(call("sceCdCallback",{0x155000})==0);
    check(call("sceCdCallback",{0x155004})==0x155000);
    host.l.cd.readEnd=100;
    check(call("sceCdCallback",{0})==0 && host.l.cd.callback==0x155004);
    host.l.cd.readEnd=0;
    check(call("sceCdCallback",{0})==0x155004 && host.l.cd.callback==0);
    check(call("sceCdCallback",{0x155000})==0);
    libs::Libraries independent;check(!independent.cd.callback && !independent.cd.callbackConfigured);
    const BozoFunctionEntry funcs[]={{0x100000,mainThread},{0x155000,callback}};
    const BozoResumeEntry resumes[]={{0x100000,0x100010,mainThread},{0x155000,0x155010,callback}};
    d.setFunctions(funcs,2,resumes,2);d.bindLibrary(0x300020);
    try{d.run();check(false);}catch(const Done &){}
    check(host.callbacks==1 && !d.kernel().handlerQueued());
    check(call("sceCdGetReadPos",{})==0);
    std::puts("native CD callback registration/configuration/isolation and real read completion passed");return 0;
 }catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
