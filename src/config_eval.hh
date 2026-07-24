#pragma once

#include <ostream>

#include "pure.hh"

namespace nixciconfig {

// Prepare the Nix runtime. Must be called once, before 'run'.
void initNixRuntime();

// Read the NixCI configuration the flake reference points at, writing one JSON
// object describing what it concluded to 'out' and any diagnostics to 'err'.
//
// Answers 0 as long as it ran, whatever it concluded: a repository with no
// configuration, or one whose configuration is broken, has still been read
// successfully. A non-zero exit is reserved for the tool itself breaking, and
// comes from an uncaught exception rather than from here.
int run(const Args & args, std::ostream & out, std::ostream & err);

} // namespace nixciconfig
