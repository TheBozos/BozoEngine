// Execute code generated through BOTH CLI options; original load bytes stay unchanged.
#include "bozo/runtime.h"
#include "entries_functions.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

static void check(bool value) { if(!value) throw std::runtime_error("combined entries/adaptations mismatch"); }
int main(int argc,char **argv) {
    try {
        check(argc==2);
        std::ifstream in(argv[1],std::ios::binary);
        std::vector<char> elf{std::istreambuf_iterator<char>(in),{}};
        check(elf.size()>=0x118);
        uint32_t original;
        std::memcpy(&original,elf.data()+0x10c,4);
        check(original==0x24420001u);
        Runtime rt;
        std::memcpy(rt.memory.ram()+0x100000,elf.data()+0x100,24);
        std::vector<uint8_t> before(rt.memory.flat(),rt.memory.flat()+bozo::kFlatBytes);
        rt.functions.setGenerated(entries_function_table,entries_function_count);
        rt.functions.setResume(entries_resume_table,entries_resume_count);
        R5900Context ctx;
        ctx.r[31].uw[0]=0x200000;
        check(rt.functions.generated(0x100000) && rt.functions.generated(0x10000c));
        bozo_call_indirect(rt.memory.flat(),&ctx,&rt,0x100000);
        check(ctx.r[2].uw[0]==7 && ctx.pc==0x200000);
        ctx.r[2].uw[0]=20;
        bozo_call_indirect(rt.memory.flat(),&ctx,&rt,0x10000c);
        check(ctx.r[2].uw[0]==23 && ctx.pc==0x200000);
        auto resume=rt.functions.resumeEntry(0x10000c);
        check(resume && resume->start==0x100000 && resume->end==0x100018);
        ctx.pc=0x10000c; ctx.r[2].uw[0]=30;
        resume->resume(rt.memory.flat(),&ctx,&rt);
        check(ctx.r[2].uw[0]==33 && ctx.pc==0x200000 && ctx.cycles==0);
        check(!std::memcmp(before.data(),rt.memory.flat(),bozo::kFlatBytes));
        std::puts("combined --entries/--patches: primary, interior indirect, resume, unchanged load bytes/RAM pass");
        return 0;
    } catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
