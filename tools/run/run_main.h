// bozo-run's run, as a function a port's checker can call (TMB_Recomp's tmb-check: its generated code and overrides
// linked in). The generated code's tables (tmb_function_table, tmb_resume_table, tmb_external_table) come from the
// executable that links it.
#pragma once

#include <cstdint>
#include <functional>

namespace bozo::run
{
    // Called on the game's thread after each frame's record is written and compared (framesEnded: the frames ended so
    // far, the record's frame + 1), before the next frame runs: a port's window can hold the run there while it shows
    // and captures that frame's picture (tmb-check --window). Set before main.
    void setFrameHook(std::function<void(uint64_t framesEnded)> hook);

    // The command line of tools/run/README.md. With a port set (bozo/port.h, BOZO_PORT), its overrides run natively
    // (--overrides log answers them from the log as before).
    int main(int argc, char **argv);
}
