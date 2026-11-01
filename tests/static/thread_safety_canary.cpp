// Thread-safety canary (ADR 0009): this file must NOT compile with clang's
// -Wthread-safety as an error. Increment() writes a FORGEDB_GUARDED_BY
// member without holding its mutex; the ctest
// ThreadSafetyCanary.UnguardedAccessIsAnError passes only if clang-cl
// rejects it with "requires holding mutex 'mutex_'", which proves the
// analysis is active. It is not part of any build target.
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace {

class Counter {
 public:
  void Increment() { ++count_; }  // the mistake: mutex_ is not held

  int Read() {
    const forgedb::MutexLock lock(&mutex_);  // the right way
    return count_;
  }

 private:
  forgedb::Mutex mutex_;
  int count_ FORGEDB_GUARDED_BY(mutex_) = 0;
};

}  // namespace

int main() {
  Counter counter;
  counter.Increment();
  return counter.Read();
}
