// Mutex, MutexLock and CondVar: std::mutex and std::condition_variable with
// Clang thread-safety annotations (util/thread_annotations.h; ADR 0009).
// MSVC's standard library does not annotate std::mutex, so ForgeDB's own
// code locks through these.
//
//   Mutex mutex_;
//   CondVar changed_;
//   int count_ FORGEDB_GUARDED_BY(mutex_);
//
//   void Wait() {
//     MutexLock lock(&mutex_);
//     while (count_ == 0) {
//       changed_.Wait(&mutex_);
//     }
//   }
#pragma once

#include <condition_variable>
#include <mutex>

#include "util/thread_annotations.h"

namespace forgedb {

class FORGEDB_CAPABILITY("mutex") Mutex {
 public:
  Mutex() = default;
  ~Mutex() = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
  Mutex(Mutex&&) = delete;
  Mutex& operator=(Mutex&&) = delete;

  void Lock() FORGEDB_ACQUIRE() { mutex_.lock(); }
  void Unlock() FORGEDB_RELEASE() { mutex_.unlock(); }

  // Tells the analysis the caller holds this mutex, for code it cannot
  // follow. Not checked at run time.
  void AssertHeld() const FORGEDB_ASSERT_CAPABILITY(this) {}

 private:
  friend class CondVar;
  std::mutex mutex_;
};

// Holds a Mutex for its scope. Unlock() and Lock() release it and take it
// again within the scope (around I/O); the destructor releases it only if
// it is held.
class FORGEDB_SCOPED_CAPABILITY MutexLock {
 public:
  explicit MutexLock(Mutex* mutex) FORGEDB_ACQUIRE(mutex) : mutex_(mutex) { mutex_->Lock(); }
  ~MutexLock() FORGEDB_RELEASE() {
    if (held_) {
      mutex_->Unlock();
    }
  }
  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;
  MutexLock(MutexLock&&) = delete;
  MutexLock& operator=(MutexLock&&) = delete;

  void Unlock() FORGEDB_RELEASE() {
    mutex_->Unlock();
    held_ = false;
  }
  void Lock() FORGEDB_ACQUIRE() {
    mutex_->Lock();
    held_ = true;
  }

 private:
  Mutex* mutex_;
  bool held_ = true;
};

class CondVar {
 public:
  CondVar() = default;
  ~CondVar() = default;
  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;
  CondVar(CondVar&&) = delete;
  CondVar& operator=(CondVar&&) = delete;

  // Releases `mutex`, waits for a Signal or SignalAll (or a spurious
  // wakeup: always wait in a loop that rechecks the condition), and holds
  // `mutex` again when it returns.
  void Wait(Mutex* mutex) FORGEDB_REQUIRES(mutex) {
    std::unique_lock<std::mutex> lock(mutex->mutex_, std::adopt_lock);
    cv_.wait(lock);
    lock.release();  // still held: the caller's
  }

  void Signal() noexcept { cv_.notify_one(); }
  void SignalAll() noexcept { cv_.notify_all(); }

 private:
  std::condition_variable cv_;
};

}  // namespace forgedb
