// JsonWriter: writes JSON for forgedb_bench's results (TODO 5.1), so the
// tool needs no JSON library.
//
//   JsonWriter json;
//   json.BeginObject();
//   json.Key("ops");
//   json.Number(std::uint64_t{1000});
//   json.EndObject();
//   std::string text = json.Take();   // {"ops": 1000}, indented two spaces
//
// Commas and indentation are written for you. The caller keeps keys and
// values in the right order (no checks beyond FORGEDB_DCHECK). Doubles that
// are not finite are written as null.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forgedb::bench {

class JsonWriter {
 public:
  void BeginObject() { Open('{'); }
  void EndObject() { Close('}'); }
  void BeginArray() { Open('['); }
  void EndArray() { Close(']'); }

  // In an object: the next value's name.
  void Key(std::string_view name);

  void String(std::string_view value);
  void Number(double value);
  void Number(std::uint64_t value);
  void Number(std::int64_t value);
  void Bool(bool value);
  void Null();

  // The text so far (complete once every object and array is closed).
  [[nodiscard]] const std::string& Text() const noexcept { return out_; }
  [[nodiscard]] std::string Take() { return std::move(out_); }

  // `value` as a JSON string literal, quotes included.
  [[nodiscard]] static std::string Quote(std::string_view value);

 private:
  struct Level {
    bool empty = true;
  };

  void Open(char bracket);
  void Close(char bracket);
  // Before a value: a comma and a new line, unless it follows a key.
  void BeforeValue();
  void NewLine();

  std::string out_;
  std::vector<Level> levels_;
  bool after_key_ = false;
};

}  // namespace forgedb::bench
