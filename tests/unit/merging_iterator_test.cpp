#include "table/merging_iterator.h"

#include <compare>
#include <cstddef>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/iterator.h"
#include "forgedb/status.h"

namespace forgedb {
namespace {

std::strong_ordering Bytewise(std::string_view a, std::string_view b) noexcept {
  return a.compare(b) <=> 0;
}

// Sorted key/value pairs; optionally fails (not Valid, status IOError) when
// it would move to entry `fail_at`.
class VectorIterator final : public Iterator {
 public:
  explicit VectorIterator(std::vector<std::pair<std::string, std::string>> entries,
                          std::optional<std::size_t> fail_at = std::nullopt)
      : entries_(std::move(entries)), fail_at_(fail_at) {}

  [[nodiscard]] bool Valid() const override { return status_.ok() && at_ < entries_.size(); }
  void SeekToFirst() override { MoveTo(0); }
  void Seek(std::string_view target) override {
    std::size_t i = 0;
    while (i < entries_.size() && entries_[i].first < target) {
      ++i;
    }
    MoveTo(i);
  }
  void Next() override { MoveTo(at_ + 1); }
  [[nodiscard]] std::string_view key() const override { return entries_[at_].first; }
  [[nodiscard]] std::string_view value() const override { return entries_[at_].second; }
  [[nodiscard]] Status status() const override { return status_; }

 private:
  void MoveTo(std::size_t i) {
    at_ = i;
    if (fail_at_ && i >= *fail_at_ && i < entries_.size()) {
      status_ = Status::IOError("injected read error");
    }
  }

  std::vector<std::pair<std::string, std::string>> entries_;
  std::optional<std::size_t> fail_at_;
  std::size_t at_ = 0;
  Status status_;
};

using Entries = std::vector<std::pair<std::string, std::string>>;

std::unique_ptr<Iterator> Merge(const std::vector<Entries>& children,
                                std::optional<std::size_t> fail_child = std::nullopt,
                                std::size_t fail_at = 0) {
  std::vector<std::unique_ptr<Iterator>> iterators;
  iterators.reserve(children.size());
  for (std::size_t i = 0; i < children.size(); ++i) {
    iterators.push_back(std::make_unique<VectorIterator>(
        children[i], fail_child == i ? std::optional<std::size_t>(fail_at) : std::nullopt));
  }
  return NewMergingIterator(std::move(iterators), &Bytewise);
}

std::string Keys(Iterator& it) {
  std::string keys;
  for (; it.Valid(); it.Next()) {
    keys.append(it.key()).append(":").append(it.value()).append(" ");
  }
  return keys;
}

TEST(MergingIteratorTest, MergesInOrder) {
  const std::unique_ptr<Iterator> it =
      Merge({{{"b", "1"}, {"e", "1"}}, {{"a", "2"}, {"d", "2"}, {"f", "2"}}, {}, {{"c", "3"}}});
  it->SeekToFirst();
  EXPECT_EQ(Keys(*it), "a:2 b:1 c:3 d:2 e:1 f:2 ");
  EXPECT_TRUE(it->status().ok());
}

TEST(MergingIteratorTest, SeekPositionsEveryChild) {
  const std::unique_ptr<Iterator> it =
      Merge({{{"b", "1"}, {"e", "1"}}, {{"a", "2"}, {"d", "2"}, {"f", "2"}}, {{"c", "3"}}});
  it->Seek("c");
  EXPECT_EQ(Keys(*it), "c:3 d:2 e:1 f:2 ");
  it->Seek("cc");
  EXPECT_EQ(Keys(*it), "d:2 e:1 f:2 ");
  it->Seek("g");
  EXPECT_FALSE(it->Valid());
  it->Seek("");
  EXPECT_EQ(Keys(*it), "a:2 b:1 c:3 d:2 e:1 f:2 ");
}

TEST(MergingIteratorTest, EqualKeysComeFromTheEarlierChildFirst) {
  const std::unique_ptr<Iterator> it = Merge({{{"k", "first"}}, {{"k", "second"}}});
  it->SeekToFirst();
  EXPECT_EQ(Keys(*it), "k:first k:second ");
}

TEST(MergingIteratorTest, NoChildrenOrAllEmpty) {
  const std::unique_ptr<Iterator> none = Merge({});
  none->SeekToFirst();
  EXPECT_FALSE(none->Valid());
  const std::unique_ptr<Iterator> empty = Merge({{}, {}});
  empty->Seek("a");
  EXPECT_FALSE(empty->Valid());
  EXPECT_TRUE(empty->status().ok());
}

TEST(MergingIteratorTest, AChildErrorStopsTheMerge) {
  // Child 1 fails when it moves past its first entry ("a"). The merge stops
  // right there: the failed child might have held keys before "b", so
  // returning "b" could skip data.
  const std::unique_ptr<Iterator> it =
      Merge({{{"b", "1"}, {"e", "1"}}, {{"a", "2"}, {"d", "2"}, {"f", "2"}}}, 1, 1);
  it->SeekToFirst();
  EXPECT_EQ(Keys(*it), "a:2 ");
  EXPECT_EQ(it->status().code(), Status::Code::kIOError);
  it->SeekToFirst();  // sticky
  EXPECT_FALSE(it->Valid());
}

TEST(MergingIteratorTest, AnErrorWhilePositioning) {
  const std::unique_ptr<Iterator> it = Merge({{{"a", "1"}}, {{"b", "2"}}}, 1, 0);
  it->Seek("a");
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(it->status().code(), Status::Code::kIOError);
}

TEST(MergingIteratorTest, ManyChildren) {
  std::vector<Entries> children(50);
  for (int i = 0; i < 1000; ++i) {
    const std::string key = std::to_string(100000 + i);
    children[static_cast<std::size_t>(i * 7 % 50)].emplace_back(key, "v");
  }
  const std::unique_ptr<Iterator> it = Merge(children);
  int count = 0;
  std::string previous;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    EXPECT_LT(previous, it->key());
    previous = std::string(it->key());
    ++count;
  }
  EXPECT_EQ(count, 1000);
}

TEST(ErrorIteratorTest, IsEmptyWithItsStatus) {
  const std::unique_ptr<Iterator> it = NewErrorIterator(Status::Corruption("bad table"));
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek("x");
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(it->status().code(), Status::Code::kCorruption);
}

}  // namespace
}  // namespace forgedb
