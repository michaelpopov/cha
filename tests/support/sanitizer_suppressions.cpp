// ThreadSanitizer calls __tsan_default_suppressions() at startup, so linking
// this into a test binary needs no environment variable and no runner support.
//
// Only races inside libraries cha does not compile belong here. A suppression
// that could hide a race in cha's own code does not, because the point of the
// instrumented build is to find exactly those.

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CHA_THREAD_SANITIZER 1
#  endif
#endif
#if defined(__SANITIZE_THREAD__)
#  define CHA_THREAD_SANITIZER 1
#endif

#if defined(CHA_THREAD_SANITIZER)

extern "C" const char* __tsan_default_suppressions();

extern "C" const char* __tsan_default_suppressions() {
    // libstdc++ fills the std::ctype<char> narrow/widen cache lazily and
    // without synchronization. Two threads formatting text at the same time
    // therefore race on it, which any test issuing concurrent HTTP requests
    // will hit. The writes store identical values, the standard library here
    // is not instrumented, and the reports bottom out in libstdc++'s own data
    // segment rather than in cha code.
    return "race:std::ctype<char>::narrow\n"
           "race:std::ctype<char>::widen\n";
}

#endif
