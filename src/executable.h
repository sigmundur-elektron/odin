#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Locate an executable the way a shell would.
//
// This is shutil.which, which the Python implementation used for `npm` and for
// agent CLIs. The Windows half is the part worth stating: a name on PATH is only
// executable if it carries one of the PATHEXT suffixes, and npm ships `npm.cmd`
// rather than `npm.exe`, so a search that only tries `.exe` finds nothing.
//
// Returns an empty path when nothing matches.
std::filesystem::path executable_find(const std::string &name);

// Search a specific list of directories rather than PATH. Used by discovery to
// report tools installed off-PATH - npm global prefixes, Scoop shims,
// ~/.local/bin, and Odin's own .odin/tools.
std::filesystem::path executable_find_in(const std::string &name,
										 const std::vector<std::filesystem::path> &directories);

// The suffixes tried on this platform, in order. Empty string last means "the
// name exactly as given". On POSIX this is just {""}.
const std::vector<std::string> &executable_suffixes();
