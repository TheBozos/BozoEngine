// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The port as a PC game (moved from the team's ps2_app, phase 4 milestone M2): a per-user folder for the memory
// cards, the settings (input.ini, video.ini) and the logs, the game data found without an argument, the console's
// output also in a log file, a crash report and message boxes instead of a silent exit. The port names itself with
// setIdentity() first thing in main (the fork took the same names from CMake's PS2X_APP_* definitions); with no
// folder the runner behaves as a tool (configured() false, userDirectory() empty).
#pragma once

#include <filesystem>
#include <string>

namespace bozo::app
{
    struct Identity
    {
        std::string title;      // the window title ("Twisted Metal: Black")
        std::string folder;     // the user folder's name on Windows: %APPDATA%\<folder> ("TMB_Recomp")
        std::string xdgFolder;  // elsewhere: $XDG_DATA_HOME/<xdgFolder> (empty: folder)
        std::string bootElf;    // the game's boot ELF ("SCUS_971.01"), for findGameData
        std::string dataFolder = "game"; // where the game data is looked for, beside the executable or the user folder
    };

    // Before anything below (the user folder is fixed at its first use).
    void setIdentity(const Identity &identity);
    const Identity &identity();

    // Whether the port named a user folder.
    bool configured();

    // The window title (empty when not configured).
    const char *title();

    // The per-user folder, created on first use: PS2X_USER_DIR, else %APPDATA%\<folder> on Windows,
    // $XDG_DATA_HOME/<xdgFolder> (~/.local/share/...) elsewhere. Empty when not configured.
    const std::filesystem::path &userDirectory();

    // The folder of the running executable.
    std::filesystem::path executableDirectory();

    // The game's boot ELF with no argument: <exe>/<dataFolder>/<bootElf>, then the same under the user folder. Empty
    // when neither exists; searched lists the places looked at.
    std::filesystem::path findGameData(std::string *searched = nullptr);

    // Copies the memory cards (mc0, mc1) and the settings (input.ini, video.ini) from the folder they were kept in
    // before (the ELF's) to the user folder, once per old folder, never overwriting and never deleting.
    void migrateFrom(const std::filesystem::path &oldDirectory);

    // Everything written to stdout and stderr also goes to <user folder>/logs/<folder>.log (the last 3 runs kept:
    // <name>.log, <name>.1.log, <name>.2.log), the terminal still getting it when there is one. On Windows, a
    // GUI-subsystem runner started from cmd or PowerShell attaches to their console. PS2X_LOG_FILE=<path> writes
    // there instead, PS2X_LOG_FILE=0 writes no file.
    void startLogging();
    // The log file in use (empty if none).
    const std::filesystem::path &logPath();
    // Writes out what is still in the pipes to the log (before the process exits).
    void flushLog();

    // Windows: an unhandled-exception filter writing <user folder>/crashes/crash-<time>.txt (exception, address and
    // module, the stack, the last 200 log lines) and showing a message box naming it. PS2X_TEST_CRASH=<seconds>
    // crashes a thread on purpose after that long, to test it.
    void installCrashHandler();
    // The same report for a fatal error that is not a CPU exception (std::terminate, a fatal exception in main).
    void reportFatal(const std::string &reason);

    // A message box on Windows (stderr elsewhere), the text in the log either way; PS2X_NO_DIALOGS=1: the log only
    // (unattended runs).
    void showMessage(const std::string &text, bool error);
}
