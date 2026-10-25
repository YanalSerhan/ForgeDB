// FaultInjectionFileSystem: a FileSystem wrapper that simulates crashes and
// I/O errors, for crash tests (TODO 2.4; used by the torture test in 2.5).
//
// It wraps any FileSystem (normally MemFileSystem) and forwards every call to
// it, while tracking what a crash would leave on disk. SimulateCrash() then
// rewrites the wrapped file system into one possible post-crash state.
//
// What survives a crash (only documented behavior is trusted; docs/adr/0002):
//
//   kPowerLoss    An operating-system crash or power loss.
//     - A file's synced bytes always survive, exactly (F2, F3).
//     - A name survives once its file has been synced under that name (rule 3:
//       F3 read as covering the file's own directory entry). A name whose
//       latest change was never followed by a sync comes back as either the
//       old version (the last file synced under that name, or nothing) or the
//       new one. So a file that was never synced may vanish.
//     - Bytes that were flushed but not synced: per Options::unsynced_tail,
//       dropped, a random prefix kept, or a random prefix kept with its end
//       torn (overwritten with garbage or zeros). Synced bytes are never
//       altered. Bytes never flushed are still in the process's memory and
//       never reach the disk.
//     - Renames and removals are not durable unless the options say so
//       (Windows documents no way to make them durable: ADR 0002, F5 and F7).
//       Each name is decided on its own, so after a rename that was not made
//       durable, both names (or neither) may show the file.
//     - Directories are durable once created (an assumption: Windows does not
//       document it either; ADR 0002, rule 11).
//   kProcessCrash  The process dies; the operating system keeps running.
//     - Everything flushed survives, with every rename and removal; only bytes
//       appended but never flushed are lost.
//
// Files that existed before the wrapper first touched them (including every
// file after a SimulateCrash) are treated as fully durable.
//
// After a crash, every file and lock object handed out before it is dead:
// every call returns IOError and Close writes nothing, so the crashed DB can
// be destroyed afterwards without changing the post-crash state. Locks held
// before the crash are released, as when a process dies.
//
// Faults:
//   FailNth / FailWithProbability   make calls of one operation fail with
//                                   IOError (WritableFile errors stay sticky)
//   CrashAtOperation(n)             the process "dies" at the n-th operation
//                                   that changes the file system: it and every
//                                   later call fail and change nothing, until
//                                   SimulateCrash
//
// Randomness comes from Options::seed, so a single-threaded test that makes
// the same calls gets the same faults and the same crash outcomes.
//
// Not modeled: reads that fail or return damaged data (corruption tests flip
// bits in files directly), and writing through a handle whose file has since
// been recreated with NewWritableFile (Windows refuses that open anyway).
//
// Thread safety: like any FileSystem. The FaultInjectionFileSystem must outlive
// every file and lock it returns.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "env/file_system.h"

namespace forgedb {

// The file and lock types it hands out (defined in the .cpp file).
class FaultWritableFile;
class FaultSequentialFile;
class FaultRandomAccessFile;
class FaultFileLock;

class FaultInjectionFileSystem final : public FileSystem {
 public:
  enum class CrashKind : std::uint8_t { kPowerLoss, kProcessCrash };

  // What a power loss leaves of a file's flushed but unsynced bytes.
  enum class UnsyncedTail : std::uint8_t {
    kDrop,            // nothing
    kKeepPrefix,      // a random prefix (possibly empty or all of it)
    kKeepPrefixTorn,  // a random prefix whose last 1..512 bytes are garbage or zeros
  };

  struct Options {
    std::uint64_t seed = 1;
    UnsyncedTail unsynced_tail = UnsyncedTail::kKeepPrefixTorn;
    // Also zero random 4 KiB pages inside the kept unsynced bytes, as if the
    // operating system wrote later pages before earlier ones. Off by default:
    // ADR 0004 assumes in-order persistence; 2.5 decides whether to turn it on.
    bool zero_pages_in_unsynced_tail = false;
    // ADR 0002, option A: a rename is durable once RenameReplace returns.
    bool durable_renames = false;
    // A removal is durable once RemoveFile returns.
    bool durable_removals = false;
    // A failing Append (FailNth/FailWithProbability) first writes a random
    // strict prefix of its data, as a real partial write could.
    bool partial_append_on_error = true;
  };

  // The operations that change the file system: the ones that can fail on
  // purpose and that CrashAtOperation counts.
  enum class Op : std::uint8_t {
    kNewWritableFile,
    kAppend,
    kFlush,
    kSync,
    kClose,
    kRenameReplace,
    kRemoveFile,
    kCreateDir,
    kLockFile,
  };
  static constexpr std::size_t kOpCount = 9;

  // One file's fate in the last SimulateCrash.
  struct CrashedFile {
    enum class Outcome : std::uint8_t { kUnchanged, kRewritten, kRemoved };
    std::string path;  // UTF-8, '/' separators
    Outcome outcome = Outcome::kUnchanged;
    std::uint64_t size = 0;  // after the crash (0 if removed)
  };

  explicit FaultInjectionFileSystem(std::shared_ptr<FileSystem> base);
  FaultInjectionFileSystem(std::shared_ptr<FileSystem> base, const Options& options);
  ~FaultInjectionFileSystem() override;
  FaultInjectionFileSystem(const FaultInjectionFileSystem&) = delete;
  FaultInjectionFileSystem& operator=(const FaultInjectionFileSystem&) = delete;
  FaultInjectionFileSystem(FaultInjectionFileSystem&&) = delete;
  FaultInjectionFileSystem& operator=(FaultInjectionFileSystem&&) = delete;

  // --- Faults ---

  // The n-th call of `op` from now (n = 1: the next one) fails with IOError.
  void FailNth(Op op, std::uint64_t n);
  // From now on, each call of `op` fails with probability p (0 turns it off).
  void FailWithProbability(Op op, double p);
  // Removes every FailNth and FailWithProbability.
  void ClearFaults();
  // The n-th operation from now that would change the file system (n = 1:
  // the next one) and every call after it fail, as if the process died there.
  void CrashAtOperation(std::uint64_t n);
  // Operations that changed (or tried to change) the file system so far.
  [[nodiscard]] std::uint64_t OperationCount() const;
  // True once CrashAtOperation has fired, until SimulateCrash.
  [[nodiscard]] bool ProcessIsDead() const;

  // --- Crashing ---

  // Rewrites the wrapped file system into one possible state after a crash
  // of the given kind (see the top of this file). Afterwards every file is
  // durable, the process is alive again and earlier handles are dead.
  // Faults set with FailNth/FailWithProbability stay in force.
  [[nodiscard]] Status SimulateCrash(CrashKind kind);
  // What the last SimulateCrash did to each file it had to decide on.
  [[nodiscard]] std::vector<CrashedFile> LastCrash() const;

  // --- FileSystem ---

  [[nodiscard]] Result<std::unique_ptr<WritableFile>> NewWritableFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<SequentialFile>> NewSequentialFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Status RenameReplace(const std::filesystem::path& from,
                                     const std::filesystem::path& to) override;
  [[nodiscard]] Status RemoveFile(const std::filesystem::path& path) override;
  [[nodiscard]] Result<bool> FileExists(const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::vector<std::string>> GetChildren(
      const std::filesystem::path& dir) override;
  [[nodiscard]] Result<std::uint64_t> GetFileSize(const std::filesystem::path& path) override;
  [[nodiscard]] Status CreateDir(const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<FileLock>> LockFile(
      const std::filesystem::path& path) override;
  [[nodiscard]] Status UnlockFile(std::unique_ptr<FileLock> lock) override;

 private:
  friend class FaultWritableFile;
  friend class FaultSequentialFile;
  friend class FaultRandomAccessFile;
  friend class FaultFileLock;

  using Key = std::filesystem::path::string_type;

  // One file (an inode): created by NewWritableFile or LockFile, or imported
  // from the wrapped file system. Names point to files; a rename moves the
  // name, not the data.
  struct Node {
    std::string data;           // every byte written to the file (its live contents)
    std::uint64_t flushed = 0;  // bytes handed to the OS: survive a process crash
    std::uint64_t synced = 0;   // bytes on stable storage: survive a power loss
    bool has_live_name = false;
    Key live_name;  // where the file is now, if has_live_name
  };
  using NodePtr = std::shared_ptr<Node>;

  // Starts an operation that changes the file system: fails if the process is
  // dead, counts it, fires CrashAtOperation, and applies injected faults.
  // `injected` tells the caller the failure was a FailNth/probability fault.
  [[nodiscard]] Status BeginOperation(Op op, bool* injected = nullptr);
  // Status for any call while the process is dead.
  [[nodiscard]] static Status DeadStatus();
  // Status for a call on a handle from before the last crash.
  [[nodiscard]] static Status StaleStatus();
  // Makes sure `key` is tracked; a file the wrapped file system already has
  // is imported as fully durable (its contents read only if `read_contents`).
  [[nodiscard]] Status Track(const Key& key, bool read_contents = true);
  void SetLiveName(const Key& key, const NodePtr& node);
  void ClearLiveName(const Key& key);
  // The bytes of `node` that a power loss leaves (synced bytes + a tail).
  std::string PowerLossContents(const Node& node);
  // Applies the torn end and zero pages to contents[tail_start, end).
  void TearAndZero(std::string& contents, std::size_t tail_start);
  std::uint64_t Random(std::uint64_t bound);  // in [0, bound); 0 if bound is 0
  bool Chance(double p);
  // Releases a lock handed out by LockFile; OK if a crash already did.
  [[nodiscard]] Status ReleaseLockLocked(std::uint64_t id);

  const std::shared_ptr<FileSystem> base_;
  const Options options_;

  mutable std::mutex mutex_;
  std::mt19937_64 rng_;
  std::map<Key, NodePtr> live_;     // tracked names that exist now
  std::map<Key, NodePtr> durable_;  // tracked names -> file after a power loss (null: no file)
  std::set<FaultWritableFile*> open_files_;
  std::map<std::uint64_t, std::unique_ptr<FileLock>> held_locks_;  // id -> lock in base_
  std::uint64_t next_lock_id_ = 1;
  std::array<std::uint64_t, kOpCount> fail_countdown_{};  // 0 = off
  std::array<double, kOpCount> fail_probability_{};
  std::uint64_t operations_ = 0;
  std::uint64_t crash_at_ = 0;  // 0 = off; else the operation number that dies
  std::vector<CrashedFile> last_crash_;
  // Read without the mutex by readers, hence atomic.
  std::atomic<std::uint64_t> epoch_{0};  // advanced by every SimulateCrash
  std::atomic<bool> dead_{false};
};

[[nodiscard]] const char* OpName(FaultInjectionFileSystem::Op op) noexcept;

}  // namespace forgedb
