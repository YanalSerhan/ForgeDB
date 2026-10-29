#include "support/gated_file_system.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

namespace forgedb {
namespace {

class GatedFile final : public WritableFile {
 public:
  GatedFile(std::unique_ptr<WritableFile> base, Gate* gate) : base_(std::move(base)), gate_(gate) {}
  Status Append(std::string_view data) override { return base_->Append(data); }
  Status Flush() override { return base_->Flush(); }
  Status Sync() override {
    FORGEDB_RETURN_IF_ERROR(gate_->Pass());
    return base_->Sync();
  }
  Status Close() override { return base_->Close(); }

 private:
  std::unique_ptr<WritableFile> base_;
  Gate* gate_;
};

}  // namespace

void Gate::Close(int free_passes) {
  const std::scoped_lock lock(mutex_);
  open_ = false;
  free_passes_ = free_passes;
}

void Gate::Open(Status result) {
  {
    const std::scoped_lock lock(mutex_);
    open_ = true;
    result_ = std::move(result);
  }
  cv_.notify_all();
}

void Gate::WaitForArrival() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return waiting_ > 0; });
}

Status Gate::Pass() {
  std::unique_lock lock(mutex_);
  if (!open_ && free_passes_ > 0) {
    --free_passes_;
    return Status::OK();
  }
  ++waiting_;
  cv_.notify_all();
  cv_.wait(lock, [this] { return open_; });
  --waiting_;
  return result_;
}

GatedFileSystem::GatedFileSystem(std::shared_ptr<FileSystem> base, Gate* gate)
    : base_(std::move(base)), gate_(gate) {}

Result<std::unique_ptr<WritableFile>> GatedFileSystem::NewWritableFile(
    const std::filesystem::path& path) {
  FORGEDB_ASSIGN_OR_RETURN(std::unique_ptr<WritableFile> file, base_->NewWritableFile(path));
  if (path.extension() == ".sst") {
    return std::unique_ptr<WritableFile>(std::make_unique<GatedFile>(std::move(file), gate_));
  }
  return file;
}

Result<std::unique_ptr<SequentialFile>> GatedFileSystem::NewSequentialFile(
    const std::filesystem::path& path) {
  return base_->NewSequentialFile(path);
}

Result<std::unique_ptr<RandomAccessFile>> GatedFileSystem::NewRandomAccessFile(
    const std::filesystem::path& path) {
  return base_->NewRandomAccessFile(path);
}

Status GatedFileSystem::RenameReplace(const std::filesystem::path& from,
                                      const std::filesystem::path& to) {
  return base_->RenameReplace(from, to);
}

Status GatedFileSystem::RemoveFile(const std::filesystem::path& path) {
  return base_->RemoveFile(path);
}

Result<bool> GatedFileSystem::FileExists(const std::filesystem::path& path) {
  return base_->FileExists(path);
}

Result<std::vector<std::string>> GatedFileSystem::GetChildren(const std::filesystem::path& dir) {
  return base_->GetChildren(dir);
}

Result<std::uint64_t> GatedFileSystem::GetFileSize(const std::filesystem::path& path) {
  return base_->GetFileSize(path);
}

Status GatedFileSystem::SyncFile(const std::filesystem::path& path) {
  return base_->SyncFile(path);
}

Status GatedFileSystem::CreateDir(const std::filesystem::path& path) {
  return base_->CreateDir(path);
}

Result<std::unique_ptr<FileLock>> GatedFileSystem::LockFile(const std::filesystem::path& path) {
  return base_->LockFile(path);
}

Status GatedFileSystem::UnlockFile(std::unique_ptr<FileLock> lock) {
  return base_->UnlockFile(std::move(lock));
}

}  // namespace forgedb
