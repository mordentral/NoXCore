// Where noxsdl's log (SDL_Log) goes. Always to noxcore-log.txt next to the exe (else in SDL's
// per-user folder), emptied each run, so a run started by double-clicking leaves a log; and to
// stderr.
//
// On Windows noxsdl is a GUI program, with no console window of its own. Its log lines go to
// stderr when whoever started it redirected stderr (a file or a pipe: a script's
// Start-Process -RedirectStandardError, a shell's 2> file), and with -cmd (or --cmd) to a console
// as well: the terminal it was started from, else a new console window. Elsewhere stderr is the
// terminal as usual and -cmd changes nothing.
#pragma once

#include <string_view>

namespace noxsdl {

/// Whether `arg` is -cmd / --cmd (a console for the log on Windows).
bool isConsoleFlag(std::string_view arg);

/// Sets the log's outputs up from the command line: the first thing at start-up, before anything
/// is logged.
void startLog(int argc, char* argv[]);

} // namespace noxsdl
