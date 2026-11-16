#pragma once
#include <string>

// Human-readable view of a Pure Data patch. The patch itself is built in Pd; this view only shows what is useful in CAW and lets you
// change the text of the objects and messages (the arguments, the names of [r] / [s], the numbers). Positions and connections stay as they are.
//
//   1: obj    r freq
//   2: obj    osc~
//   3: msg    440
//   ...
//   connections:
//   1 -> 2
std::string pdToReadable(const std::string& patch);

// Writes the edited object / message / comment texts of `readable` back into `patch`. Lines are matched by their number.
// Returns the new patch text (the same patch when nothing changed).
std::string pdApplyReadable(const std::string& patch, const std::string& readable);

// A small empty patch for "Edit in Pd" when the clip has no patch yet.
std::string pdEmptyPatch();
