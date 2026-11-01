// Clang's thread-safety analysis (TODO 4.6; docs/adr/0009).
//
// These attributes say which mutex guards which data and which functions
// need it held. clang and clang-cl check them at compile time with
// -Wthread-safety (on for every ForgeDB target built with clang-cl, and an
// error there under /WX); MSVC ignores them. The analysis is the nearest
// thing to a data-race check on Windows, where ThreadSanitizer does not run.
//
//   Mutex mutex_;
//   int count_ FORGEDB_GUARDED_BY(mutex_);
//   void Bump() FORGEDB_REQUIRES(mutex_);   // callers must hold mutex_
//   void Tick() FORGEDB_EXCLUDES(mutex_);   // callers must not
//
// Lambdas are analyzed as separate functions that hold nothing: code under
// a lock should not touch guarded data from inside a lambda (write loops
// around CondVar::Wait instead of predicates).
//
// See https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
#pragma once

// NOLINTBEGIN(cppcoreguidelines-macro-usage): attributes can only be macros

#ifdef __clang__
#define FORGEDB_THREAD_ANNOTATION(x) __attribute__((x))
#else
#define FORGEDB_THREAD_ANNOTATION(x)
#endif

// On a class: it is a capability (a mutex).
#define FORGEDB_CAPABILITY(name) FORGEDB_THREAD_ANNOTATION(capability(name))
// On a class: an RAII holder of a capability (MutexLock).
#define FORGEDB_SCOPED_CAPABILITY FORGEDB_THREAD_ANNOTATION(scoped_lockable)
// On a member: read or written only with the mutex held.
#define FORGEDB_GUARDED_BY(x) FORGEDB_THREAD_ANNOTATION(guarded_by(x))
// On a pointer member: what it points to is guarded (the pointer is not).
#define FORGEDB_PT_GUARDED_BY(x) FORGEDB_THREAD_ANNOTATION(pt_guarded_by(x))
// On a function: callers hold these.
#define FORGEDB_REQUIRES(...) FORGEDB_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
// On a function: callers do not hold these (it takes them itself).
#define FORGEDB_EXCLUDES(...) FORGEDB_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))
// On a function: it takes / releases the capability.
#define FORGEDB_ACQUIRE(...) FORGEDB_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define FORGEDB_RELEASE(...) FORGEDB_THREAD_ANNOTATION(release_capability(__VA_ARGS__))
// On a function: when it returns, the caller holds the capability (an
// assertion the analysis trusts).
#define FORGEDB_ASSERT_CAPABILITY(x) FORGEDB_THREAD_ANNOTATION(assert_capability(x))
// On a function: not analyzed. Every use says why.
#define FORGEDB_NO_THREAD_SAFETY_ANALYSIS FORGEDB_THREAD_ANNOTATION(no_thread_safety_analysis)

// NOLINTEND(cppcoreguidelines-macro-usage)
