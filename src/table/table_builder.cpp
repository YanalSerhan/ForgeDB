#include "table/table_builder.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "forgedb/check.h"
#include "forgedb/status.h"

#include "db/internal_key.h"
#include "table/block_format.h"
#include "table/table_format.h"

namespace forgedb {

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(&CompareInternalKeys, options.block_restart_interval),
      index_block_(&CompareInternalKeys, 1) {
  FORGEDB_CHECK(options.block_size >= 1 && options.filter_bits_per_key >= 0);
  if (options.filter_bits_per_key > 0) {
    filter_.emplace(options.filter_bits_per_key);
  }
}

void TableBuilder::Add(std::string_view internal_key, std::string_view value) {
  FORGEDB_DCHECK(!finished_);
  FORGEDB_DCHECK(ParseInternalKey(internal_key).has_value());
  FORGEDB_DCHECK(entries_ == 0 || CompareInternalKeys(internal_key, last_key_) > 0);
  if (!status_.ok()) {
    return;
  }
  data_block_.Add(internal_key, value);
  if (filter_) {
    filter_->AddKey(ExtractUserKey(internal_key));
  }
  last_key_.assign(internal_key);
  ++entries_;
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) {
    return;
  }
  const BlockHandle handle = WriteBlock(data_block_.Finish());
  data_block_.Reset();
  if (!status_.ok()) {
    return;
  }
  // The index maps each block's last key to the block: the first index entry
  // >= a target names the only block that can hold the target.
  std::string encoded;
  AppendBlockHandle(encoded, handle);
  index_block_.Add(last_key_, encoded);
}

BlockHandle TableBuilder::WriteBlock(std::string_view contents) {
  const BlockHandle handle{.offset = offset_, .size = contents.size()};
  const auto trailer = MakeBlockTrailer(contents);
  status_ = file_->Append(contents);
  if (status_.ok()) {
    status_ = file_->Append(std::string_view(trailer.data(), trailer.size()));
  }
  if (status_.ok()) {
    offset_ += contents.size() + trailer.size();
  }
  return handle;
}

Status TableBuilder::Finish() {
  FORGEDB_DCHECK(!finished_);
  finished_ = true;
  FlushDataBlock();

  TableFooter footer;
  if (status_.ok() && filter_) {
    const std::string filter = filter_->Finish();
    footer.filter = WriteBlock(filter);
  }
  if (status_.ok()) {
    footer.index = WriteBlock(index_block_.Finish());
  }
  if (status_.ok()) {
    const std::string encoded = EncodeTableFooter(footer);
    status_ = file_->Append(encoded);
    if (status_.ok()) {
      offset_ += encoded.size();
    }
  }
  return status_;
}

}  // namespace forgedb
