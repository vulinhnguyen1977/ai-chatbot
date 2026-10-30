// selftest.h -- built-in consistency checks, run with `mstutor selftest`.
//
// These ship with the binary so the client can verify a build on their own
// machine without a test framework or a network connection. Nothing here
// contacts Ollama or an SMTP server.
#pragma once

namespace selftest {

// Returns true when every check passed. Progress is printed to stdout.
bool run_all();

}  // namespace selftest
