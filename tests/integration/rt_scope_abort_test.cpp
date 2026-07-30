// Proves the DEFAULT violation handler really does abort the process.
//
// This cannot live in the Catch2 suite: the default handler calls abort(), which
// would take the whole test binary down. It is a standalone executable whose
// ctest entry uses PASS_REGULAR_EXPRESSION, so the test passes when the expected
// message reaches stderr — regardless of the SIGABRT exit status.
//
// Without this, every other RT_SCOPE test would be exercising only the
// test-installed counting handler, and the production path would be untested.

#include "rt/rt_scope.hpp"

#include <cstdio>
#include <new>
#include <vector>

namespace {

// The compiler is allowed to elide calls to operator new whose result is never
// observed ([expr.new]/10), and at -O2 it does — an earlier version of this test
// passed vacuously for exactly that reason. Storing through a volatile pointer
// makes the allocation escape, so it must actually happen.
volatile void* g_sink = nullptr;

}  // namespace

int main() {
  // Deliberately NOT installing a handler: the default must be in force.
  std::vector<int> values;
  values.push_back(1);  // allocates -> default handler -> write + abort
  g_sink = values.data();

  {
    RT_SCOPE();
    void* block = ::operator new(64);
    g_sink = block;

    values.push_back(2);
    values.push_back(3);
    values.push_back(4);
    g_sink = values.data();

    ::operator delete(block);
  }

  // Unreachable. If we get here the guard failed to fire, so fail loudly rather
  // than exiting 0 and letting the suite report a false pass.
  std::fputs("ERROR: allocation inside RT_SCOPE did not abort\n", stderr);
  return 1;
}
