// bozo-run's run, as a function a port's checker can call (TMB_Recomp's tmb-check: its generated code and overrides
// linked in). The generated code's tables (tmb_function_table, tmb_resume_table, tmb_external_table) come from the
// executable that links it.
#pragma once

namespace bozo::run
{
    // The command line of tools/run/README.md. With a port set (bozo/port.h, BOZO_PORT), its overrides run natively
    // (--overrides log answers them from the log as before).
    int main(int argc, char **argv);
}
