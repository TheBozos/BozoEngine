// SPDX-License-Identifier: MIT
#include "bozo/native/compiled_libraries.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
using namespace bozo::native::irx;
namespace {
void check(bool v,const char *what){if(!v)throw std::runtime_error(std::string("native disc files: ")+what);}
template<class F>void rejects(F f,const char *message){bool yes=false;try{f();}catch(const std::runtime_error &e){yes=std::strstr(e.what(),message)!=nullptr;}check(yes,message);}
constexpr uint32_t entry=0xe000,callback=0xe100,readCall=0xe800,syncCall=0xe808,errorCall=0xe810,callbackCall=0xe818,
    suspendCall=0xe820,resumeCall=0xe828,mode=0x30000,buffer=0x31000,token=0x33000;
void unused(Env &){throw std::runtime_error("unbound fixture entry");}
const Entry entries[]={{entry,unused},{callback,unused}};
const Registration registration("native-disc-files",1,0xe000,0x1000,entries,2);
const Entry foreignEntries[]={{0xf000,unused}};
const Registration foreignRegistration("unadmitted-disc-callback",2,0xf000,0x100,foreignEntries,1);
uint32_t call(Env &e,uint32_t function,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0){
    e.cpu.gpr[4]=a;e.cpu.gpr[5]=b;e.cpu.gpr[6]=c;e.cpu.gpr[7]=d;e.cpu.gpr[31]=0xe004;
    check(e.call(function,0xe004),"unexpected yield");return e.cpu.gpr[2];
}
struct Iso {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("bozo-native-disc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    std::vector<uint8_t> bytes=std::vector<uint8_t>(32*2048);
    Iso(){for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t((i*7+i/2048*13)&255);
        bytes[16*2048]=1;std::memcpy(bytes.data()+16*2048+1,"CD001",5);
        std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());check(bool(out),"write synthetic ISO");}
    ~Iso(){std::error_code ec;std::filesystem::remove(path,ec);}
};
struct Fixture {
    Iso iso;bozo::libs::Libraries libraries;CompiledLibraries host{libraries,0x40000};
    unsigned notifications=0;bool excluded=false,reentrant=false;uint32_t parentStack=0;
    Fixture(bool enabled,bool exclude=false,bool again=false):excluded(exclude),reentrant(again){
        libraries.cd.imagePath=iso.path.string();libraries.cd.readEnd=0x12345678;libraries.cd.readBytes=99;
        Image image{"native-disc-files",1,0xe000,entry,0x12340,std::vector<uint8_t>(0x1000)};
        image.imports={{readCall,"cdvdman",6},{syncCall,"cdvdman",11},{errorCall,"cdvdman",8},{callbackCall,"cdvdman",37},
            {suspendCall,"intrman",17},{resumeCall,"intrman",18}};
        host.addImage(std::move(image));if(enabled)host.useNativeDiscFiles();
        host.bindNative(callback,[this](Env &e){const auto ret=e.cpu.gpr[31];
            check(e.cpu.gpr[4]==1 && e.cpu.gpr[28]==0x12340 && e.cpu.gpr[29]!=parentStack,"isolated native completion context");
            for(unsigned i=0;i<2048;++i)check(e.r8(buffer+i)==iso.bytes[20*2048+i],"callback follows actual byte copy");
            check(call(e,syncCall,1)==0,"completed actual read has no outstanding request");
            if(reentrant && ++notifications==1)check(call(e,readCall,21,1,buffer+2048,mode)==1,"reentrant actual read");
            else if(!reentrant)++notifications;
            e.cpu.pc=ret;
        });
        host.bindNative(entry,[this](Env &e){const auto ret=e.cpu.gpr[31];parentStack=e.cpu.gpr[29];
            check(call(e,syncCall,0)==0 && call(e,syncCall,0x11)==0,"initial owned request set is empty");
            check(call(e,callbackCall,callback)==0,"install native completion");
            e.w8(mode,0);e.w8(mode+1,1);e.w8(mode+2,0);
            if(excluded)check(call(e,suspendCall,token)==0,"exclude callbacks");
            check(call(e,readCall,20,2,buffer,mode)==1,"actual ISO read accepted");
            check(e.cpu.gpr[29]==parentStack && e.cpu.gpr[28]==0x12340,"caller context survives callback");
            if(excluded){check(notifications==0 && call(e,syncCall,1)==0,"copied data completes independently of deferred callback");
                call(e,resumeCall,e.r32(token));}
            check(notifications==(reentrant?2u:1u),"actual completion count");
            for(unsigned i=0;i<4096;++i)check(e.r8(buffer+i)==iso.bytes[20*2048+i],"exact native file contents");
            check(call(e,errorCall)==0,"read succeeded");
            const auto count=notifications;
            check(call(e,readCall,31,2,buffer,mode)==0 && call(e,errorCall)==0x30 && call(e,syncCall,0)==0,"real EOF failure completes without fabricated data");
            check(notifications==count,"rejected read has no completion");
            for(unsigned i=0;i<4096;++i)check(e.r8(buffer+i)==iso.bytes[20*2048+i],"failed read preserves destination");
            rejects([&]{call(e,readCall,20,0,buffer,mode);},"count outside");
            e.w8(mode+2,1);rejects([&]{call(e,readCall,20,1,buffer,mode);},"sector format");e.w8(mode+2,0);
            rejects([&]{call(e,readCall,20,1,0x1fffff,mode);},"outside");
            rejects([&]{call(e,callbackCall,0x55555);},"admitted linked");
            rejects([&]{call(e,callbackCall,0xf000);},"admitted linked");
            rejects([&]{call(e,callbackCall,readCall);},"admitted linked");
            check(call(e,callbackCall,0)==callback,"invalid replacement preserved callback");
            check(call(e,readCall,20,1,buffer,mode)==1 && call(e,errorCall)==0 && notifications==count,"success clears real error; removed callback stays removed");
            check(libraries.cd.readEnd==0x12345678 && libraries.cd.readBytes==99,"EE drive-time state untouched");
            e.cpu.gpr[2]=1;e.cpu.pc=ret;
        });
    }
    void run(){check(host.startImage("native-disc-files")==1,"completed native entry");}
};
}
int main(){try{
    Fixture disabled(false);rejects([&]{disabled.run();},"unsupported native IRX import cdvdman:11");
    Fixture plain(true),excluded(true,true),reentrant(true,false,true);
    plain.run();check(excluded.notifications==0 && reentrant.notifications==0,"per-host completion ownership");excluded.run();reentrant.run();
    rejects([&]{plain.host.useNativeDiscFiles();},"before execution");
    plain.host.reset();plain.notifications=0;plain.run();
    Fixture missing(true);missing.libraries.cd.imagePath=missing.iso.path.string()+".missing";
    missing.host.bindNative(entry,[&](Env &e){const auto ret=e.cpu.gpr[31];
        e.w32(mode,0);for(unsigned i=0;i<2048;++i)e.w8(buffer+i,0x5a);
        check(call(e,callbackCall,callback)==0,"missing-file callback install");
        check(call(e,readCall,20,1,buffer,mode)==0 && call(e,errorCall)==0x30 && call(e,syncCall,1)==0,"missing native ISO is an actual read failure");
        for(unsigned i=0;i<2048;++i)check(e.r8(buffer+i)==0x5a,"missing-file read preserves destination");
        check(!missing.notifications,"missing-file read has no fabricated callback");e.cpu.gpr[2]=1;e.cpu.pc=ret;
    });missing.run();
    Fixture truncated(true);std::string openError;
    check(truncated.libraries.cd.disc.open(truncated.iso.path.string(),openError),"open ISO before a real short-read failure");
    std::filesystem::resize_file(truncated.iso.path,20*2048+100);
    truncated.host.bindNative(entry,[&](Env &e){const auto ret=e.cpu.gpr[31];e.w32(mode,0);e.w32(buffer,0x5a5a5a5a);
        check(call(e,callbackCall,callback)==0 && call(e,readCall,21,1,buffer,mode)==0,"actual host short read fails");
        check(e.r32(buffer)==0x5a5a5a5a && call(e,errorCall)==0x30 && call(e,syncCall)==0 && !truncated.notifications,"short read preserves bytes and owns no pending notification");
        e.cpu.gpr[2]=1;e.cpu.pc=ret;
    });truncated.run();
    std::puts("actual native ISO bytes, sync/error/EOF, callbacks/exclusions/reentrancy, reset, isolation and default rejection passed");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
