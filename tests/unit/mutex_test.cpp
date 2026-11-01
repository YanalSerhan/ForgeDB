// Mutex, MutexLock and CondVar (src/util/mutex.h; ADR 0009). Their
// annotations are checked by the compiler (clang-cl, -Wthread-safety) and by
// the ThreadSafetyCanary test; these tests check that they lock and wake.
#include "util/mutex.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "util/thread_annotations.h"

namespace forgedb {
namespace {

class Counter {
 public:
  void Add(int n) {
    const MutexLock lock(&mutex_);
    count_ += n;
  }
  [[nodiscard]] int Get() {
    const MutexLock lock(&mutex_);
    return count_;
  }

  // Unlock() and Lock() within one MutexLock's scope.
  [[nodiscard]] int AddAroundAGap(int n) {
    MutexLock lock(&mutex_);
    const int before = count_;
    lock.Unlock();
    std::this_thread::yield();  // others may add meanwhile
    lock.Lock();
    count_ += n;
    return before;
  }

 private:
  Mutex mutex_;
  int count_ FORGEDB_GUARDED_BY(mutex_) = 0;
};

TEST(MutexTest, MutexLockExcludesOtherThreads) {
  Counter counter;
  constexpr int kThreads = 8;
  constexpr int kAdds = 20000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&counter] {
      for (int i = 0; i < kAdds; ++i) {
        counter.Add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(counter.Get(), kThreads * kAdds);
}

TEST(MutexTest, UnlockAndLockAgainWithinAScope) {
  Counter counter;
  EXPECT_EQ(counter.AddAroundAGap(5), 0);
  EXPECT_EQ(counter.AddAroundAGap(2), 5);
  EXPECT_EQ(counter.Get(), 7);
  // The destructor of a MutexLock released early must not release again:
  // the mutex is free and can be taken (a double unlock would be undefined).
  counter.Add(1);
  EXPECT_EQ(counter.Get(), 8);
}

// A one-slot queue: Put waits while it is full, Take while it is empty.
class Slot {
 public:
  void Put(int value) {
    const MutexLock lock(&mutex_);
    while (full_) {
      changed_.Wait(&mutex_);
    }
    value_ = value;
    full_ = true;
    changed_.SignalAll();
  }
  [[nodiscard]] int Take() {
    const MutexLock lock(&mutex_);
    while (!full_) {
      changed_.Wait(&mutex_);
    }
    full_ = false;
    changed_.SignalAll();
    return value_;
  }

 private:
  Mutex mutex_;
  CondVar changed_;
  bool full_ FORGEDB_GUARDED_BY(mutex_) = false;
  int value_ FORGEDB_GUARDED_BY(mutex_) = 0;
};

TEST(CondVarTest, WaitReleasesTheMutexAndWakesOnSignal) {
  Slot slot;
  constexpr int kValues = 5000;
  std::thread producer([&slot] {
    for (int i = 1; i <= kValues; ++i) {
      slot.Put(i);
    }
  });
  long long sum = 0;
  for (int i = 1; i <= kValues; ++i) {
    const int value = slot.Take();
    ASSERT_EQ(value, i) << "in order, none lost or repeated";
    sum += value;
  }
  producer.join();
  EXPECT_EQ(sum, static_cast<long long>(kValues) * (kValues + 1) / 2);
}

TEST(CondVarTest, SignalAllWakesEveryWaiter) {
  Mutex mutex;
  CondVar go;
  bool open = false;  // guarded by mutex (a local: the analysis can't annotate it)
  int woken = 0;      // guarded by mutex
  constexpr int kWaiters = 6;
  std::vector<std::thread> waiters;
  waiters.reserve(kWaiters);
  for (int i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&] {
      const MutexLock lock(&mutex);
      while (!open) {
        go.Wait(&mutex);
      }
      ++woken;
    });
  }
  {
    const MutexLock lock(&mutex);
    open = true;
  }
  go.SignalAll();
  for (std::thread& waiter : waiters) {
    waiter.join();
  }
  const MutexLock lock(&mutex);
  EXPECT_EQ(woken, kWaiters);
}

}  // namespace
}  // namespace forgedb
