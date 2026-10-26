#pragma once
// The test's checks: CHECK(cond, msg) counts a check and, on a miss, prints "FAIL: msg (file:line)" at once;
// jlink_test::report() prints the summary line and returns the exit code (0 = no check failed).
#include <cstdio>

namespace jlink_test {
struct Counts {
    int checks;
    int failures;
};

inline Counts& counts() {
    static Counts c{0, 0};
    return c;
}

inline void check(bool        ok,
                  char const* msg,
                  char const* file,
                  int         line) {
    ++counts().checks;
    if(!ok) {
        ++counts().failures;
        std::printf("FAIL: %s (%s:%d)\n", msg, file, line);
        std::fflush(stdout);
    }
}

inline int report() {
    auto const& c = counts();
    if(c.failures == 0) {
        std::printf("ALL OK (%d checks)\n", c.checks);
    } else {
        std::printf("%d FAILURES (%d checks)\n", c.failures, c.checks);
    }
    return c.failures == 0 ? 0 : 1;
}
}   // namespace jlink_test

#define CHECK(cond, msg) ::jlink_test::check(static_cast<bool>(cond), msg, __FILE__, __LINE__)
