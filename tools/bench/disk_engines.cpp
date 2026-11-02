// NewDiskEngineFactory and FolderBytes (tools/bench/engine.h).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "forgedb/result.h"
#include "forgedb/status.h"

#include "tools/bench/engine.h"

namespace forgedb::bench {
namespace {

Status FileError(std::string_view what, const std::filesystem::path& path,
                 const std::error_code& error) {
  return Status::IOError(std::string(what) + " " + path.generic_string(), error.message());
}

class DiskEngineFactory final : public EngineFactory {
 public:
  explicit DiskEngineFactory(std::vector<DiskEngine> engines) : engines_(std::move(engines)) {}

  [[nodiscard]] std::vector<std::string> Engines() const override {
    std::vector<std::string> names;
    names.reserve(engines_.size());
    for (const DiskEngine& engine : engines_) {
      names.push_back(engine.name);
    }
    return names;
  }

  Result<std::unique_ptr<Engine>> Open(std::string_view engine, const std::filesystem::path& dir,
                                       const EngineOptions& options) override {
    for (const DiskEngine& known : engines_) {
      if (known.name == engine) {
        return known.open(dir, options);
      }
    }
    return Status::InvalidArgument("unknown engine", engine);
  }

  Status Remove(const std::filesystem::path& dir) override {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    return error ? FileError("removing", dir, error) : Status::OK();
  }

  Status CreateRoot(const std::filesystem::path& dir) override {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return error ? FileError("creating", dir, error) : Status::OK();
  }

  [[nodiscard]] bool RealFiles() const override { return true; }

 private:
  std::vector<DiskEngine> engines_;
};

}  // namespace

std::unique_ptr<EngineFactory> NewDiskEngineFactory(std::vector<DiskEngine> engines) {
  return std::make_unique<DiskEngineFactory>(std::move(engines));
}

Result<std::uint64_t> FolderBytes(const std::filesystem::path& dir) {
  std::error_code error;
  std::filesystem::directory_iterator it(dir, error);
  if (error) {
    return FileError("listing", dir, error);
  }
  std::uint64_t bytes = 0;
  for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
    // A file the engine removes while we list it is no longer part of the total.
    const bool regular = it->is_regular_file(error);
    if (!error && regular) {
      const std::uint64_t size = it->file_size(error);
      if (!error) {
        bytes += size;
      }
    }
    error.clear();
  }
  if (error) {
    return FileError("listing", dir, error);
  }
  return bytes;
}

}  // namespace forgedb::bench
