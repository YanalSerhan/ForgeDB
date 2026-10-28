#include "table/merging_iterator.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "forgedb/check.h"
#include "forgedb/iterator.h"
#include "forgedb/status.h"

namespace forgedb {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(std::vector<std::unique_ptr<Iterator>> children, KeyCompare compare)
      : children_(std::move(children)), compare_(compare) {
    heap_.reserve(children_.size());
  }

  [[nodiscard]] bool Valid() const override { return status_.ok() && !heap_.empty(); }

  void SeekToFirst() override {
    if (!status_.ok()) {
      return;
    }
    for (const std::unique_ptr<Iterator>& child : children_) {
      child->SeekToFirst();
    }
    RebuildHeap();
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) {
      return;
    }
    for (const std::unique_ptr<Iterator>& child : children_) {
      child->Seek(target);
    }
    RebuildHeap();
  }

  void Next() override {
    FORGEDB_DCHECK(Valid());
    // The current child is at the top: advance it and put it back.
    std::ranges::pop_heap(heap_, Greater{this});
    const std::size_t current = heap_.back();
    heap_.pop_back();
    Iterator& child = *children_[current];
    child.Next();
    if (child.Valid()) {
      heap_.push_back(current);
      std::ranges::push_heap(heap_, Greater{this});
    } else if (!child.status().ok()) {
      Fail(child.status());
    }
  }

  [[nodiscard]] std::string_view key() const override {
    FORGEDB_DCHECK(Valid());
    return children_[heap_.front()]->key();
  }

  [[nodiscard]] std::string_view value() const override {
    FORGEDB_DCHECK(Valid());
    return children_[heap_.front()]->value();
  }

  [[nodiscard]] Status status() const override { return status_; }

 private:
  // Heap order: the child with the smallest key (then the lowest index) on top.
  struct Greater {
    const MergingIterator* self;
    bool operator()(std::size_t a, std::size_t b) const {
      const std::strong_ordering order =
          self->compare_(self->children_[a]->key(), self->children_[b]->key());
      return order > 0 || (order == 0 && a > b);
    }
  };

  void RebuildHeap() {
    heap_.clear();
    for (std::size_t i = 0; i < children_.size(); ++i) {
      if (children_[i]->Valid()) {
        heap_.push_back(i);
      } else if (!children_[i]->status().ok()) {
        Fail(children_[i]->status());
        return;
      }
    }
    std::ranges::make_heap(heap_, Greater{this});
  }

  void Fail(Status status) {
    status_ = std::move(status);
    heap_.clear();
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  KeyCompare compare_;
  std::vector<std::size_t> heap_;  // indices of the valid children
  Status status_;
};

class ErrorIterator final : public Iterator {
 public:
  explicit ErrorIterator(Status status) : status_(std::move(status)) {}
  [[nodiscard]] bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view /*target*/) override {}
  void Next() override { FORGEDB_DCHECK(false); }
  [[nodiscard]] std::string_view key() const override {
    FORGEDB_DCHECK(false);
    return {};
  }
  [[nodiscard]] std::string_view value() const override {
    FORGEDB_DCHECK(false);
    return {};
  }
  [[nodiscard]] Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewErrorIterator(Status status) {
  return std::make_unique<ErrorIterator>(std::move(status));
}

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children,
                                             KeyCompare compare) {
  return std::make_unique<MergingIterator>(std::move(children), compare);
}

}  // namespace forgedb
