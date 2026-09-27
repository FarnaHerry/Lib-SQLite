#pragma once

/// @file
/// Public API for asynchronous SQLite access, schema declarations, migrations,
/// typed records, raw SQL, and explicit transactions in HuxerUI applications.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/task.h>

namespace huxerui::sqlite {

/// Identifies the broad category of a database failure.
///
/// Use this portable category for application control flow. When SQLite
/// supplied a native code, Error::SqlitePrimaryCode() and
/// Error::SqliteExtendedCode() retain the more specific diagnostic value.
enum class ErrorCode {
  Unknown,        ///< A failure that does not map to a more specific category.
  Sql,            ///< Invalid SQL, parameters, or statement usage.
  Constraint,     ///< A SQLite constraint such as UNIQUE or FOREIGN KEY failed.
  Storage,        ///< The database file or underlying storage failed.
  Busy,           ///< The database could not proceed before the busy timeout.
  Locked,         ///< A table or shared-cache resource was locked.
  Cancelled,      ///< The owning task cancelled the pending or running operation.
  Migration,      ///< A migration declaration or callback failed.
  SchemaMismatch, ///< The live schema does not match the declared schema.
  Unsupported,    ///< The requested mode or platform capability is unsupported.
  Decode,         ///< A bound or returned value could not be encoded or decoded.
  NotFound,       ///< An operation requiring one row found no matching row.
  Closed,         ///< The operation targeted a closed database.
  Transaction,    ///< Transaction creation, execution, commit, or rollback failed.
};

/// Describes a failed SQLite operation without exposing a SQLite handle.
///
/// Error objects are values and remain valid after the originating operation and worker callback have completed.
class Error final {
public:
  /// Creates an error value.
  ///
  /// @param code Portable error category.
  /// @param message Human-readable diagnostic text.
  /// @param operation Short description of the operation that failed.
  /// @param sqlite_primary_code Optional primary SQLite result code.
  /// @param sqlite_extended_code Optional extended SQLite result code.
  Error(
      ErrorCode code,
      std::string message,
      std::string operation = {},
      std::optional<int> sqlite_primary_code = std::nullopt,
      std::optional<int> sqlite_extended_code = std::nullopt
  );

  /// @return The portable error category.
  [[nodiscard]] ErrorCode Code() const noexcept;

  /// @return Human-readable diagnostic text owned by this error.
  [[nodiscard]] const std::string& Message() const noexcept;

  /// @return The operation description, or an empty string when unavailable.
  [[nodiscard]] const std::string& Operation() const noexcept;

  /// @return The primary SQLite result code when SQLite supplied one.
  [[nodiscard]] std::optional<int> SqlitePrimaryCode() const noexcept;

  /// @return The extended SQLite result code when SQLite supplied one.
  [[nodiscard]] std::optional<int> SqliteExtendedCode() const noexcept;

private:
  ErrorCode code_;
  std::string message_;
  std::string operation_;
  std::optional<int> sqlite_primary_code_;
  std::optional<int> sqlite_extended_code_;
};

/// Holds either a successful value or an Error.
///
/// @tparam T Successful value type.
///
/// @code
/// auto result = co_await database.ExecuteAsync("DELETE FROM users");
/// if (!result) {
///   ShowError(result.Error().Message());
///   co_return;
/// }
/// auto affected = result->rows_affected;
/// @endcode
template <class T> class [[nodiscard]] Result final {
  static_assert(!std::same_as<std::remove_cv_t<T>, sqlite::Error>);
  static_assert(std::is_object_v<T>);
  static_assert(std::same_as<T, std::remove_cv_t<T>>);

public:
  /// Creates a successful result.
  /// @param value Value stored in the result.
  Result(T value) : value_(std::move(value)) {}

  /// Creates a failed result.
  /// @param error Error stored in the result.
  Result(sqlite::Error error) : value_(std::move(error)) {}

  /// @return `true` when this result contains a successful value.
  [[nodiscard]] bool Succeeded() const noexcept {
    return std::holds_alternative<T>(value_);
  }

  /// Tests whether this result succeeded.
  /// @return The same state reported by Succeeded().
  [[nodiscard]] explicit operator bool() const noexcept {
    return Succeeded();
  }

  /// Accesses the successful value.
  /// @return A mutable reference to the stored value.
  /// @throws std::logic_error If this result contains an error.
  [[nodiscard]] T& Value() & {
    if (auto* value = std::get_if<T>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI SQLite result does not contain a value");
  }

  /// Accesses the successful value.
  /// @return A const reference to the stored value.
  /// @throws std::logic_error If this result contains an error.
  [[nodiscard]] const T& Value() const& {
    if (const auto* value = std::get_if<T>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI SQLite result does not contain a value");
  }

  /// Moves the successful value out of an rvalue result.
  /// @return An rvalue reference to the stored value.
  /// @throws std::logic_error If this result contains an error.
  [[nodiscard]] T&& Value() && {
    return std::move(static_cast<Result&>(*this).Value());
  }

  /// Accesses the stored error.
  /// @return A mutable reference to the error.
  /// @throws std::logic_error If this result contains a value.
  [[nodiscard]] sqlite::Error& Error() & {
    if (auto* error = std::get_if<sqlite::Error>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI SQLite result does not contain an error");
  }

  /// Accesses the stored error.
  /// @return A const reference to the error.
  /// @throws std::logic_error If this result contains a value.
  [[nodiscard]] const sqlite::Error& Error() const& {
    if (const auto* error = std::get_if<sqlite::Error>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI SQLite result does not contain an error");
  }

  /// Dereferences the successful value.
  /// @return A mutable reference to the stored value.
  /// @throws std::logic_error If this result contains an error.
  [[nodiscard]] T& operator*() & {
    return Value();
  }

  /// Dereferences the successful value.
  /// @return A const reference to the stored value.
  /// @throws std::logic_error If this result contains an error.
  [[nodiscard]] const T& operator*() const& {
    return Value();
  }

  /// Accesses a member of the successful value.
  /// @return A pointer to the stored value.
  /// @throws std::logic_error If this result contains an error.
  [[nodiscard]] T* operator->() {
    return &Value();
  }

  /// Accesses a member of the successful value.
  /// @return A const pointer to the stored value.
  /// @throws std::logic_error If this result contains an error.
  [[nodiscard]] const T* operator->() const {
    return &Value();
  }

private:
  std::variant<T, sqlite::Error> value_;
};

/// Represents success without a value, or an Error.
template <> class [[nodiscard]] Result<void> final {
public:
  Result() = default;

  /// Creates a failed result.
  /// @param error Error stored in the result.
  Result(sqlite::Error error) : error_(std::move(error)) {}

  /// @return `true` when this result represents success.
  [[nodiscard]] bool Succeeded() const noexcept {
    return !error_.has_value();
  }

  /// Tests whether this result succeeded.
  /// @return The same state reported by Succeeded().
  [[nodiscard]] explicit operator bool() const noexcept {
    return Succeeded();
  }

  /// Verifies that this result succeeded.
  /// @throws std::logic_error If this result contains an error.
  void Value() const {
    if (error_) {
      throw std::logic_error("HuxerUI SQLite result does not contain a value");
    }
  }

  /// Accesses the stored error.
  /// @return A mutable reference to the error.
  /// @throws std::logic_error If this result represents success.
  [[nodiscard]] sqlite::Error& Error() & {
    if (error_) {
      return *error_;
    }
    throw std::logic_error("HuxerUI SQLite result does not contain an error");
  }

  /// Accesses the stored error.
  /// @return A const reference to the error.
  /// @throws std::logic_error If this result represents success.
  [[nodiscard]] const sqlite::Error& Error() const& {
    if (error_) {
      return *error_;
    }
    throw std::logic_error("HuxerUI SQLite result does not contain an error");
  }

private:
  std::optional<sqlite::Error> error_;
};

/// Represents the SQL `NULL` value in Value.
struct Null final {
  bool operator==(const Null&) const = default;
};

/// Type-erased value accepted by parameter binding and returned by RowView.
using Value = std::variant<Null, std::int64_t, double, bool, std::string, Bytes>;

/// Declares the SQLite storage family used by a ValueCodec.
enum class StorageAffinity {
  Integer, ///< Signed integers, Boolean values, and opted-in enumerations.
  Real,    ///< Floating-point values.
  Text,    ///< Valid UTF-8 text.
  Blob,    ///< Arbitrary byte arrays.
};

/// Customization point for converting application values to and from Value.
///
/// A specialization must expose a static `affinity` member and static
/// `Encode` and `Decode` functions. Encoding and decoding failures are returned
/// as Error values rather than thrown as routine runtime failures.
///
/// @tparam T Application value type.
///
/// @code
/// struct UserCode { std::string value; };
///
/// template <> struct sqlite::ValueCodec<UserCode> {
///   static constexpr auto affinity = sqlite::StorageAffinity::Text;
///
///   static sqlite::Result<sqlite::Value> Encode(const UserCode& code) {
///     return sqlite::ValueCodec<std::string>::Encode(code.value);
///   }
///
///   static sqlite::Result<UserCode> Decode(const sqlite::Value& value) {
///     auto text = sqlite::ValueCodec<std::string>::Decode(value);
///     if (!text) return text.Error();
///     return UserCode{std::move(*text)};
///   }
/// };
/// @endcode
template <class T> struct ValueCodec;

/// ValueCodec specialization for signed integer types other than `bool`.
/// @tparam T Signed application integer type.
template <class T>
  requires(std::signed_integral<T> && !std::same_as<T, bool>)
struct ValueCodec<T> {
  /// SQLite storage affinity used for the encoded value.
  static constexpr StorageAffinity affinity = StorageAffinity::Integer;

  /// Encodes a signed integer.
  /// @param value Integer to encode.
  /// @return The integer widened to the library's 64-bit Value representation.
  [[nodiscard]] static Result<Value> Encode(T value) {
    return Value{static_cast<std::int64_t>(value)};
  }

  /// Decodes a signed integer with range checking.
  /// @param value SQLite value to decode.
  /// @return The decoded integer, or ErrorCode::Decode for a type or range mismatch.
  [[nodiscard]] static Result<T> Decode(const Value& value) {
    const auto* integer = std::get_if<std::int64_t>(&value);
    if (!integer || *integer < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
        *integer > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
      return sqlite::Error{ErrorCode::Decode, "SQLite value is not a compatible signed integer"};
    }
    return static_cast<T>(*integer);
  }
};

/// ValueCodec specialization for floating-point types.
/// @tparam T Application floating-point type.
template <std::floating_point T> struct ValueCodec<T> {
  /// SQLite storage affinity used for the encoded value.
  static constexpr StorageAffinity affinity = StorageAffinity::Real;

  /// Encodes a floating-point value.
  /// @param value Number to encode.
  /// @return The number converted to the library's double Value representation.
  [[nodiscard]] static Result<Value> Encode(T value) {
    return Value{static_cast<double>(value)};
  }

  /// Decodes either a SQLite REAL or INTEGER as a floating-point value.
  /// @param value SQLite value to decode.
  /// @return The converted number, or ErrorCode::Decode for a nonnumeric value.
  [[nodiscard]] static Result<T> Decode(const Value& value) {
    if (const auto* real = std::get_if<double>(&value)) {
      return static_cast<T>(*real);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      return static_cast<T>(*integer);
    }
    return sqlite::Error{ErrorCode::Decode, "SQLite value is not numeric"};
  }
};

/// ValueCodec specialization that stores Boolean values using INTEGER affinity.
template <> struct ValueCodec<bool> {
  /// SQLite storage affinity used for Boolean values.
  static constexpr StorageAffinity affinity = StorageAffinity::Integer;

  /// Encodes a Boolean value.
  /// @param value Boolean to encode.
  /// @return The encoded Value.
  [[nodiscard]] static Result<Value> Encode(bool value);

  /// Decodes the canonical SQLite Boolean representation.
  /// @param value SQLite value to decode.
  /// @return The decoded Boolean, or ErrorCode::Decode on mismatch.
  [[nodiscard]] static Result<bool> Decode(const Value& value);
};

/// ValueCodec specialization for validated UTF-8 text.
template <> struct ValueCodec<std::string> {
  /// SQLite storage affinity used for strings.
  static constexpr StorageAffinity affinity = StorageAffinity::Text;

  /// Encodes UTF-8 text.
  /// @param value Text to validate and encode.
  /// @return The encoded text, or ErrorCode::Decode for invalid UTF-8.
  [[nodiscard]] static Result<Value> Encode(const std::string& value);

  /// Decodes validated UTF-8 text.
  /// @param value SQLite value to decode.
  /// @return The decoded string, or ErrorCode::Decode on mismatch or invalid UTF-8.
  [[nodiscard]] static Result<std::string> Decode(const Value& value);
};

/// ValueCodec specialization for arbitrary byte arrays.
template <> struct ValueCodec<Bytes> {
  /// SQLite storage affinity used for byte arrays.
  static constexpr StorageAffinity affinity = StorageAffinity::Blob;

  /// Encodes a byte array without text interpretation.
  /// @param value Bytes to encode.
  /// @return The encoded BLOB value.
  [[nodiscard]] static Result<Value> Encode(const Bytes& value);

  /// Decodes a SQLite BLOB, or the raw bytes of a SQLite text value.
  /// @param value SQLite value to decode.
  /// @return The decoded bytes, or ErrorCode::Decode on mismatch. Text is returned as stored, so
  ///         this is the way to read a text value that is not valid UTF-8.
  [[nodiscard]] static Result<Bytes> Decode(const Value& value);
};

/// Adds SQL `NULL` support to another ValueCodec.
/// @tparam T Non-optional application value type.
template <class T> struct ValueCodec<std::optional<T>> {
  /// Storage affinity inherited from ValueCodec<T>.
  static constexpr StorageAffinity affinity = ValueCodec<T>::affinity;

  /// Encodes an optional value, mapping an empty optional to SQL `NULL`.
  /// @param value Optional value to encode.
  /// @return The encoded inner value or Null.
  [[nodiscard]] static Result<Value> Encode(const std::optional<T>& value) {
    if (!value) {
      return Value{Null{}};
    }
    return ValueCodec<T>::Encode(*value);
  }

  /// Decodes SQL `NULL` as an empty optional.
  /// @param value SQLite value to decode.
  /// @return The decoded optional, or the inner codec's error.
  [[nodiscard]] static Result<std::optional<T>> Decode(const Value& value) {
    if (std::holds_alternative<Null>(value)) {
      return std::optional<T>{};
    }
    auto decoded = ValueCodec<T>::Decode(value);
    if (!decoded) {
      return decoded.Error();
    }
    return std::optional<T>{std::move(*decoded)};
  }
};

/// Opt-in codec base for enumerations with a signed underlying type.
///
/// Specialize ValueCodec for the enumeration by inheriting this type:
///
/// @code
/// enum class State : std::int32_t { Active, Disabled };
/// template <> struct sqlite::ValueCodec<State> : sqlite::EnumValueCodec<State> {};
/// @endcode
///
/// @tparam Enum Enumeration type to encode using its underlying integer.
template <class Enum> struct EnumValueCodec {
  static_assert(std::is_enum_v<Enum>);
  /// Signed integer type underlying `Enum`.
  using Underlying = std::underlying_type_t<Enum>;
  static_assert(std::signed_integral<Underlying>);
  /// SQLite storage affinity used for the enumeration.
  static constexpr StorageAffinity affinity = StorageAffinity::Integer;

  /// Encodes an enumeration through its signed underlying type.
  /// @param value Enumeration value to encode.
  /// @return The encoded integer Value.
  [[nodiscard]] static Result<Value> Encode(Enum value) {
    return ValueCodec<Underlying>::Encode(static_cast<Underlying>(value));
  }

  /// Decodes an enumeration through its signed underlying type.
  /// @param value SQLite value to decode.
  /// @return The decoded enumeration, or ErrorCode::Decode on mismatch.
  [[nodiscard]] static Result<Enum> Decode(const Value& value) {
    auto decoded = ValueCodec<Underlying>::Decode(value);
    if (!decoded) {
      return decoded.Error();
    }
    return static_cast<Enum>(*decoded);
  }
};

namespace detail {
struct RowViewAccess;
struct TransactionAccess;
} // namespace detail

/// Non-owning view of the current row passed to a raw-query decoder.
///
/// A RowView is valid only for the duration of the decoder call. Decode or copy
/// every required value before returning from the callback.
///
/// @code
/// auto users = co_await database.QueryAsync<UserName>(
///     "SELECT id, name FROM users WHERE active = ?",
///     [](const sqlite::RowView& row) -> sqlite::Result<UserName> {
///       auto id = row.Get<std::int64_t>("id");
///       if (!id) return id.Error();
///       auto name = row.Get<std::string>("name");
///       if (!name) return name.Error();
///       return UserName{*id, std::move(*name)};
///     },
///     true);
/// @endcode
class RowView final {
public:
  RowView(const RowView&) = delete;
  RowView(RowView&&) = delete;
  RowView& operator=(const RowView&) = delete;
  RowView& operator=(RowView&&) = delete;

  /// @return Number of columns in the current result row.
  [[nodiscard]] std::size_t ColumnCount() const noexcept;

  /// Returns a column name by zero-based position.
  /// @param index Zero-based column position.
  /// @return A view of the column name, valid only during the decoder call.
  /// @throws std::out_of_range If `index` is outside the row.
  [[nodiscard]] std::string_view ColumnName(std::size_t index) const;

  /// Returns the type-erased value at a zero-based position.
  /// @param index Zero-based column position.
  /// @return A reference valid only during the decoder call.
  /// @throws std::out_of_range If `index` is outside the row.
  [[nodiscard]] const sqlite::Value& Value(std::size_t index) const;

  /// Decodes a column by zero-based position.
  /// @tparam T Application type with a ValueCodec specialization.
  /// @param index Zero-based column position.
  /// @return The decoded value or a codec error.
  /// @throws std::out_of_range If `index` is outside the row.
  template <class T> [[nodiscard]] Result<T> Get(std::size_t index) const {
    return ValueCodec<T>::Decode(Value(index));
  }

  /// Decodes a column by its result-set name.
  /// @tparam T Application type with a ValueCodec specialization.
  /// @param name Exact column name or SQL alias to locate.
  /// @return The decoded value, or ErrorCode::Decode when the column is absent.
  template <class T> [[nodiscard]] Result<T> Get(std::string_view name) const {
    const auto index = FindColumn(name);
    if (!index) {
      return sqlite::Error{ErrorCode::Decode, "SQLite result does not contain the requested column"};
    }
    return Get<T>(*index);
  }

private:
  RowView(const std::vector<std::string>& names, const std::vector<sqlite::Value>& values)
      : names_(&names), values_(&values) {}

  [[nodiscard]] std::optional<std::size_t> FindColumn(std::string_view name) const noexcept;

  friend struct detail::RowViewAccess;

  const std::vector<std::string>* names_;
  const std::vector<sqlite::Value>* values_;
};

namespace detail {

template <class T>
concept EncodableValue = requires(const std::remove_cvref_t<T>& value) {
  { ValueCodec<std::remove_cvref_t<T>>::Encode(value) } -> std::same_as<Result<Value>>;
};

template <EncodableValue... Arguments>
[[nodiscard]] Result<std::vector<Value>> EncodeValues(Arguments&&... arguments) {
  std::vector<Value> values;
  values.reserve(sizeof...(Arguments));
  std::optional<sqlite::Error> error;
  auto encode = [&](auto&& argument) {
    if (error) {
      return;
    }
    using Argument = std::remove_cvref_t<decltype(argument)>;
    auto encoded = ValueCodec<Argument>::Encode(argument);
    if (!encoded) {
      error = encoded.Error();
      return;
    }
    values.push_back(std::move(*encoded));
  };
  (encode(std::forward<Arguments>(arguments)), ...);
  if (error) {
    return std::move(*error);
  }
  return values;
}

template <class Decoder, class T>
concept RowDecoder = requires(Decoder& decoder, const RowView& row) {
  { std::invoke(decoder, row) } -> std::same_as<Result<T>>;
};

template <class T> struct ResultTraits;

template <class T> struct ResultTraits<Result<T>> {
  using Value = T;
};

template <class T>
concept ResultType = requires {
  typename ResultTraits<std::remove_cvref_t<T>>::Value;
};

template <class MemberPointer> struct MemberPointerTraits;

template <class Record, class Member> struct MemberPointerTraits<Member Record::*> {
  using RecordType = Record;
  using MemberType = Member;
};

template <class T> struct OptionalTraits {
  using Value = T;
  static constexpr bool optional = false;
};

template <class T> struct OptionalTraits<std::optional<T>> {
  using Value = T;
  static constexpr bool optional = true;
};

template <auto Member> inline constexpr unsigned char MemberIdentity = 0;

template <auto Member> [[nodiscard]] const void* MemberKey() noexcept {
  return &MemberIdentity<Member>;
}

struct ForeignKeySchema {
  std::string table;
  std::string column;
};

struct ColumnSchema {
  std::string name;
  StorageAffinity affinity = StorageAffinity::Blob;
  bool nullable = false;
  bool integer_key_compatible = false;
  bool primary_key = false;
  bool auto_increment = false;
  bool unique = false;
  std::optional<Value> default_value;
  std::optional<ForeignKeySchema> foreign_key;
  const void* member_key = nullptr;
  std::function<Result<Value>(const void*)> encode_member;
  std::function<Result<void>(void*, const Value&)> decode_member;
};

struct IndexSchema {
  std::string name;
  bool unique = false;
  std::vector<const void*> member_keys;
  std::vector<std::string> columns;
};

struct TableSchema {
  std::string name;
  std::vector<ColumnSchema> columns;
  std::vector<IndexSchema> indexes;
};

[[nodiscard]] bool ValueMatchesAffinity(const Value& value, StorageAffinity affinity) noexcept;
void ValidateTableSchema(TableSchema& table);
void ValidateSchemaTables(int version, const std::vector<TableSchema>& tables);

template <class T> struct IsDefault : std::false_type {};

template <class T> struct IsTable : std::false_type {};
template <class T> struct IsColumn : std::false_type {};

struct SchemaAccess;
struct MigrationAccess;

} // namespace detail

/// Marks a declared column as the table's primary key.
struct PrimaryKey final {};

/// Marks an INTEGER primary key as generated by SQLite.
///
/// Auto-increment columns are omitted from typed insert statements. Their
/// generated values are reported through InsertResult or InsertManyResult.
struct AutoIncrement final {};

/// Adds a single-column UNIQUE constraint to a declared column.
struct Unique final {};

/// Supplies a schema default value for a column.
/// @tparam T Value type supported by ValueCodec<T>.
template <class T> struct Default final {
  /// Value encoded into the table declaration.
  T value;
};

/// Deduces a value-owning Default option.
/// @tparam T Supplied default value type.
/// @param value Value copied or moved into the option.
template <class T> Default(T value) -> Default<std::decay_t<T>>;

/// Deduces UTF-8 string storage for a string-literal default.
/// @param value Null-terminated default text.
Default(const char* value) -> Default<std::string>;

/// Declares a single-column foreign-key reference.
struct References final {
  /// Referenced table name.
  std::string table;

  /// Referenced column name.
  std::string column;
};

namespace detail {

template <class T> struct IsDefault<Default<T>> : std::true_type {};

template <class T>
concept ColumnOption = std::same_as<std::remove_cvref_t<T>, PrimaryKey> ||
                       std::same_as<std::remove_cvref_t<T>, AutoIncrement> ||
                       std::same_as<std::remove_cvref_t<T>, Unique> ||
                       std::same_as<std::remove_cvref_t<T>, References> ||
                       IsDefault<std::remove_cvref_t<T>>::value;

inline void ApplyColumnOption(ColumnSchema& column, PrimaryKey) {
  column.primary_key = true;
}

inline void ApplyColumnOption(ColumnSchema& column, AutoIncrement) {
  column.auto_increment = true;
}

inline void ApplyColumnOption(ColumnSchema& column, Unique) {
  column.unique = true;
}

inline void ApplyColumnOption(ColumnSchema& column, References reference) {
  column.foreign_key = ForeignKeySchema{
      .table = std::move(reference.table),
      .column = std::move(reference.column),
  };
}

template <class T> void ApplyColumnOption(ColumnSchema& column, Default<T> default_value) {
  auto encoded = ValueCodec<T>::Encode(default_value.value);
  if (!encoded) {
    throw std::invalid_argument("HuxerUI SQLite could not encode a column default value");
  }
  if (!ValueMatchesAffinity(*encoded, column.affinity)) {
    throw std::invalid_argument("HuxerUI SQLite column default has an incompatible storage affinity");
  }
  column.default_value = std::move(*encoded);
}

} // namespace detail

/// Maps one public record data member to a SQLite column.
///
/// The member type determines nullability and storage affinity through
/// ValueCodec. Use `std::optional<T>` for nullable columns.
///
/// @tparam Member Pointer to the mapped record data member.
///
/// @code
/// struct User {
///   std::int64_t id = 0;
///   std::string name;
///   std::optional<std::string> avatar;
/// };
///
/// auto id = sqlite::Column<&User::id>("id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{});
/// auto name = sqlite::Column<&User::name>("name", sqlite::Unique{});
/// @endcode
template <auto Member> class Column final {
  static_assert(std::is_member_object_pointer_v<decltype(Member)>);

public:
  /// Record type that owns the mapped member.
  using RecordType = typename detail::MemberPointerTraits<decltype(Member)>::RecordType;
  /// Exact declared data-member type before top-level const removal.
  using RawMemberType = typename detail::MemberPointerTraits<decltype(Member)>::MemberType;
  /// Writable data-member type used by record encoding and decoding.
  using MemberType = std::remove_cv_t<RawMemberType>;
  /// Non-optional value type used to determine key compatibility.
  using StorageType = typename detail::OptionalTraits<MemberType>::Value;

  static_assert(!std::is_const_v<RawMemberType>);

  /// Creates a typed column declaration.
  ///
  /// @tparam Options Zero or more PrimaryKey, AutoIncrement, Unique,
  /// Default, or References option types.
  /// @param name SQLite column name. The name must be nonempty and unique in
  /// the containing Table.
  /// @param options Column options applied to this declaration.
  /// @throws std::invalid_argument If an option is invalid or its default
  /// value cannot be encoded with the member's codec.
  template <detail::ColumnOption... Options>
  explicit Column(std::string name, Options... options)
      : schema_{
            .name = std::move(name),
            .affinity = ValueCodec<MemberType>::affinity,
            .nullable = detail::OptionalTraits<MemberType>::optional,
            .integer_key_compatible =
                std::signed_integral<StorageType> && !std::same_as<StorageType, bool>,
            .member_key = detail::MemberKey<Member>(),
            .encode_member = [](const void* value) {
              const auto& record = *static_cast<const RecordType*>(value);
              return ValueCodec<MemberType>::Encode(record.*Member);
            },
            .decode_member = [](void* value, const Value& stored) -> Result<void> {
              auto decoded = ValueCodec<MemberType>::Decode(stored);
              if (!decoded) {
                return decoded.Error();
              }
              auto& record = *static_cast<RecordType*>(value);
              record.*Member = std::move(*decoded);
              return {};
            },
        } {
    (detail::ApplyColumnOption(schema_, std::move(options)), ...);
  }

private:
  template <class> friend class Table;

  detail::ColumnSchema schema_;
};

namespace detail {

template <auto Member> struct IsColumn<Column<Member>> : std::true_type {};

} // namespace detail

/// Declares an index over one or more members of the same record type.
///
/// The member order is preserved in generated SQL and when validating an upsert conflict target.
///
/// @tparam FirstMember First indexed record member.
/// @tparam Members Remaining indexed record members.
template <auto FirstMember, auto... Members> class Index final {
  static_assert(std::is_member_object_pointer_v<decltype(FirstMember)>);

public:
  /// Record type shared by every indexed member.
  using RecordType =
      typename detail::MemberPointerTraits<decltype(FirstMember)>::RecordType;

  /// Creates an index declaration.
  /// @param name SQLite index name. The name must be nonempty and unique in
  /// the containing Schema.
  /// @param unique Whether the generated index enforces uniqueness.
  explicit Index(std::string name, bool unique = false)
      : schema_{
            .name = std::move(name),
            .unique = unique,
            .member_keys = {detail::MemberKey<FirstMember>(), detail::MemberKey<Members>()...},
        } {
    static_assert((std::is_member_object_pointer_v<decltype(Members)> && ...));
    static_assert((std::same_as<
                       RecordType,
                       typename detail::MemberPointerTraits<decltype(Members)>::RecordType> &&
                   ...));
  }

private:
  template <class> friend class Table;

  detail::IndexSchema schema_;
};

template <class Record> class Table;
template <class Record> class Predicate;
template <class Record, auto Member> class ColumnExpression;
template <class Record> class FieldAssignment;
template <class Record> class ConflictTarget;
struct DoNothing;
template <class Record> class Selection;
template <class Record> class TransactionSelection;

namespace detail {

template <class T> struct IsIndex : std::false_type {};
template <auto FirstMember, auto... Members>
struct IsIndex<Index<FirstMember, Members...>> : std::true_type {};

template <class Element, class Record>
concept TableElement = requires {
  typename std::remove_cvref_t<Element>::RecordType;
} && std::same_as<typename std::remove_cvref_t<Element>::RecordType, Record> &&
                       (IsColumn<std::remove_cvref_t<Element>>::value ||
                        IsIndex<std::remove_cvref_t<Element>>::value);

template <class Record> struct IsTable<Table<Record>> : std::true_type {};

} // namespace detail

class Schema;

/// Declares the complete typed mapping for a plain C++ record.
///
/// A table must contain at least one Column and exactly one primary key. Every
/// Index member must also be declared as a Column in the same table.
///
/// @tparam Record Default-initializable plain record type.
///
/// @code
/// const sqlite::Table<User> users{
///     "users",
///     sqlite::Column<&User::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
///     sqlite::Column<&User::name>{"name", sqlite::Unique{}},
///     sqlite::Column<&User::avatar>{"avatar"}
/// };
/// @endcode
template <class Record> class Table final {
public:
  /// Application record type mapped by this table.
  using RecordType = Record;

  /// Creates and validates a table declaration.
  /// @tparam Elements Column and Index declarations for `Record`.
  /// @param name SQLite table name.
  /// @param elements Typed columns and indexes belonging to this table.
  /// @throws std::invalid_argument If the table name, columns, indexes,
  /// primary key, defaults, or relationships are structurally invalid.
  template <class... Elements>
    requires(sizeof...(Elements) > 0) && (detail::TableElement<Elements, Record> && ...)
  explicit Table(std::string name, const Elements&... elements) : schema_{.name = std::move(name)} {
    (Add(elements), ...);
    detail::ValidateTableSchema(schema_);
  }

  /// Creates a typed expression for a declared member.
  /// @tparam Member Pointer to a member declared by this table.
  /// @return A column expression used by predicates, ordering, Set, and
  /// OnConflict.
  /// @throws std::invalid_argument If `Member` was not declared in this table.
  template <auto Member>
  [[nodiscard]] ColumnExpression<Record, Member> Column() const;

private:
  template <auto Member> void Add(const sqlite::Column<Member>& column) {
    schema_.columns.push_back(column.schema_);
  }

  template <auto FirstMember, auto... Members>
  void Add(const Index<FirstMember, Members...>& index) {
    schema_.indexes.push_back(index.schema_);
  }

  friend class Schema;
  friend class Transaction;
  friend class Database;
  friend struct detail::SchemaAccess;

  detail::TableSchema schema_;
};

/// Declares the expected database schema at one application version.
///
/// Fresh databases are created directly at this version. Existing older
/// databases require a contiguous Migrations path before schema validation.
class Schema final {
public:
  /// Creates and validates a schema declaration.
  /// @tparam Tables Typed Table declaration types.
  /// @param version Positive application schema version stored in
  /// `PRAGMA user_version`.
  /// @param tables Complete set of application-managed tables.
  /// @throws std::invalid_argument If the version or table graph is invalid.
  template <class... Tables>
    requires(sizeof...(Tables) > 0) && (detail::IsTable<std::remove_cvref_t<Tables>>::value && ...)
  explicit Schema(int version, const Tables&... tables)
      : version_(version), tables_{tables.schema_...} {
    detail::ValidateSchemaTables(version_, tables_);
  }

  /// @return Declared application schema version.
  [[nodiscard]] int Version() const noexcept {
    return version_;
  }

private:
  friend struct detail::SchemaAccess;

  int version_;
  std::vector<detail::TableSchema> tables_;
};

/// Controls how SQLite opens the database file.
enum class OpenMode {
  ReadOnly,        ///< Open an existing database without write access.
  ReadWrite,       ///< Open an existing database for reads and writes.
  ReadWriteCreate, ///< Open for reads and writes, creating the file if absent.
};

/// Selects the configured SQLite journal mode.
enum class JournalMode {
  Delete, ///< Traditional rollback journal mode.
  Wal,    ///< Write-ahead logging mode for improved read/write concurrency.
};

/// Selects SQLite's conflict algorithm for typed inserts.
enum class ConflictPolicy {
  Abort,    ///< Abort the statement and preserve the surrounding transaction.
  Fail,     ///< Stop at the failing row while retaining earlier statement changes.
  Ignore,   ///< Skip rows that violate applicable constraints.
  Replace,  ///< Delete conflicting rows before inserting the new row.
  Rollback, ///< Roll back the active transaction when a constraint fails.
};

/// Options applied while opening and configuring a database connection.
struct OpenOptions {
  /// File access mode.
  OpenMode mode = OpenMode::ReadWriteCreate;
  /// Journal mode requested and verified during open.
  JournalMode journal_mode = JournalMode::Wal;
  /// Maximum time SQLite waits for a busy lock before returning ErrorCode::Busy.
  std::chrono::milliseconds busy_timeout = std::chrono::seconds{5};
  /// Creates missing parent directories before opening a writable database.
  bool create_parent_directories = false;
};

/// Result metadata returned by SQL statements that do not decode rows.
struct ExecuteResult {
  /// Rows changed by the most recently executed statement.
  std::int64_t rows_affected = 0;
  /// Connection-local SQLite last-insert row id after the statement.
  ///
  /// Prefer InsertResult::generated_primary_key for typed inserts because this
  /// value may refer to an earlier statement when no row was inserted.
  std::int64_t last_insert_row_id = 0;
};

/// Result of inserting one typed record.
struct InsertResult {
  /// Number of rows affected by the insert.
  std::int64_t rows_affected = 0;
  /// Generated integer primary key, or empty when the table does not generate
  /// one or the conflict policy skipped the row.
  std::optional<std::int64_t> generated_primary_key;
};

/// Aggregate result of inserting a sequence of typed records.
struct InsertManyResult {
  /// Sum of rows affected by the batch.
  std::int64_t rows_affected = 0;
  /// Generated key aligned with each input record. An entry is empty when its
  /// row was skipped or its table does not generate integer primary keys.
  std::vector<std::optional<std::int64_t>> generated_primary_keys;
};

/// Read-only build and runtime information for the bundled SQLite engine.
struct Diagnostics {
  /// SQLite semantic version string.
  std::string sqlite_version;
  /// Full SQLite source identifier.
  std::string sqlite_source_id;
  /// Compile-time options reported by the bundled SQLite engine.
  std::vector<std::string> compile_options;
};

namespace detail {

struct CrudStatement final {
  std::string sql;
  std::vector<Value> parameters;
  bool returns_generated_key = false;
};

[[nodiscard]] Result<CrudStatement> BuildInsertStatement(
    const TableSchema& table,
    const void* record,
    ConflictPolicy conflict_policy
);
template <class Record>
[[nodiscard]] Result<std::vector<CrudStatement>> BuildInsertManyStatements(
    const TableSchema& table,
    const std::vector<Record>& records,
    ConflictPolicy conflict_policy
) {
  std::vector<CrudStatement> statements;
  statements.reserve(records.size());
  for (const Record& record : records) {
    auto statement = BuildInsertStatement(table, &record, conflict_policy);
    if (!statement) {
      return statement.Error();
    }
    statements.push_back(std::move(*statement));
  }
  return statements;
}
[[nodiscard]] Result<CrudStatement>
BuildUpdateStatement(const TableSchema& table, const void* record);
[[nodiscard]] Result<CrudStatement>
BuildFindStatement(const TableSchema& table, Result<Value> key);
[[nodiscard]] Result<CrudStatement>
BuildDeleteStatement(const TableSchema& table, Result<Value> key);
[[nodiscard]] Result<void>
DecodeRecord(const TableSchema& table, void* record, const RowView& row);
[[nodiscard]] InsertResult
MakeInsertResult(bool returns_generated_key, const ExecuteResult& result);

} // namespace detail

/// Synchronous database access valid only inside a Database::TransactionAsync callback.
///
/// The callback runs on the database's serialized worker sequence. It must not
/// suspend, retain the Transaction, or use it after the callback returns. A
/// returned Error, exception, cancellation, or fatal SQLite transaction state
/// causes the complete transaction to roll back.
///
/// @code
/// auto transferred = co_await database.TransactionAsync(
///     [&](sqlite::Transaction& transaction) -> sqlite::Result<void> {
///       auto debited = transaction.Update(accounts, source);
///       if (!debited) return debited.Error();
///       auto credited = transaction.Update(accounts, destination);
///       if (!credited) return credited.Error();
///       return {};
///     });
/// @endcode
class Transaction final {
public:
  Transaction(const Transaction&) = delete;
  Transaction(Transaction&&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction& operator=(Transaction&&) = delete;
  ~Transaction() = default;

  /// Executes one raw SQL statement without parameters.
  /// @param sql Exactly one SQL statement. Transaction-control statements are rejected.
  /// @return Execution metadata or a typed database error.
  [[nodiscard]] Result<ExecuteResult> Execute(std::string sql);

  /// Executes one raw SQL statement with pre-encoded parameters.
  /// @param sql Exactly one SQL statement using `?` parameter placeholders.
  /// @param parameters Values bound positionally to the statement.
  /// @return Execution metadata or a typed database error.
  [[nodiscard]] Result<ExecuteResult> Execute(std::string sql, std::vector<Value> parameters);

  /// Encodes and binds application values before executing one raw statement.
  /// @tparam Arguments Types supported by ValueCodec.
  /// @param sql Exactly one SQL statement using `?` parameter placeholders.
  /// @param arguments Application values bound in argument order.
  /// @return Execution metadata or an encoding/database error.
  template <detail::EncodableValue... Arguments>
    requires(sizeof...(Arguments) > 0)
  [[nodiscard]] Result<ExecuteResult> Execute(std::string sql, Arguments&&... arguments) {
    auto parameters = detail::EncodeValues(std::forward<Arguments>(arguments)...);
    if (!parameters) {
      return parameters.Error();
    }
    return Execute(std::move(sql), std::move(*parameters));
  }

  /// Executes a raw query without parameters and decodes every row.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @param sql Exactly one row-producing SQL statement.
  /// @param decoder Row decoder invoked synchronously on the database worker.
  /// @return Decoded rows in SQLite result order, or the first error.
  template <class T, class Decoder>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T>
  [[nodiscard]] Result<std::vector<T>> Query(std::string sql, Decoder&& decoder) {
    return Query<T>(std::move(sql), std::forward<Decoder>(decoder), std::vector<Value>{});
  }

  /// Executes a raw query with pre-encoded parameters and decodes every row.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @param sql Exactly one row-producing statement using `?` placeholders.
  /// @param decoder Row decoder invoked synchronously on the database worker.
  /// @param parameters Values bound positionally to the statement.
  /// @return Decoded rows in SQLite result order, or the first error.
  template <class T, class Decoder>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T>
  [[nodiscard]] Result<std::vector<T>>
  Query(std::string sql, Decoder&& decoder, std::vector<Value> parameters) {
    std::vector<T> output;
    auto owned_decoder = std::make_shared<std::decay_t<Decoder>>(std::forward<Decoder>(decoder));
    auto result = QueryEach(
        std::move(sql), std::move(parameters),
        [owned_decoder, &output](const RowView& row) -> Result<void> {
          auto decoded = std::invoke(*owned_decoder, row);
          if (!decoded) {
            return decoded.Error();
          }
          output.push_back(std::move(*decoded));
          return {};
        }
    );
    if (!result) {
      return result.Error();
    }
    return Result<std::vector<T>>{std::move(output)};
  }

  /// Encodes parameters, executes a raw query, and decodes every row.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @tparam Arguments Types supported by ValueCodec.
  /// @param sql Exactly one row-producing statement using `?` placeholders.
  /// @param decoder Row decoder invoked synchronously on the database worker.
  /// @param arguments Application values bound in argument order.
  /// @return Decoded rows in SQLite result order, or the first error.
  template <class T, class Decoder, detail::EncodableValue... Arguments>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T> && (sizeof...(Arguments) > 0)
  [[nodiscard]] Result<std::vector<T>>
  Query(std::string sql, Decoder&& decoder, Arguments&&... arguments) {
    auto parameters = detail::EncodeValues(std::forward<Arguments>(arguments)...);
    if (!parameters) {
      return parameters.Error();
    }
    return Query<T>(std::move(sql), std::forward<Decoder>(decoder), std::move(*parameters));
  }

  /// Inserts one typed record.
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed destination table.
  /// @param record Record whose non-generated members are encoded and inserted.
  /// @param conflict_policy SQLite conflict algorithm for this insert.
  /// @return Affected-row count and an optional generated integer primary key.
  template <class Record>
  [[nodiscard]] Result<InsertResult> Insert(
      const Table<Record>& table,
      const Record& record,
      ConflictPolicy conflict_policy = ConflictPolicy::Abort
  ) {
    auto statement = detail::BuildInsertStatement(table.schema_, &record, conflict_policy);
    if (!statement) {
      return statement.Error();
    }
    const bool returns_generated_key = statement->returns_generated_key;
    auto result = Execute(std::move(statement->sql), std::move(statement->parameters));
    if (!result) {
      return result.Error();
    }
    return detail::MakeInsertResult(returns_generated_key, *result);
  }

  /// Inserts multiple typed records as one atomic batch.
  ///
  /// The insert statement is prepared once and rebound for each record. A batch failure marks the surrounding transaction for
  /// rollback even if its Result is accidentally ignored.
  ///
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed destination table.
  /// @param records Records to insert in order. An empty vector is a no-op.
  /// @param conflict_policy SQLite conflict algorithm applied to every record.
  /// @return Aggregate affected-row count and input-aligned generated keys.
  template <class Record>
  [[nodiscard]] Result<InsertManyResult> InsertMany(
      const Table<Record>& table,
      const std::vector<Record>& records,
      ConflictPolicy conflict_policy = ConflictPolicy::Abort
  ) {
    return InsertManyEncoded(
        detail::BuildInsertManyStatements(table.schema_, records, conflict_policy)
    );
  }

  /// Finds one typed record by the table's primary key.
  /// @tparam Record Plain record type declared by `table`.
  /// @tparam Key Primary-key type supported by ValueCodec.
  /// @param table Typed table to search.
  /// @param key Primary-key value to bind.
  /// @return The decoded record, an empty optional when absent, or an error.
  template <class Record, detail::EncodableValue Key>
    requires std::default_initializable<Record>
  [[nodiscard]] Result<std::optional<Record>> Find(
      const Table<Record>& table,
      const Key& key
  ) {
    auto statement = detail::BuildFindStatement(
        table.schema_, ValueCodec<std::remove_cvref_t<Key>>::Encode(key)
    );
    if (!statement) {
      return statement.Error();
    }
    auto rows = Query<Record>(
        std::move(statement->sql),
        [&table](const RowView& row) -> Result<Record> {
          Record record{};
          auto decoded = detail::DecodeRecord(table.schema_, &record, row);
          if (!decoded) {
            return decoded.Error();
          }
          return record;
        },
        std::move(statement->parameters)
    );
    if (!rows) {
      return rows.Error();
    }
    if (rows->empty()) {
      return std::optional<Record>{};
    }
    return std::optional<Record>{std::move(rows->front())};
  }

  /// Replaces every non-primary-key field of a typed record.
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed table to update.
  /// @param record Record containing its primary key and replacement field values.
  /// @return Execution metadata or an encoding/database error.
  template <class Record>
  [[nodiscard]] Result<ExecuteResult> Update(
      const Table<Record>& table,
      const Record& record
  ) {
    auto statement = detail::BuildUpdateStatement(table.schema_, &record);
    if (!statement) {
      return statement.Error();
    }
    return Execute(std::move(statement->sql), std::move(statement->parameters));
  }

  /// Updates selected fields of one row identified by its primary key.
  /// @tparam Record Plain record type declared by `table`.
  /// @tparam Key Primary-key type supported by ValueCodec.
  /// @tparam Assignments FieldAssignment types created with Set().
  /// @param table Typed table to update.
  /// @param key Primary-key value identifying the row.
  /// @param assignments One or more non-primary-key field assignments.
  /// @return Execution metadata or an encoding/database error.
  /// @throws std::invalid_argument If assignments are duplicated, target another
  /// table, or attempt to update the primary key.
  template <class Record, detail::EncodableValue Key, class... Assignments>
    requires(sizeof...(Assignments) > 0) &&
            (std::same_as<std::remove_cvref_t<Assignments>, FieldAssignment<Record>> && ...)
  [[nodiscard]] Result<ExecuteResult> UpdateFields(
      const Table<Record>& table,
      const Key& key,
      Assignments&&... assignments
  );

  /// Inserts a record or updates explicitly selected fields on a unique conflict.
  /// @tparam Record Plain record type declared by `table`.
  /// @tparam Assignments FieldAssignment types created with Set().
  /// @param table Typed destination table.
  /// @param record Record used by the insert branch.
  /// @param conflict_target Unique conflict target created with OnConflict().
  /// @param assignments Fields written by the conflict-update branch.
  /// @return Execution metadata or an encoding/database error.
  /// @throws std::invalid_argument If the target is not a declared unique key or
  /// an assignment is structurally invalid.
  template <class Record, class... Assignments>
    requires(sizeof...(Assignments) > 0) &&
            (std::same_as<std::remove_cvref_t<Assignments>, FieldAssignment<Record>> && ...)
  [[nodiscard]] Result<ExecuteResult> Upsert(
      const Table<Record>& table,
      const Record& record,
      ConflictTarget<Record> conflict_target,
      Assignments&&... assignments
  );

  /// Inserts a record or ignores a matching unique conflict.
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed destination table.
  /// @param record Record used by the insert branch.
  /// @param conflict_target Unique conflict target created with OnConflict().
  /// @param policy DoNothing policy selecting the ignore branch.
  /// @return Execution metadata or an encoding/database error.
  /// @throws std::invalid_argument If the target is not a declared unique key.
  template <class Record>
  [[nodiscard]] Result<ExecuteResult> Upsert(
      const Table<Record>& table,
      const Record& record,
      ConflictTarget<Record> conflict_target,
      DoNothing policy
  );

  /// Deletes one row by the table's primary key.
  /// @tparam Record Plain record type declared by `table`.
  /// @tparam Key Primary-key type supported by ValueCodec.
  /// @param table Typed table to modify.
  /// @param key Primary-key value identifying the row.
  /// @return Execution metadata or an encoding/database error.
  template <class Record, detail::EncodableValue Key>
  [[nodiscard]] Result<ExecuteResult> Delete(
      const Table<Record>& table,
      const Key& key
  ) {
    auto statement = detail::BuildDeleteStatement(
        table.schema_, ValueCodec<std::remove_cvref_t<Key>>::Encode(key)
    );
    if (!statement) {
      return statement.Error();
    }
    return Execute(std::move(statement->sql), std::move(statement->parameters));
  }

  /// Begins a synchronous typed selection inside this transaction.
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed source table.
  /// @return A composable TransactionSelection value.
  template <class Record>
  [[nodiscard]] TransactionSelection<Record> Select(const Table<Record>& table);

private:
  struct Impl;

  explicit Transaction(Impl* impl) : impl_(impl) {}
  [[nodiscard]] Result<void> QueryEach(
      std::string sql,
      std::vector<Value> parameters,
      std::function<Result<void>(const RowView&)> decoder
  );
  [[nodiscard]] Result<InsertManyResult> InsertManyEncoded(
      Result<std::vector<detail::CrudStatement>> statements
  );

  friend struct detail::TransactionAccess;

  Impl* impl_;
};

/// Synchronous SQL context supplied to one Migration callback.
///
/// The context is valid only while its callback is running. Migration work is
/// already inside the schema migration transaction and must not suspend.
class MigrationContext final {
public:
  MigrationContext(const MigrationContext&) = delete;
  MigrationContext(MigrationContext&&) = delete;
  MigrationContext& operator=(const MigrationContext&) = delete;
  MigrationContext& operator=(MigrationContext&&) = delete;

  /// Executes one migration statement without parameters.
  /// @param sql Exactly one SQL statement. Transaction control is rejected.
  /// @return Execution metadata or a typed database error.
  [[nodiscard]] Result<ExecuteResult> Execute(std::string sql) {
    return transaction_->Execute(std::move(sql));
  }

  /// Executes one migration statement with pre-encoded parameters.
  /// @param sql Exactly one SQL statement using `?` placeholders.
  /// @param parameters Values bound positionally to the statement.
  /// @return Execution metadata or a typed database error.
  [[nodiscard]] Result<ExecuteResult> Execute(std::string sql, std::vector<Value> parameters) {
    return transaction_->Execute(std::move(sql), std::move(parameters));
  }

  /// Encodes and binds application values for one migration statement.
  /// @tparam Arguments Types supported by ValueCodec.
  /// @param sql Exactly one SQL statement using `?` placeholders.
  /// @param arguments Application values bound in argument order.
  /// @return Execution metadata or an encoding/database error.
  template <detail::EncodableValue... Arguments>
    requires(sizeof...(Arguments) > 0)
  [[nodiscard]] Result<ExecuteResult> Execute(std::string sql, Arguments&&... arguments) {
    return transaction_->Execute(std::move(sql), std::forward<Arguments>(arguments)...);
  }

  /// Executes a migration query without parameters.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @param sql Exactly one row-producing SQL statement.
  /// @param decoder Row decoder invoked synchronously during migration.
  /// @return Decoded rows in SQLite result order, or the first error.
  template <class T, class Decoder>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T>
  [[nodiscard]] Result<std::vector<T>> Query(std::string sql, Decoder&& decoder) {
    return transaction_->Query<T>(std::move(sql), std::forward<Decoder>(decoder));
  }

  /// Executes a migration query with pre-encoded parameters.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @param sql Exactly one row-producing statement using `?` placeholders.
  /// @param decoder Row decoder invoked synchronously during migration.
  /// @param parameters Values bound positionally to the statement.
  /// @return Decoded rows in SQLite result order, or the first error.
  template <class T, class Decoder>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T>
  [[nodiscard]] Result<std::vector<T>>
  Query(std::string sql, Decoder&& decoder, std::vector<Value> parameters) {
    return transaction_->Query<T>(
        std::move(sql), std::forward<Decoder>(decoder), std::move(parameters)
    );
  }

  /// Encodes parameters, executes a migration query, and decodes every row.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @tparam Arguments Types supported by ValueCodec.
  /// @param sql Exactly one row-producing statement using `?` placeholders.
  /// @param decoder Row decoder invoked synchronously during migration.
  /// @param arguments Application values bound in argument order.
  /// @return Decoded rows in SQLite result order, or the first error.
  template <class T, class Decoder, detail::EncodableValue... Arguments>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T> && (sizeof...(Arguments) > 0)
  [[nodiscard]] Result<std::vector<T>>
  Query(std::string sql, Decoder&& decoder, Arguments&&... arguments) {
    return transaction_->Query<T>(
        std::move(sql), std::forward<Decoder>(decoder), std::forward<Arguments>(arguments)...
    );
  }

private:
  explicit MigrationContext(Transaction& transaction) : transaction_(&transaction) {}

  friend struct detail::MigrationAccess;

  Transaction* transaction_;
};

/// Declares one adjacent schema-version transition.
///
/// @code
/// sqlite::Migration add_avatar{
///     1,
///     2,
///     [](sqlite::MigrationContext& migration) -> sqlite::Result<void> {
///       auto changed = migration.Execute(
///           "ALTER TABLE users ADD COLUMN avatar TEXT");
///       if (!changed) return changed.Error();
///       return {};
///     }};
/// @endcode
class Migration final {
public:
  /// Creates a migration transition.
  /// @tparam Callback Callable taking MigrationContext& and returning Result<void>.
  /// @param from_version Existing schema version accepted by this transition.
  /// @param to_version Immediately following schema version produced on success.
  /// @param callback Synchronous migration body executed inside an atomic transaction.
  /// @throws std::invalid_argument If versions do not describe a forward transition.
  template <class Callback>
    requires std::invocable<std::decay_t<Callback>&, MigrationContext&> &&
             std::same_as<
                 std::invoke_result_t<std::decay_t<Callback>&, MigrationContext&>,
                 Result<void>>
  Migration(int from_version, int to_version, Callback&& callback)
      : from_version_(from_version), to_version_(to_version) {
    auto owned_callback =
        std::make_shared<std::decay_t<Callback>>(std::forward<Callback>(callback));
    callback_ = [owned_callback](MigrationContext& context) {
      return std::invoke(*owned_callback, context);
    };
    Validate();
  }

  /// @return Source schema version.
  [[nodiscard]] int FromVersion() const noexcept {
    return from_version_;
  }

  /// @return Destination schema version.
  [[nodiscard]] int ToVersion() const noexcept {
    return to_version_;
  }

private:
  void Validate() const;

  friend struct detail::MigrationAccess;

  int from_version_;
  int to_version_;
  std::function<Result<void>(MigrationContext&)> callback_;
};

/// Ordered set of declared schema migrations.
///
/// The set may contain historical migrations. Opening a database runs only the
/// contiguous transitions needed to reach the requested Schema version.
class Migrations final {
public:
  Migrations() = default;

  /// Creates and validates an ordered migration set.
  /// @param migrations Migration transitions in ascending version order.
  /// @throws std::invalid_argument If transitions are duplicated, overlap, or
  /// do not form a valid ordered chain.
  Migrations(std::initializer_list<Migration> migrations);

private:
  friend struct detail::MigrationAccess;

  std::vector<Migration> migrations_;
};

namespace detail {

struct SchemaAccess final {
  [[nodiscard]] static const std::vector<TableSchema>& Tables(const Schema& schema) noexcept;
};

struct MigrationAccess final {
  [[nodiscard]] static const std::vector<Migration>& Values(const Migrations& migrations) noexcept;
  [[nodiscard]] static Result<void> Run(const Migration& migration, Transaction& transaction);
};

} // namespace detail

/// Copyable handle to one serialized asynchronous SQLite connection.
///
/// Copies share the same connection, worker sequence, and close state. Public asynchronous operations are queued in call order
/// and never execute SQLite work on the owning UI thread. Awaiting from a UI-affine HuxerUI task resumes on that task's owning
/// sequence.
///
/// @code
/// const sqlite::Schema schema{3, users};
/// auto opened = co_await sqlite::Database::OpenAsync(
///     database_file, schema, UserMigrations(), {.create_parent_directories = true});
/// if (!opened) {
///   ShowError(opened.Error().Message());
///   co_return;
/// }
/// auto database = *opened;
/// @endcode
class Database final {
public:
  Database(const Database&) = default;
  Database(Database&&) noexcept = default;
  Database& operator=(const Database&) = default;
  Database& operator=(Database&&) noexcept = default;
  ~Database() = default;

  /// Performs a blocking startup query on a temporary read-only connection.
  /// Does not create a database, migrate schema, or change journal mode.
  /// The decoder runs on the calling thread; the connection closes before return.
  [[nodiscard]] static Result<void> QueryReadOnlySync(
      File file, std::string sql,
      const std::function<Result<void>(const RowView&)>& decoder,
      std::vector<Value> parameters = {},
      std::chrono::milliseconds busy_timeout = std::chrono::seconds{5});

  /// Opens a database without managing its schema.
  /// @param file Database file supplied through HuxerUI File.
  /// @param options Access, journal, timeout, and parent-directory options.
  /// @return A task producing an open shared handle or a typed error.
  /// @throws std::invalid_argument If options are internally inconsistent.
  [[nodiscard]] static Task<Result<Database>> OpenAsync(File file, OpenOptions options = {});

  /// Opens a database and creates, migrates, or validates its declared schema.
  /// @param file Database file supplied through HuxerUI File.
  /// @param schema Latest application schema declaration.
  /// @param migrations Ordered historical transitions used for older databases.
  /// @param options Access, journal, timeout, and parent-directory options.
  /// @return A task producing an open shared handle or a typed error.
  /// @throws std::invalid_argument If options or migration declarations are invalid.
  [[nodiscard]] static Task<Result<Database>> OpenAsync(
      File file,
      Schema schema,
      Migrations migrations = {},
      OpenOptions options = {}
  );

  /// Closes the shared connection after earlier queued operations finish.
  ///
  /// Closing through one copy closes every copy. Repeated close calls succeed.
  /// @return A task completing with success or a close error.
  [[nodiscard]] Task<Result<void>> CloseAsync() const;

  /// Executes one raw SQL statement without parameters.
  /// @param sql Exactly one SQL statement.
  /// @return A task producing execution metadata or a typed database error.
  [[nodiscard]] Task<Result<ExecuteResult>> ExecuteAsync(std::string sql) const;

  /// Executes one raw SQL statement with pre-encoded parameters.
  /// @param sql Exactly one SQL statement using `?` placeholders.
  /// @param parameters Values bound positionally to the statement.
  /// @return A task producing execution metadata or a typed database error.
  [[nodiscard]] Task<Result<ExecuteResult>> ExecuteAsync(
      std::string sql,
      std::vector<Value> parameters
  ) const;

  /// Encodes and binds application values before executing one raw statement.
  /// @tparam Arguments Types supported by ValueCodec.
  /// @param sql Exactly one SQL statement using `?` placeholders.
  /// @param arguments Application values bound in argument order.
  /// @return A task producing execution metadata or an encoding/database error.
  template <detail::EncodableValue... Arguments>
    requires(sizeof...(Arguments) > 0)
  [[nodiscard]] Task<Result<ExecuteResult>> ExecuteAsync(std::string sql, Arguments&&... arguments) const {
    return ExecuteEncodedAsync(
        state_, std::move(sql), detail::EncodeValues(std::forward<Arguments>(arguments)...)
    );
  }

  /// Executes a raw query without parameters and decodes every row.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @param sql Exactly one row-producing SQL statement.
  /// @param decoder Row decoder invoked synchronously on the database worker.
  /// @return A task producing decoded rows or the first error.
  template <class T, class Decoder>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T>
  [[nodiscard]] Task<Result<std::vector<T>>> QueryAsync(std::string sql, Decoder&& decoder) const {
    return QueryEncodedAsync<T>(
        state_, std::move(sql), std::vector<Value>{}, std::forward<Decoder>(decoder)
    );
  }

  /// Executes a raw query with pre-encoded parameters and decodes every row.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @param sql Exactly one row-producing statement using `?` placeholders.
  /// @param decoder Row decoder invoked synchronously on the database worker.
  /// @param parameters Values bound positionally to the statement.
  /// @return A task producing decoded rows or the first error.
  template <class T, class Decoder>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T>
  [[nodiscard]] Task<Result<std::vector<T>>>
  QueryAsync(std::string sql, Decoder&& decoder, std::vector<Value> parameters) const {
    return QueryEncodedAsync<T>(
        state_, std::move(sql), std::move(parameters), std::forward<Decoder>(decoder)
    );
  }

  /// Encodes parameters, executes a raw query, and decodes every row.
  /// @tparam T Value returned by the row decoder.
  /// @tparam Decoder Callable receiving `const RowView&` and returning Result<T>.
  /// @tparam Arguments Types supported by ValueCodec.
  /// @param sql Exactly one row-producing statement using `?` placeholders.
  /// @param decoder Row decoder invoked synchronously on the database worker.
  /// @param arguments Application values bound in argument order.
  /// @return A task producing decoded rows or the first error.
  template <class T, class Decoder, detail::EncodableValue... Arguments>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T> && (sizeof...(Arguments) > 0)
  [[nodiscard]] Task<Result<std::vector<T>>>
  QueryAsync(std::string sql, Decoder&& decoder, Arguments&&... arguments) const {
    return QueryEncodedAsync<T>(
        state_, std::move(sql), detail::EncodeValues(std::forward<Arguments>(arguments)...),
        std::forward<Decoder>(decoder)
    );
  }

  /// Inserts one typed record asynchronously.
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed destination table.
  /// @param record Record whose non-generated members are encoded and inserted.
  /// @param conflict_policy SQLite conflict algorithm for this insert.
  /// @return A task producing the affected-row count and optional generated key.
  template <class Record>
  [[nodiscard]] Task<Result<InsertResult>> InsertAsync(
      const Table<Record>& table,
      const Record& record,
      ConflictPolicy conflict_policy = ConflictPolicy::Abort
  ) const {
    return InsertEncodedAsync(
        state_, detail::BuildInsertStatement(table.schema_, &record, conflict_policy)
    );
  }

  /// Inserts multiple typed records as one asynchronous atomic batch.
  ///
  /// The statement is prepared once and rebound for each record. Any error rolls back all changes made by this batch. The input
  /// records are encoded before the operation is queued.
  ///
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed destination table.
  /// @param records Records to insert in order. An empty vector is a no-op.
  /// @param conflict_policy SQLite conflict algorithm applied to every record.
  /// @return A task producing aggregate affected rows and input-aligned keys.
  template <class Record>
  [[nodiscard]] Task<Result<InsertManyResult>> InsertManyAsync(
      const Table<Record>& table,
      const std::vector<Record>& records,
      ConflictPolicy conflict_policy = ConflictPolicy::Abort
  ) const {
    return InsertManyEncodedAsync(
        state_, detail::BuildInsertManyStatements(table.schema_, records, conflict_policy)
    );
  }

  /// Finds one typed record asynchronously by primary key.
  /// @tparam Record Default-initializable record type declared by `table`.
  /// @tparam Key Primary-key type supported by ValueCodec.
  /// @param table Typed table to search.
  /// @param key Primary-key value to bind.
  /// @return A task producing the record, an empty optional, or an error.
  template <class Record, detail::EncodableValue Key>
    requires std::default_initializable<Record>
  [[nodiscard]] Task<Result<std::optional<Record>>> FindAsync(
      const Table<Record>& table,
      const Key& key
  ) const {
    return FindEncodedAsync<Record>(
        state_, table.schema_,
        detail::BuildFindStatement(
            table.schema_, ValueCodec<std::remove_cvref_t<Key>>::Encode(key)
        )
    );
  }

  /// Replaces every non-primary-key field of a typed record asynchronously.
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed table to update.
  /// @param record Record containing its primary key and replacement values.
  /// @return A task producing execution metadata or an encoding/database error.
  template <class Record>
  [[nodiscard]] Task<Result<ExecuteResult>> UpdateAsync(
      const Table<Record>& table,
      const Record& record
  ) const {
    return ExecuteCrudAsync(state_, detail::BuildUpdateStatement(table.schema_, &record));
  }

  /// Updates selected fields asynchronously by primary key.
  /// @tparam Record Plain record type declared by `table`.
  /// @tparam Key Primary-key type supported by ValueCodec.
  /// @tparam Assignments FieldAssignment types created with Set().
  /// @param table Typed table to update.
  /// @param key Primary-key value identifying the row.
  /// @param assignments One or more non-primary-key field assignments.
  /// @return A task producing execution metadata or an encoding/database error.
  /// @throws std::invalid_argument If assignments are structurally invalid.
  template <class Record, detail::EncodableValue Key, class... Assignments>
    requires(sizeof...(Assignments) > 0) &&
            (std::same_as<std::remove_cvref_t<Assignments>, FieldAssignment<Record>> && ...)
  [[nodiscard]] Task<Result<ExecuteResult>> UpdateFieldsAsync(
      const Table<Record>& table,
      const Key& key,
      Assignments&&... assignments
  ) const;

  /// Inserts a record or asynchronously updates selected fields on conflict.
  /// @tparam Record Plain record type declared by `table`.
  /// @tparam Assignments FieldAssignment types created with Set().
  /// @param table Typed destination table.
  /// @param record Record used by the insert branch.
  /// @param conflict_target Unique conflict target created with OnConflict().
  /// @param assignments Fields written by the conflict-update branch.
  /// @return A task producing execution metadata or an encoding/database error.
  /// @throws std::invalid_argument If the target or assignments are invalid.
  template <class Record, class... Assignments>
    requires(sizeof...(Assignments) > 0) &&
            (std::same_as<std::remove_cvref_t<Assignments>, FieldAssignment<Record>> && ...)
  [[nodiscard]] Task<Result<ExecuteResult>> UpsertAsync(
      const Table<Record>& table,
      const Record& record,
      ConflictTarget<Record> conflict_target,
      Assignments&&... assignments
  ) const;

  /// Inserts a record or asynchronously ignores a matching unique conflict.
  /// @tparam Record Plain record type declared by `table`.
  /// @param table Typed destination table.
  /// @param record Record used by the insert branch.
  /// @param conflict_target Unique conflict target created with OnConflict().
  /// @param policy DoNothing policy selecting the ignore branch.
  /// @return A task producing execution metadata or an encoding/database error.
  /// @throws std::invalid_argument If the conflict target is invalid.
  template <class Record>
  [[nodiscard]] Task<Result<ExecuteResult>> UpsertAsync(
      const Table<Record>& table,
      const Record& record,
      ConflictTarget<Record> conflict_target,
      DoNothing policy
  ) const;

  /// Deletes one row asynchronously by primary key.
  /// @tparam Record Plain record type declared by `table`.
  /// @tparam Key Primary-key type supported by ValueCodec.
  /// @param table Typed table to modify.
  /// @param key Primary-key value identifying the row.
  /// @return A task producing execution metadata or an encoding/database error.
  template <class Record, detail::EncodableValue Key>
  [[nodiscard]] Task<Result<ExecuteResult>> DeleteAsync(
      const Table<Record>& table,
      const Key& key
  ) const {
    return ExecuteCrudAsync(
        state_, detail::BuildDeleteStatement(
                    table.schema_, ValueCodec<std::remove_cvref_t<Key>>::Encode(key)
                )
    );
  }

  /// Begins a composable asynchronous typed selection.
  /// @tparam Record Default-initializable record type declared by `table`.
  /// @param table Typed source table.
  /// @return An immutable Selection value.
  template <class Record>
  [[nodiscard]] Selection<Record> Select(const Table<Record>& table) const;

  /// Runs an explicit atomic transaction on the serialized database worker.
  ///
  /// The callback is synchronous and must return Result<T>; it cannot be a
  /// coroutine. Returning an error, throwing, cancellation, or allowing SQLite
  /// to end the transaction unexpectedly causes rollback. Do not retain the
  /// Transaction reference after the callback returns.
  ///
  /// @tparam Callback Callable taking Transaction& and returning Result<T>.
  /// @param callback Synchronous transaction body.
  /// @return A task producing the callback value after commit, or the error
  /// that caused rollback.
  template <class Callback>
    requires std::invocable<std::remove_cvref_t<Callback>&, Transaction&> &&
             detail::ResultType<std::invoke_result_t<std::remove_cvref_t<Callback>&, Transaction&>>
  [[nodiscard]] auto TransactionAsync(Callback&& callback) const {
    using CallbackResult =
        std::remove_cvref_t<std::invoke_result_t<std::remove_cvref_t<Callback>&, Transaction&>>;
    using Value = typename detail::ResultTraits<CallbackResult>::Value;
    return TransactionEncodedAsync<Value>(state_, std::forward<Callback>(callback));
  }

private:
  struct State;

  explicit Database(std::shared_ptr<State> state) : state_(std::move(state)) {}

  [[nodiscard]] static Task<Result<Database>>
  OpenEncodedAsync(std::shared_ptr<State> state, File file, OpenOptions options);
  [[nodiscard]] static Task<Result<Database>> OpenSchemaEncodedAsync(
      std::shared_ptr<State> state,
      File file,
      Schema schema,
      Migrations migrations,
      OpenOptions options
  );
  [[nodiscard]] static Task<Result<ExecuteResult>> ExecuteEncodedAsync(
      std::shared_ptr<State> state,
      std::string sql,
      Result<std::vector<Value>> parameters
  );
  [[nodiscard]] static Task<Result<ExecuteResult>> ExecuteCrudAsync(
      std::shared_ptr<State> state,
      Result<detail::CrudStatement> statement
  );
  [[nodiscard]] static Task<Result<InsertResult>> InsertEncodedAsync(
      std::shared_ptr<State> state,
      Result<detail::CrudStatement> statement
  );
  [[nodiscard]] static Task<Result<InsertManyResult>> InsertManyEncodedAsync(
      std::shared_ptr<State> state,
      Result<std::vector<detail::CrudStatement>> statements
  );
  [[nodiscard]] static Task<Result<void>> QueryEachAsync(
      std::shared_ptr<State> state,
      std::string sql,
      Result<std::vector<Value>> parameters,
      std::function<Result<void>(const RowView&)> decoder
  );
  [[nodiscard]] static Task<Result<void>> RunTransactionAsync(
      std::shared_ptr<State> state,
      std::function<Result<void>(Transaction&)> callback
  );

  template <class Record>
    requires std::default_initializable<Record>
  [[nodiscard]] static Task<Result<std::optional<Record>>> FindEncodedAsync(
      std::shared_ptr<State> state,
      detail::TableSchema table,
      Result<detail::CrudStatement> statement
  ) {
    if (!statement) {
      co_return statement.Error();
    }
    auto owned_table = std::make_shared<detail::TableSchema>(std::move(table));
    auto rows = co_await QueryEncodedAsync<Record>(
        std::move(state), std::move(statement->sql), std::move(statement->parameters),
        [owned_table](const RowView& row) -> Result<Record> {
          Record record{};
          auto decoded = detail::DecodeRecord(*owned_table, &record, row);
          if (!decoded) {
            return decoded.Error();
          }
          return record;
        }
    );
    if (!rows) {
      co_return rows.Error();
    }
    if (rows->empty()) {
      co_return std::optional<Record>{};
    }
    co_return std::optional<Record>{std::move(rows->front())};
  }

  template <class T, class Decoder>
    requires detail::RowDecoder<std::remove_cvref_t<Decoder>, T>
  [[nodiscard]] static Task<Result<std::vector<T>>> QueryEncodedAsync(
      std::shared_ptr<State> state,
      std::string sql,
      Result<std::vector<Value>> parameters,
      Decoder decoder
  ) {
    if (!parameters) {
      co_return parameters.Error();
    }
    auto output = std::make_shared<std::vector<T>>();
    auto owned_decoder = std::make_shared<Decoder>(std::move(decoder));
    auto result = co_await QueryEachAsync(
        std::move(state), std::move(sql), std::move(parameters),
        [owned_decoder, output](const RowView& row) -> Result<void> {
          auto decoded = std::invoke(*owned_decoder, row);
          if (!decoded) {
            return decoded.Error();
          }
          output->push_back(std::move(*decoded));
          return {};
        }
    );
    if (!result) {
      co_return result.Error();
    }
    co_return std::move(*output);
  }

  template <class T, class Callback>
  [[nodiscard]] static Task<Result<T>>
  TransactionEncodedAsync(std::shared_ptr<State> state, Callback callback) {
    auto owned_callback = std::make_shared<Callback>(std::move(callback));
    if constexpr (std::is_void_v<T>) {
      auto result = co_await RunTransactionAsync(
          std::move(state),
          [owned_callback](Transaction& transaction) -> Result<void> {
            return std::invoke(*owned_callback, transaction);
          }
      );
      if (!result) {
        co_return result.Error();
      }
      co_return Result<void>{};
    } else {
      auto output = std::make_shared<std::optional<T>>();
      auto result = co_await RunTransactionAsync(
          std::move(state),
          [owned_callback, output](Transaction& transaction) -> Result<void> {
            auto value = std::invoke(*owned_callback, transaction);
            if (!value) {
              return value.Error();
            }
            output->emplace(std::move(*value));
            return {};
          }
      );
      if (!result) {
        co_return result.Error();
      }
      co_return std::move(**output);
    }
  }

  std::shared_ptr<State> state_;
};

/// Returns version and compile-option diagnostics for the bundled SQLite engine.
///
/// This function is read-only and does not open a database.
/// @return SQLite version, source id, and compile options.
[[nodiscard]] Diagnostics GetDiagnostics();

} // namespace huxerui::sqlite

#include <huxerui/sqlite/query.h>
