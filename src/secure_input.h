#pragma once
#include <string>

// Read a line from the terminal without echoing it.
//
// This is what stops a provider key appearing in a shared screen, a scrollback
// buffer, or a recorded terminal session. It is the one piece of `auth set` that
// cannot be done portably with the standard library.
//
// Falls back to a plain read when stdin is not a terminal - a piped secret is
// already not being echoed by us - and reports that through out_echoed so the
// caller can decide whether to warn.
//
// Returns false only when input could not be read at all (EOF on a closed
// stdin), which is distinct from reading an empty line.
bool secure_input_read(const std::string &prompt, std::string &out_secret, bool &out_echoed);
