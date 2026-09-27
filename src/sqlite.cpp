#include <huxerui/sqlite.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stop_token>
#include <utility>

#include "sqlite_backend.h"
#include "sql_builder.h"

namespace huxerui::sqlite {

namespace detail {

struct RowViewAccess final {
  [[nodiscard]] static RowView
  Create(const std::vector<std::string>& names, const std::vector<Value>& values) {
    return RowView{names, values};
  }
};

} // namespace detail

namespace {

[[nodiscard]] ErrorCode MapErrorCode(int primary_code) noexcept {
  switch (primary_code) {
  case HUXER_SQLITE_CONSTRAINT:
    return ErrorCode::Constraint;
  case HUXER_SQLITE_BUSY:
    return ErrorCode::Busy;
  case HUXER_SQLITE_LOCKED:
    return ErrorCode::Locked;
  case HUXER_SQLITE_INTERRUPT:
    return ErrorCode::Cancelled;
  case HUXER_SQLITE_AUTH:
    return ErrorCode::Transaction;
  case HUXER_SQLITE_CANTOPEN:
  case HUXER_SQLITE_CORRUPT:
  case HUXER_SQLITE_FULL:
  case HUXER_SQLITE_IOERR:
  case HUXER_SQLITE_NOTADB:
  case HUXER_SQLITE_PERM:
  case HUXER_SQLITE_READONLY:
    return ErrorCode::Storage;
  case HUXER_SQLITE_ERROR:
  case HUXER_SQLITE_MISMATCH:
  case HUXER_SQLITE_MISUSE:
  case HUXER_SQLITE_RANGE:
    return ErrorCode::Sql;
  default:
    return ErrorCode::Unknown;
  }
}

[[nodiscard]] Error
MakeError(HuxerSqliteConnection* connection, int result_code, std::string operation) {
  const int extended_code = connection ? huxer_sqlite_extended_error_code(connection) : result_code;
  const int primary_code = extended_code & 0xFF;
  const char* message = connection ? huxer_sqlite_error_message(connection)
                                   : huxer_sqlite_error_string(result_code);
  return Error{
      MapErrorCode(primary_code),
      message ? std::string{message} : std::string{"Unknown SQLite error"},
      std::move(operation),
      primary_code,
      extended_code,
  };
}

[[nodiscard]] Error CancelledError(std::string operation) {
  return Error{
      ErrorCode::Cancelled,
      "SQLite operation was cancelled",
      std::move(operation),
      HUXER_SQLITE_INTERRUPT,
      HUXER_SQLITE_INTERRUPT,
  };
}

[[nodiscard]] Error ClosedError(std::string operation) {
  return Error{ErrorCode::Closed, "The database is closed", std::move(operation)};
}

class StatementGuard final {
public:
  explicit StatementGuard(HuxerSqliteStatement* statement) : statement_(statement) {}
  StatementGuard(const StatementGuard&) = delete;
  StatementGuard& operator=(const StatementGuard&) = delete;

  ~StatementGuard() {
    if (statement_) {
      huxer_sqlite_finalize(statement_);
    }
  }

  [[nodiscard]] HuxerSqliteStatement* Get() const noexcept {
    return statement_;
  }

  [[nodiscard]] int Finalize() noexcept {
    HuxerSqliteStatement* statement = std::exchange(statement_, nullptr);
    return huxer_sqlite_finalize(statement);
  }

private:
  HuxerSqliteStatement* statement_;
};

[[nodiscard]] bool HasTrailingSql(const char* tail) {
  if (!tail) {
    return false;
  }
  while (*tail != '\0') {
    if (std::isspace(static_cast<unsigned char>(*tail)) == 0 && *tail != ';') {
      return true;
    }
    ++tail;
  }
  return false;
}

[[nodiscard]] int BindValue(HuxerSqliteStatement* statement, int index, const Value& value) {
  return std::visit(
      [statement, index](const auto& stored) -> int {
        using Stored = std::decay_t<decltype(stored)>;
        if constexpr (std::same_as<Stored, Null>) {
          return huxer_sqlite_bind_null(statement, index);
        } else if constexpr (std::same_as<Stored, std::int64_t>) {
          return huxer_sqlite_bind_int64(statement, index, stored);
        } else if constexpr (std::same_as<Stored, double>) {
          return huxer_sqlite_bind_double(statement, index, stored);
        } else if constexpr (std::same_as<Stored, bool>) {
          return huxer_sqlite_bind_int(statement, index, stored ? 1 : 0);
        } else if constexpr (std::same_as<Stored, std::string>) {
          return huxer_sqlite_bind_text(statement, index, stored.data(), stored.size());
        } else {
          return huxer_sqlite_bind_blob(statement, index, stored.data(), stored.size());
        }
      },
      value
  );
}

[[nodiscard]] Result<void> ValidateSql(const std::string& sql, std::string operation) {
  if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
    return Error{ErrorCode::Sql, "The SQL statement is too large", std::move(operation)};
  }
  if (sql.find('\0') != std::string::npos) {
    return Error{ErrorCode::Sql, "The SQL statement contains a null byte", std::move(operation)};
  }
  return {};
}

[[nodiscard]] Result<void> BindParameters(
    HuxerSqliteConnection* connection,
    HuxerSqliteStatement* statement,
    const std::vector<Value>& parameters
) {
  if (parameters.size() > static_cast<std::size_t>(INT_MAX)) {
    return Error{ErrorCode::Sql, "The SQL statement has too many parameters", "bind parameters"};
  }
  const int parameter_count = huxer_sqlite_parameter_count(statement);
  if (parameter_count != static_cast<int>(parameters.size())) {
    return Error{
        ErrorCode::Sql,
        "The SQL parameter count does not match the supplied values",
        "bind parameters",
    };
  }
  for (int index = 0; index < parameter_count; ++index) {
    const int result = BindValue(statement, index + 1, parameters[static_cast<std::size_t>(index)]);
    if (result != HUXER_SQLITE_OK) {
      return MakeError(connection, result, "bind parameter");
    }
  }
  return {};
}

[[nodiscard]] bool IsValidUtf8(const unsigned char* text, std::size_t size) noexcept {
  std::size_t index = 0;
  while (index < size) {
    const unsigned char first = text[index++];
    if (first <= 0x7F) {
      continue;
    }
    if (first >= 0xC2 && first <= 0xDF) {
      if (index >= size || text[index] < 0x80 || text[index] > 0xBF) {
        return false;
      }
      ++index;
      continue;
    }
    if (first >= 0xE0 && first <= 0xEF) {
      if (index + 1 >= size) {
        return false;
      }
      const unsigned char second = text[index];
      const unsigned char third = text[index + 1];
      const unsigned char second_minimum = first == 0xE0 ? 0xA0 : 0x80;
      const unsigned char second_maximum = first == 0xED ? 0x9F : 0xBF;
      if (second < second_minimum || second > second_maximum || third < 0x80 || third > 0xBF) {
        return false;
      }
      index += 2;
      continue;
    }
    if (first >= 0xF0 && first <= 0xF4) {
      if (index + 2 >= size) {
        return false;
      }
      const unsigned char second = text[index];
      const unsigned char third = text[index + 1];
      const unsigned char fourth = text[index + 2];
      const unsigned char second_minimum = first == 0xF0 ? 0x90 : 0x80;
      const unsigned char second_maximum = first == 0xF4 ? 0x8F : 0xBF;
      if (second < second_minimum || second > second_maximum || third < 0x80 || third > 0xBF ||
          fourth < 0x80 || fourth > 0xBF) {
        return false;
      }
      index += 3;
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] Result<Value>
ReadColumn(HuxerSqliteConnection* connection, HuxerSqliteStatement* statement, int column) {
  switch (huxer_sqlite_column_type(statement, column)) {
  case HUXER_SQLITE_INTEGER:
    return Value{huxer_sqlite_column_int64(statement, column)};
  case HUXER_SQLITE_FLOAT:
    return Value{huxer_sqlite_column_double(statement, column)};
  case HUXER_SQLITE_TEXT: {
    const unsigned char* text = huxer_sqlite_column_text(statement, column);
    const int byte_count = huxer_sqlite_column_bytes(statement, column);
    if (byte_count > 0 && !text) {
      return MakeError(connection, HUXER_SQLITE_NOMEM, "read text column");
    }
    // Text is materialized as the bytes SQLite stores. Validating UTF-8 here would fail the whole
    // query for a value the decoder may never read (invalid text is written by other tools, by
    // older versions of an application, or by hand). Validation belongs to
    // ValueCodec<std::string>::Decode, which reports it for the value that is actually decoded;
    // ValueCodec<Bytes>::Decode keeps the raw bytes available for callers that want them.
    if (byte_count == 0) {
      return Value{std::string{}};
    }
    return Value{std::string{reinterpret_cast<const char*>(text), static_cast<std::size_t>(byte_count)}};
  }
  case HUXER_SQLITE_BLOB: {
    const void* data = huxer_sqlite_column_blob(statement, column);
    const int byte_count = huxer_sqlite_column_bytes(statement, column);
    if (byte_count > 0 && !data) {
      return MakeError(connection, HUXER_SQLITE_NOMEM, "read blob column");
    }
    Bytes bytes(static_cast<std::size_t>(byte_count));
    if (byte_count > 0) {
      std::memcpy(bytes.data(), data, static_cast<std::size_t>(byte_count));
    }
    return Value{std::move(bytes)};
  }
  case HUXER_SQLITE_NULL:
    return Value{Null{}};
  default:
    return Error{ErrorCode::Decode, "SQLite returned an unknown column type", "read column"};
  }
}

[[nodiscard]] Result<ExecuteResult> ExecuteConnection(
    HuxerSqliteConnection* connection,
    std::string sql,
    std::vector<Value> parameters,
    std::stop_token stop_token
) {
  if (stop_token.stop_requested()) {
    return CancelledError("execute");
  }
  auto valid = ValidateSql(sql, "prepare statement");
  if (!valid) {
    return valid.Error();
  }

  HuxerSqliteStatement* statement = nullptr;
  const char* tail = nullptr;
  int result = huxer_sqlite_prepare(
      connection, sql.data(), static_cast<int>(sql.size()), &statement, &tail
  );
  if (result != HUXER_SQLITE_OK) {
    return MakeError(connection, result, "prepare statement");
  }
  if (!statement) {
    return Error{ErrorCode::Sql, "The SQL statement is empty", "prepare statement"};
  }
  StatementGuard guard(statement);
  if (HasTrailingSql(tail)) {
    return Error{ErrorCode::Sql, "Execute accepts exactly one SQL statement", "prepare statement"};
  }
  auto bound = BindParameters(connection, statement, parameters);
  if (!bound) {
    return bound.Error();
  }
  if (stop_token.stop_requested()) {
    return CancelledError("execute statement");
  }

  do {
    result = huxer_sqlite_step(statement);
  } while (result == HUXER_SQLITE_ROW);
  if (result != HUXER_SQLITE_DONE) {
    return MakeError(connection, result, "execute statement");
  }

  ExecuteResult execute_result{
      .rows_affected = huxer_sqlite_changes(connection),
      .last_insert_row_id = huxer_sqlite_last_insert_row_id(connection),
  };
  result = guard.Finalize();
  if (result != HUXER_SQLITE_OK) {
    return MakeError(connection, result, "finalize statement");
  }
  return execute_result;
}

[[nodiscard]] Result<InsertManyResult> ExecuteInsertManyConnection(
    HuxerSqliteConnection* connection,
    std::vector<detail::CrudStatement> statements,
    std::stop_token stop_token
) {
  InsertManyResult batch_result;
  batch_result.generated_primary_keys.reserve(statements.size());
  if (statements.empty()) {
    return batch_result;
  }
  if (stop_token.stop_requested()) {
    return CancelledError("insert batch");
  }

  const std::string& sql = statements.front().sql;
  const bool returns_generated_key = statements.front().returns_generated_key;
  auto valid = ValidateSql(sql, "prepare insert batch statement");
  if (!valid) {
    return valid.Error();
  }
  for (const detail::CrudStatement& statement : statements) {
    if (statement.sql != sql || statement.returns_generated_key != returns_generated_key) {
      throw std::logic_error("HuxerUI SQLite insert batch contains incompatible statements");
    }
  }

  HuxerSqliteStatement* prepared = nullptr;
  const char* tail = nullptr;
  int result = huxer_sqlite_prepare(
      connection, sql.data(), static_cast<int>(sql.size()), &prepared, &tail
  );
  if (result != HUXER_SQLITE_OK) {
    return MakeError(connection, result, "prepare insert batch statement");
  }
  if (!prepared) {
    return Error{
        ErrorCode::Sql,
        "The insert batch statement is empty",
        "prepare insert batch statement",
    };
  }
  StatementGuard guard(prepared);
  if (HasTrailingSql(tail)) {
    return Error{
        ErrorCode::Sql,
        "InsertMany accepts exactly one SQL statement per record",
        "prepare insert batch statement",
    };
  }

  for (std::size_t index = 0; index < statements.size(); ++index) {
    if (index > 0) {
      result = huxer_sqlite_reset(prepared);
      if (result != HUXER_SQLITE_OK) {
        return MakeError(connection, result, "reset insert batch statement");
      }
      result = huxer_sqlite_clear_bindings(prepared);
      if (result != HUXER_SQLITE_OK) {
        return MakeError(connection, result, "clear insert batch bindings");
      }
    }
    auto bound = BindParameters(connection, prepared, statements[index].parameters);
    if (!bound) {
      return bound.Error();
    }
    if (stop_token.stop_requested()) {
      return CancelledError("insert batch");
    }

    do {
      result = huxer_sqlite_step(prepared);
    } while (result == HUXER_SQLITE_ROW);
    if (result != HUXER_SQLITE_DONE) {
      return MakeError(connection, result, "execute insert batch statement");
    }

    const ExecuteResult execute_result{
        .rows_affected = huxer_sqlite_changes(connection),
        .last_insert_row_id = huxer_sqlite_last_insert_row_id(connection),
    };
    if (execute_result.rows_affected >
        std::numeric_limits<std::int64_t>::max() - batch_result.rows_affected) {
      return Error{
          ErrorCode::Sql,
          "The insert batch affected-row count overflowed",
          "execute insert batch statement",
      };
    }
    batch_result.rows_affected += execute_result.rows_affected;
    batch_result.generated_primary_keys.push_back(
        detail::MakeInsertResult(returns_generated_key, execute_result)
            .generated_primary_key
    );
  }

  result = guard.Finalize();
  if (result != HUXER_SQLITE_OK) {
    return MakeError(connection, result, "finalize insert batch statement");
  }
  return batch_result;
}

[[nodiscard]] Result<void> QueryConnection(
    HuxerSqliteConnection* connection,
    std::string sql,
    std::vector<Value> parameters,
    const std::function<Result<void>(const RowView&)>& decoder,
    std::stop_token stop_token
) {
  if (stop_token.stop_requested()) {
    return CancelledError("query");
  }
  auto valid = ValidateSql(sql, "prepare query");
  if (!valid) {
    return valid.Error();
  }

  HuxerSqliteStatement* statement = nullptr;
  const char* tail = nullptr;
  int result = huxer_sqlite_prepare(
      connection, sql.data(), static_cast<int>(sql.size()), &statement, &tail
  );
  if (result != HUXER_SQLITE_OK) {
    return MakeError(connection, result, "prepare query");
  }
  if (!statement) {
    return Error{ErrorCode::Sql, "The SQL query is empty", "prepare query"};
  }
  StatementGuard guard(statement);
  if (HasTrailingSql(tail)) {
    return Error{ErrorCode::Sql, "Query accepts exactly one SQL statement", "prepare query"};
  }
  auto bound = BindParameters(connection, statement, parameters);
  if (!bound) {
    return bound.Error();
  }

  const int column_count = huxer_sqlite_column_count(statement);
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(column_count));
  for (int column = 0; column < column_count; ++column) {
    const char* name = huxer_sqlite_column_name(statement, column);
    names.emplace_back(name ? name : "");
  }

  std::vector<Value> values;
  values.reserve(static_cast<std::size_t>(column_count));
  while ((result = huxer_sqlite_step(statement)) == HUXER_SQLITE_ROW) {
    values.clear();
    for (int column = 0; column < column_count; ++column) {
      auto value = ReadColumn(connection, statement, column);
      if (!value) {
        return value.Error();
      }
      values.push_back(std::move(*value));
    }
    const RowView row = detail::RowViewAccess::Create(names, values);
    auto decoded = decoder(row);
    if (!decoded) {
      return decoded.Error();
    }
    if (stop_token.stop_requested()) {
      return CancelledError("query");
    }
  }
  if (result != HUXER_SQLITE_DONE) {
    return MakeError(connection, result, "query");
  }
  result = guard.Finalize();
  if (result != HUXER_SQLITE_OK) {
    return MakeError(connection, result, "finalize query");
  }
  return {};
}

[[nodiscard]] std::string QuoteText(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    quoted.push_back(character);
    if (character == '\'') {
      quoted.push_back('\'');
    }
  }
  quoted.push_back('\'');
  return quoted;
}

[[nodiscard]] const char* AffinitySql(StorageAffinity affinity) noexcept {
  switch (affinity) {
  case StorageAffinity::Integer:
    return "INTEGER";
  case StorageAffinity::Real:
    return "REAL";
  case StorageAffinity::Text:
    return "TEXT";
  case StorageAffinity::Blob:
    return "BLOB";
  }
  return "BLOB";
}

[[nodiscard]] std::string ValueSql(const Value& value) {
  return std::visit(
      [](const auto& stored) -> std::string {
        using Stored = std::decay_t<decltype(stored)>;
        if constexpr (std::same_as<Stored, Null>) {
          return "NULL";
        } else if constexpr (std::same_as<Stored, std::int64_t>) {
          return std::to_string(stored);
        } else if constexpr (std::same_as<Stored, double>) {
          std::ostringstream stream;
          stream.imbue(std::locale::classic());
          stream << std::setprecision(std::numeric_limits<double>::max_digits10) << stored;
          return stream.str();
        } else if constexpr (std::same_as<Stored, bool>) {
          return stored ? "1" : "0";
        } else if constexpr (std::same_as<Stored, std::string>) {
          return QuoteText(stored);
        } else {
          static constexpr char hex[] = "0123456789ABCDEF";
          std::string literal{"X'"};
          literal.reserve(3 + stored.size() * 2);
          for (const std::byte byte : stored) {
            const unsigned int value = std::to_integer<unsigned int>(byte);
            literal.push_back(hex[value >> 4]);
            literal.push_back(hex[value & 0x0F]);
          }
          literal.push_back('\'');
          return literal;
        }
      },
      value
  );
}

[[nodiscard]] std::vector<std::string> CreationStatements(const Schema& schema) {
  std::vector<std::string> statements;
  const auto& tables = detail::SchemaAccess::Tables(schema);
  for (const detail::TableSchema& table : tables) {
    std::string sql = "CREATE TABLE " + detail::QuoteIdentifier(table.name) + " (";
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
      if (index > 0) {
        sql += ", ";
      }
      const detail::ColumnSchema& column = table.columns[index];
      sql += detail::QuoteIdentifier(column.name);
      sql += " ";
      sql += AffinitySql(column.affinity);
      if (!column.nullable) {
        sql += " NOT NULL";
      }
      if (column.primary_key) {
        sql += " PRIMARY KEY";
      }
      if (column.auto_increment) {
        sql += " AUTOINCREMENT";
      }
      if (column.unique) {
        sql += " UNIQUE";
      }
      if (column.default_value) {
        sql += " DEFAULT ";
        sql += ValueSql(*column.default_value);
      }
      if (column.foreign_key) {
        sql += " REFERENCES ";
        sql += detail::QuoteIdentifier(column.foreign_key->table);
        sql += "(";
        sql += detail::QuoteIdentifier(column.foreign_key->column);
        sql += ")";
      }
    }
    sql += ")";
    statements.push_back(std::move(sql));
  }
  for (const detail::TableSchema& table : tables) {
    for (const detail::IndexSchema& index : table.indexes) {
      std::string sql = index.unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ";
      sql += detail::QuoteIdentifier(index.name);
      sql += " ON ";
      sql += detail::QuoteIdentifier(table.name);
      sql += " (";
      for (std::size_t column = 0; column < index.columns.size(); ++column) {
        if (column > 0) {
          sql += ", ";
        }
        sql += detail::QuoteIdentifier(index.columns[column]);
      }
      sql += ")";
      statements.push_back(std::move(sql));
    }
  }
  return statements;
}

[[nodiscard]] std::optional<StorageAffinity> DeclaredAffinity(std::string type) {
  std::transform(type.begin(), type.end(), type.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  if (type.find("INT") != std::string::npos) {
    return StorageAffinity::Integer;
  }
  if (type.find("CHAR") != std::string::npos || type.find("CLOB") != std::string::npos ||
      type.find("TEXT") != std::string::npos) {
    return StorageAffinity::Text;
  }
  if (type.empty() || type.find("BLOB") != std::string::npos) {
    return StorageAffinity::Blob;
  }
  if (type.find("REAL") != std::string::npos || type.find("FLOA") != std::string::npos ||
      type.find("DOUB") != std::string::npos) {
    return StorageAffinity::Real;
  }
  return std::nullopt;
}

[[nodiscard]] Error ReclassifyError(const Error& error, ErrorCode code, std::string operation) {
  return Error{
      code,
      error.Message(),
      std::move(operation),
      error.SqlitePrimaryCode(),
      error.SqliteExtendedCode(),
  };
}

[[nodiscard]] Error MigrationFailure(const Error& error, std::string operation) {
  if (error.Code() != ErrorCode::Sql && error.Code() != ErrorCode::Transaction &&
      error.Code() != ErrorCode::Unknown) {
    return error;
  }
  return ReclassifyError(error, ErrorCode::Migration, std::move(operation));
}

} // namespace

Error::Error(
    ErrorCode code,
    std::string message,
    std::string operation,
    std::optional<int> sqlite_primary_code,
    std::optional<int> sqlite_extended_code
)
    : code_(code), message_(std::move(message)), operation_(std::move(operation)),
      sqlite_primary_code_(sqlite_primary_code), sqlite_extended_code_(sqlite_extended_code) {}

ErrorCode Error::Code() const noexcept {
  return code_;
}

const std::string& Error::Message() const noexcept {
  return message_;
}

const std::string& Error::Operation() const noexcept {
  return operation_;
}

std::optional<int> Error::SqlitePrimaryCode() const noexcept {
  return sqlite_primary_code_;
}

std::optional<int> Error::SqliteExtendedCode() const noexcept {
  return sqlite_extended_code_;
}

Result<Value> ValueCodec<bool>::Encode(bool value) {
  return Value{value};
}

Result<bool> ValueCodec<bool>::Decode(const Value& value) {
  if (const auto* boolean = std::get_if<bool>(&value)) {
    return *boolean;
  }
  if (const auto* integer = std::get_if<std::int64_t>(&value); integer && (*integer == 0 || *integer == 1)) {
    return *integer != 0;
  }
  return sqlite::Error{ErrorCode::Decode, "SQLite value is not a Boolean"};
}

Result<Value> ValueCodec<std::string>::Encode(const std::string& value) {
  const auto* text = reinterpret_cast<const unsigned char*>(value.data());
  if (!IsValidUtf8(text, value.size())) {
    return sqlite::Error{ErrorCode::Decode, "C++ string is not valid UTF-8", "encode text"};
  }
  return Value{value};
}

Result<std::string> ValueCodec<std::string>::Decode(const Value& value) {
  if (const auto* string = std::get_if<std::string>(&value)) {
    const auto* text = reinterpret_cast<const unsigned char*>(string->data());
    if (!IsValidUtf8(text, string->size())) {
      return sqlite::Error{ErrorCode::Decode, "SQLite text contains invalid UTF-8", "decode text"};
    }
    return *string;
  }
  return sqlite::Error{ErrorCode::Decode, "SQLite value is not UTF-8 text"};
}

Result<Value> ValueCodec<Bytes>::Encode(const Bytes& value) {
  return Value{value};
}

Result<Bytes> ValueCodec<Bytes>::Decode(const Value& value) {
  if (const auto* bytes = std::get_if<Bytes>(&value)) {
    return *bytes;
  }
  // Text is stored as its raw bytes, so a caller can always ask for those bytes even when the
  // value is not valid UTF-8 (this is the documented escape hatch for legacy or foreign data).
  if (const auto* text = std::get_if<std::string>(&value)) {
    Bytes bytes;
    bytes.reserve(text->size());
    for (const char character : *text) {
      bytes.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return bytes;
  }
  return sqlite::Error{ErrorCode::Decode, "SQLite value is not a byte array"};
}

std::size_t RowView::ColumnCount() const noexcept {
  return values_->size();
}

std::string_view RowView::ColumnName(std::size_t index) const {
  return names_->at(index);
}

const sqlite::Value& RowView::Value(std::size_t index) const {
  return values_->at(index);
}

std::optional<std::size_t> RowView::FindColumn(std::string_view name) const noexcept {
  for (std::size_t index = 0; index < names_->size(); ++index) {
    if ((*names_)[index] == name) {
      return index;
    }
  }
  return std::nullopt;
}

struct Transaction::Impl final {
  HuxerSqliteConnection* connection;
  std::stop_token stop_token;
  bool active = true;
  std::optional<Error> failure;
};

namespace detail {

struct TransactionAccess final {
  [[nodiscard]] static Result<void> Invoke(
      HuxerSqliteConnection* connection,
      std::stop_token stop_token,
      const std::function<Result<void>(Transaction&)>& callback
  ) {
    Transaction::Impl impl{
        .connection = connection,
        .stop_token = stop_token,
    };
    Transaction transaction{&impl};
    try {
      auto result = callback(transaction);
      if (result && impl.failure) {
        result = *impl.failure;
      }
      impl.active = false;
      return result;
    } catch (...) {
      impl.active = false;
      throw;
    }
  }

  [[nodiscard]] static Result<void> QueryEach(
      Transaction& transaction,
      std::string sql,
      std::function<Result<void>(const RowView&)> decoder
  ) {
    return transaction.QueryEach(std::move(sql), {}, std::move(decoder));
  }
};

} // namespace detail

Result<ExecuteResult> Transaction::Execute(std::string sql) {
  return Execute(std::move(sql), std::vector<Value>{});
}

Result<ExecuteResult> Transaction::Execute(std::string sql, std::vector<Value> parameters) {
  if (!impl_ || !impl_->active) {
    return Error{ErrorCode::Transaction, "The transaction is no longer active", "transaction execute"};
  }
  auto result = ExecuteConnection(
      impl_->connection, std::move(sql), std::move(parameters), impl_->stop_token
  );
  if (!huxer_sqlite_is_autocommit(impl_->connection)) {
    return result;
  }

  Error failure = result
                      ? Error{
                            ErrorCode::Transaction,
                            "The transaction ended while executing a statement",
                            "transaction execute",
                        }
                      : result.Error();
  impl_->failure = failure;
  impl_->active = false;
  if (!result) {
    return result.Error();
  }
  return failure;
}

Result<InsertManyResult> Transaction::InsertManyEncoded(
    Result<std::vector<detail::CrudStatement>> statements
) {
  if (!impl_ || !impl_->active) {
    return Error{
        ErrorCode::Transaction,
        "The transaction is no longer active",
        "transaction insert batch",
    };
  }
  if (!statements) {
    return statements.Error();
  }
  auto result = ExecuteInsertManyConnection(
      impl_->connection, std::move(*statements), impl_->stop_token
  );
  if (!result) {
    impl_->failure = result.Error();
  }
  if (huxer_sqlite_is_autocommit(impl_->connection)) {
    Error failure = result
                        ? Error{
                              ErrorCode::Transaction,
                              "The transaction ended while inserting a batch",
                              "transaction insert batch",
                          }
                        : result.Error();
    impl_->failure = failure;
    impl_->active = false;
    if (result) {
      return failure;
    }
  }
  return result;
}

Result<void> Transaction::QueryEach(
    std::string sql,
    std::vector<Value> parameters,
    std::function<Result<void>(const RowView&)> decoder
) {
  if (!impl_ || !impl_->active) {
    return Error{ErrorCode::Transaction, "The transaction is no longer active", "transaction query"};
  }
  auto result = QueryConnection(
      impl_->connection, std::move(sql), std::move(parameters), decoder, impl_->stop_token
  );
  if (!huxer_sqlite_is_autocommit(impl_->connection)) {
    return result;
  }

  Error failure = result
                      ? Error{
                            ErrorCode::Transaction,
                            "The transaction ended while querying a statement",
                            "transaction query",
                        }
                      : result.Error();
  impl_->failure = failure;
  impl_->active = false;
  if (!result) {
    return result.Error();
  }
  return failure;
}

struct Database::State final {
  struct ConnectionDiscardGuard final {
    State& state;

    ~ConnectionDiscardGuard() {
      state.DiscardPoisonedConnection();
    }
  };

  ~State() {
    if (connection) {
      huxer_sqlite_close(connection);
    }
  }

  [[nodiscard]] Result<void> Open(
      File file,
      OpenOptions options,
      std::optional<Schema> schema,
      Migrations migrations,
      std::stop_token stop_token
  ) {
    if (stop_token.stop_requested()) {
      return CancelledError("open");
    }

    if (options.create_parent_directories) {
      const auto parent = file.Parent();
      if (parent && !parent->CreateDirectories()) {
        return Error{ErrorCode::Storage, "Failed to create the database parent directory", "open"};
      }
    }
    if (stop_token.stop_requested()) {
      return CancelledError("open");
    }

    int mode = HUXER_SQLITE_OPEN_READ_WRITE_CREATE;
    switch (options.mode) {
    case OpenMode::ReadOnly:
      mode = HUXER_SQLITE_OPEN_READ_ONLY;
      break;
    case OpenMode::ReadWrite:
      mode = HUXER_SQLITE_OPEN_READ_WRITE;
      break;
    case OpenMode::ReadWriteCreate:
      mode = HUXER_SQLITE_OPEN_READ_WRITE_CREATE;
      break;
    }

    HuxerSqliteConnection* opened = nullptr;
    const std::string path = file.Path();
    const int open_result = huxer_sqlite_open(path.c_str(), mode, &opened);
    if (open_result != HUXER_SQLITE_OK) {
      Error error = MakeError(opened, open_result, "open");
      if (opened) {
        huxer_sqlite_close(opened);
      }
      return error;
    }
    auto close_opened = [](HuxerSqliteConnection* value) {
      huxer_sqlite_close(value);
    };
    std::unique_ptr<HuxerSqliteConnection, decltype(close_opened)> opened_guard(opened, close_opened);

    {
      std::stop_callback interrupt(stop_token, [opened] { huxer_sqlite_interrupt(opened); });
      if (stop_token.stop_requested()) {
        return CancelledError("open");
      }

      int result = huxer_sqlite_extended_result_codes(opened, 1);
      if (result != HUXER_SQLITE_OK) {
        return MakeError(opened, result, "enable extended result codes");
      }
      const auto timeout_count = options.busy_timeout.count();
      const int timeout = timeout_count > INT_MAX ? INT_MAX : static_cast<int>(timeout_count);
      result = huxer_sqlite_busy_timeout(opened, timeout);
      if (result != HUXER_SQLITE_OK) {
        return MakeError(opened, result, "configure busy timeout");
      }

      result = huxer_sqlite_execute(opened, "PRAGMA foreign_keys = ON");
      if (result != HUXER_SQLITE_OK) {
        return MakeError(opened, result, "enable foreign keys");
      }

      const char* journal_sql = options.journal_mode == JournalMode::Wal
                                    ? "PRAGMA journal_mode = WAL"
                                    : "PRAGMA journal_mode = DELETE";
      HuxerSqliteStatement* statement = nullptr;
      result = huxer_sqlite_prepare(opened, journal_sql, -1, &statement, nullptr);
      if (result != HUXER_SQLITE_OK) {
        return MakeError(opened, result, "configure journal mode");
      }
      if (stop_token.stop_requested()) {
        huxer_sqlite_finalize(statement);
        return CancelledError("configure journal mode");
      }
      result = huxer_sqlite_step(statement);
      if (result == HUXER_SQLITE_ROW) {
        const unsigned char* actual_text = huxer_sqlite_column_text(statement, 0);
        std::string actual = actual_text ? reinterpret_cast<const char*>(actual_text) : std::string{};
        std::transform(actual.begin(), actual.end(), actual.begin(), [](unsigned char value) {
          return static_cast<char>(std::tolower(value));
        });
        const std::string expected = options.journal_mode == JournalMode::Wal ? "wal" : "delete";
        if (actual != expected) {
          huxer_sqlite_finalize(statement);
          return Error{
              ErrorCode::Unsupported,
              "The database does not support the requested journal mode",
              "configure journal mode",
          };
        }
        result = huxer_sqlite_step(statement);
      }
      const int finalize_result = huxer_sqlite_finalize(statement);
      if (result != HUXER_SQLITE_DONE) {
        return Error{
            MapErrorCode(result & 0xFF),
            huxer_sqlite_error_string(result),
            "configure journal mode",
            result & 0xFF,
            result,
        };
      }
      if (finalize_result != HUXER_SQLITE_OK) {
        return MakeError(opened, finalize_result, "configure journal mode");
      }
    }

    connection = opened_guard.release();
    if (schema) {
      auto initialized = InitializeSchema(*schema, migrations, stop_token);
      if (!initialized) {
        Error error = initialized.Error();
        if (connection) {
          huxer_sqlite_close(connection);
          connection = nullptr;
        }
        return error;
      }
    }
    return {};
  }

  [[nodiscard]] Result<ExecuteResult>
  Execute(std::string sql, std::vector<Value> parameters, std::stop_token stop_token) {
    if (!connection) {
      return ClosedError("execute");
    }
    std::stop_callback interrupt(stop_token, [current = connection] { huxer_sqlite_interrupt(current); });
    return ExecuteConnection(connection, std::move(sql), std::move(parameters), stop_token);
  }

  [[nodiscard]] Result<InsertManyResult> InsertMany(
      std::vector<detail::CrudStatement> statements,
      std::stop_token stop_token
  ) {
    std::optional<InsertManyResult> output;
    auto result = RunTransaction(
        [this, &output, &statements, stop_token](Transaction&) -> Result<void> {
          auto inserted = ExecuteInsertManyConnection(
              connection, std::move(statements), stop_token
          );
          if (!inserted) {
            return inserted.Error();
          }
          output.emplace(std::move(*inserted));
          return {};
        },
        stop_token
    );
    if (!result) {
      return result.Error();
    }
    return std::move(*output);
  }

  [[nodiscard]] Result<void> Query(
      std::string sql,
      std::vector<Value> parameters,
      const std::function<Result<void>(const RowView&)>& decoder,
      std::stop_token stop_token
  ) {
    if (!connection) {
      return ClosedError("query");
    }
    std::stop_callback interrupt(stop_token, [current = connection] { huxer_sqlite_interrupt(current); });
    return QueryConnection(connection, std::move(sql), std::move(parameters), decoder, stop_token);
  }

  void DiscardPoisonedConnection() noexcept {
    if (!discard_connection || !connection) {
      return;
    }
    huxer_sqlite_close(connection);
    connection = nullptr;
    discard_connection = false;
  }

  [[nodiscard]] Result<void> RollbackTransaction() {
    const int result = huxer_sqlite_execute(connection, "ROLLBACK");
    transaction_active = false;
    if (result == HUXER_SQLITE_OK) {
      return {};
    }
    if (huxer_sqlite_is_autocommit(connection)) {
      return {};
    }

    Error error = MakeError(connection, result, "rollback transaction");
    discard_connection = true;
    return error;
  }

  [[nodiscard]] Result<void> RunTransaction(
      const std::function<Result<void>(Transaction&)>& callback,
      std::stop_token stop_token
  ) {
    if (!connection) {
      return ClosedError("transaction");
    }
    if (transaction_active) {
      return Error{ErrorCode::Transaction, "A transaction is already active", "begin transaction"};
    }
    if (stop_token.stop_requested()) {
      return CancelledError("transaction");
    }

    ConnectionDiscardGuard discard_guard{*this};
    std::stop_callback interrupt(stop_token, [current = connection] { huxer_sqlite_interrupt(current); });
    int result = huxer_sqlite_execute(connection, "BEGIN IMMEDIATE");
    if (result != HUXER_SQLITE_OK) {
      return MakeError(connection, result, "begin transaction");
    }
    transaction_active = true;

    result = huxer_sqlite_restrict_transaction_control(connection, 1);
    if (result != HUXER_SQLITE_OK) {
      Error error = MakeError(connection, result, "restrict transaction control");
      auto rolled_back = RollbackTransaction();
      if (!rolled_back) {
        return rolled_back.Error();
      }
      return error;
    }

    Result<void> callback_result;
    std::exception_ptr callback_exception;
    try {
      callback_result = detail::TransactionAccess::Invoke(connection, stop_token, callback);
    } catch (...) {
      callback_exception = std::current_exception();
    }

    result = huxer_sqlite_restrict_transaction_control(connection, 0);
    std::optional<Error> unrestricted_error;
    if (result != HUXER_SQLITE_OK) {
      unrestricted_error = MakeError(connection, result, "restore transaction control");
    }

    if (callback_exception || !callback_result || stop_token.stop_requested() || unrestricted_error) {
      auto rolled_back = RollbackTransaction();
      if (callback_exception) {
        std::rethrow_exception(callback_exception);
      }
      if (!rolled_back) {
        return rolled_back.Error();
      }
      if (unrestricted_error) {
        return std::move(*unrestricted_error);
      }
      if (stop_token.stop_requested()) {
        return CancelledError("transaction");
      }
      return callback_result.Error();
    }

    result = huxer_sqlite_execute(connection, "COMMIT");
    if (result != HUXER_SQLITE_OK) {
      Error commit_error = MakeError(connection, result, "commit transaction");
      auto rolled_back = RollbackTransaction();
      if (!rolled_back) {
        return rolled_back.Error();
      }
      return commit_error;
    }
    transaction_active = false;
    return {};
  }

  [[nodiscard]] Result<std::int64_t> ReadInteger(
      std::string sql,
      std::vector<Value> parameters,
      std::stop_token stop_token,
      Transaction* transaction = nullptr
  ) {
    std::optional<std::int64_t> value;
    auto decoder = [&value](const RowView& row) -> Result<void> {
          if (value) {
            return Error{ErrorCode::Sql, "SQLite scalar query returned more than one row"};
          }
          auto decoded = row.Get<std::int64_t>(0);
          if (!decoded) {
            return decoded.Error();
          }
          value = *decoded;
          return {};
        };
    Result<void> result;
    if (transaction) {
      if (!parameters.empty()) {
        return Error{ErrorCode::Sql, "Internal transaction scalar query cannot bind parameters"};
      }
      result = detail::TransactionAccess::QueryEach(
          *transaction, std::move(sql), std::move(decoder)
      );
    } else {
      result = Query(
          std::move(sql), std::move(parameters), std::move(decoder), stop_token
      );
    }
    if (!result) {
      return result.Error();
    }
    if (!value) {
      return Error{ErrorCode::Sql, "SQLite scalar query returned no rows"};
    }
    return *value;
  }

  [[nodiscard]] Result<void> ValidateSchema(
      const Schema& schema,
      std::stop_token stop_token,
      Transaction* transaction = nullptr
  ) {
    auto version = ReadInteger("PRAGMA user_version", {}, stop_token, transaction);
    if (!version) {
      return version.Error();
    }
    if (*version != schema.Version()) {
      return Error{
          ErrorCode::SchemaMismatch,
          "The committed database schema version does not match the declared schema",
          "validate schema version",
      };
    }

    struct ActualColumn final {
      std::string name;
      std::string type;
      bool not_null = false;
      bool primary_key = false;
    };

    for (const detail::TableSchema& table : detail::SchemaAccess::Tables(schema)) {
      std::vector<ActualColumn> actual_columns;
      auto decoder = [&actual_columns](const RowView& row) -> Result<void> {
            auto name = row.Get<std::string>("name");
            auto type = row.Get<std::string>("type");
            auto not_null = row.Get<bool>("notnull");
            auto primary_key = row.Get<std::int64_t>("pk");
            if (!name) {
              return name.Error();
            }
            if (!type) {
              return type.Error();
            }
            if (!not_null) {
              return not_null.Error();
            }
            if (!primary_key) {
              return primary_key.Error();
            }
            actual_columns.push_back(ActualColumn{
                .name = std::move(*name),
                .type = std::move(*type),
                .not_null = *not_null,
                .primary_key = *primary_key > 0,
            });
            return {};
          };
      Result<void> queried;
      const std::string sql = "PRAGMA table_info(" + detail::QuoteIdentifier(table.name) + ")";
      if (transaction) {
        queried = detail::TransactionAccess::QueryEach(
            *transaction, sql, std::move(decoder)
        );
      } else {
        queried = Query(sql, {}, std::move(decoder), stop_token);
      }
      if (!queried) {
        return queried.Error();
      }
      if (actual_columns.empty()) {
        return Error{
            ErrorCode::SchemaMismatch,
            "Declared table is missing: " + table.name,
            "validate schema",
        };
      }

      std::size_t actual_primary_keys = 0;
      for (const ActualColumn& actual : actual_columns) {
        if (actual.primary_key) {
          ++actual_primary_keys;
        }
      }
      std::size_t declared_primary_keys = 0;
      for (const detail::ColumnSchema& column : table.columns) {
        if (column.primary_key) {
          ++declared_primary_keys;
        }
        const auto actual = std::find_if(
            actual_columns.begin(), actual_columns.end(),
            [&column](const ActualColumn& candidate) {
              return candidate.name == column.name;
            }
        );
        if (actual == actual_columns.end()) {
          return Error{
              ErrorCode::SchemaMismatch,
              "Declared column is missing: " + table.name + "." + column.name,
              "validate schema",
          };
        }
        const auto affinity = DeclaredAffinity(actual->type);
        if (!affinity || *affinity != column.affinity || actual->not_null == column.nullable ||
            actual->primary_key != column.primary_key) {
          return Error{
              ErrorCode::SchemaMismatch,
              "Declared column does not match the database: " + table.name + "." + column.name,
              "validate schema",
          };
        }
      }
      if (actual_primary_keys != declared_primary_keys) {
        return Error{
            ErrorCode::SchemaMismatch,
            "Declared primary key does not match the database table: " + table.name,
            "validate schema",
        };
      }
    }
    return {};
  }

  [[nodiscard]] Result<void> InitializeSchema(
      const Schema& schema,
      const Migrations& migrations,
      std::stop_token stop_token
  ) {
    auto current_version = ReadInteger("PRAGMA user_version", {}, stop_token);
    if (!current_version) {
      return current_version.Error();
    }
    if (*current_version > schema.Version()) {
      return Error{
          ErrorCode::SchemaMismatch,
          "The database schema is newer than the declared application schema",
          "open schema",
      };
    }

    auto table_count = ReadInteger(
        "SELECT count(*) FROM sqlite_schema "
        "WHERE type = 'table' AND name NOT LIKE 'sqlite_%'",
        {}, stop_token
    );
    if (!table_count) {
      return table_count.Error();
    }

    const bool fresh = *current_version == 0 && *table_count == 0;
    if (fresh) {
      const std::vector<std::string> statements = CreationStatements(schema);
      auto created = RunTransaction(
          [this, &statements, &schema, stop_token](Transaction& transaction) -> Result<void> {
            for (const std::string& statement : statements) {
              auto result = transaction.Execute(statement);
              if (!result) {
                return result.Error();
              }
            }
            auto version = transaction.Execute(
                "PRAGMA user_version = " + std::to_string(schema.Version())
            );
            if (!version) {
              return version.Error();
            }
            return ValidateSchema(schema, stop_token, &transaction);
          },
          stop_token
      );
      if (!created) {
        return MigrationFailure(created.Error(), "create schema");
      }
      return {};
    }

    if (*current_version < schema.Version()) {
      std::vector<const Migration*> required;
      std::int64_t next_version = *current_version;
      for (const Migration& migration : detail::MigrationAccess::Values(migrations)) {
        if (migration.FromVersion() < next_version) {
          continue;
        }
        if (migration.FromVersion() != next_version || migration.ToVersion() > schema.Version()) {
          break;
        }
        required.push_back(&migration);
        next_version = migration.ToVersion();
        if (next_version == schema.Version()) {
          break;
        }
      }
      if (next_version != schema.Version()) {
        return Error{
            ErrorCode::Migration,
            "The database requires a migration transition that was not declared",
            "prepare migrations",
        };
      }

      auto migrated = RunTransaction(
          [this, &required, &schema, stop_token](Transaction& transaction) -> Result<void> {
            for (const Migration* migration : required) {
              auto result = detail::MigrationAccess::Run(*migration, transaction);
              if (!result) {
                return result.Error();
              }
              auto version = transaction.Execute(
                  "PRAGMA user_version = " + std::to_string(migration->ToVersion())
              );
              if (!version) {
                return version.Error();
              }
            }
            return ValidateSchema(schema, stop_token, &transaction);
          },
          stop_token
      );
      if (!migrated) {
        return MigrationFailure(migrated.Error(), "migrate schema");
      }
    }

    return ValidateSchema(schema, stop_token);
  }

  [[nodiscard]] Result<void> Close() {
    if (!connection) {
      return {};
    }
    const int result = huxer_sqlite_close(connection);
    if (result != HUXER_SQLITE_OK) {
      return MakeError(connection, result, "close");
    }
    connection = nullptr;
    return {};
  }

  WorkerSequence operations;
  HuxerSqliteConnection* connection = nullptr;
  bool transaction_active = false;
  bool discard_connection = false;
};

Result<void> Database::QueryReadOnlySync(
    File file, std::string sql,
    const std::function<Result<void>(const RowView&)>& decoder,
    std::vector<Value> parameters, std::chrono::milliseconds busy_timeout) {
  if (busy_timeout < std::chrono::milliseconds::zero() || !decoder) {
    throw std::invalid_argument("Invalid SQLite blocking read options");
  }
  HuxerSqliteConnection* opened = nullptr;
  const int result = huxer_sqlite_open(file.Path().c_str(), HUXER_SQLITE_OPEN_READ_ONLY, &opened);
  const auto close = [](HuxerSqliteConnection* value) { if (value) huxer_sqlite_close(value); };
  std::unique_ptr<HuxerSqliteConnection, decltype(close)> connection(opened, close);
  if (result != HUXER_SQLITE_OK) return MakeError(opened, result, "open read-only startup query");
  const int timeout = static_cast<int>(std::min<std::int64_t>(busy_timeout.count(), INT_MAX));
  const int configured = huxer_sqlite_busy_timeout(opened, timeout);
  if (configured != HUXER_SQLITE_OK) return MakeError(opened, configured, "configure startup query timeout");
  return QueryConnection(opened, std::move(sql), std::move(parameters), decoder, {});
}

Task<Result<Database>> Database::OpenAsync(File file, OpenOptions options) {
  if (options.busy_timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("HuxerUI SQLite busy timeout must not be negative");
  }
  if (options.mode == OpenMode::ReadOnly && options.create_parent_directories) {
    throw std::invalid_argument("HuxerUI SQLite cannot create parent directories in read-only mode");
  }
  return OpenEncodedAsync(std::make_shared<State>(), std::move(file), options);
}

Task<Result<Database>> Database::OpenAsync(
    File file,
    Schema schema,
    Migrations migrations,
    OpenOptions options
) {
  if (options.busy_timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("HuxerUI SQLite busy timeout must not be negative");
  }
  if (options.mode == OpenMode::ReadOnly && options.create_parent_directories) {
    throw std::invalid_argument("HuxerUI SQLite cannot create parent directories in read-only mode");
  }
  for (const Migration& migration : detail::MigrationAccess::Values(migrations)) {
    if (migration.ToVersion() > schema.Version()) {
      throw std::invalid_argument(
          "HuxerUI SQLite migration advances beyond the declared schema version"
      );
    }
  }
  return OpenSchemaEncodedAsync(
      std::make_shared<State>(), std::move(file), std::move(schema),
      std::move(migrations), options
  );
}

Task<Result<Database>>
Database::OpenEncodedAsync(std::shared_ptr<State> state, File file, OpenOptions options) {
  auto result = co_await state->operations.Run(
      [state](std::stop_token stop_token, File owned_file, OpenOptions owned_options) {
        return state->Open(
            std::move(owned_file), owned_options, std::nullopt, Migrations{}, stop_token
        );
      },
      std::move(file), options
  );
  if (!result) {
    co_return result.Error();
  }
  co_return Database{std::move(state)};
}

Task<Result<Database>> Database::OpenSchemaEncodedAsync(
    std::shared_ptr<State> state,
    File file,
    Schema schema,
    Migrations migrations,
    OpenOptions options
) {
  auto result = co_await state->operations.Run(
      [state](
          std::stop_token stop_token,
          File owned_file,
          Schema owned_schema,
          Migrations owned_migrations,
          OpenOptions owned_options
      ) {
        return state->Open(
            std::move(owned_file), owned_options, std::move(owned_schema),
            std::move(owned_migrations), stop_token
        );
      },
      std::move(file), std::move(schema), std::move(migrations), options
  );
  if (!result) {
    co_return result.Error();
  }
  co_return Database{std::move(state)};
}

Task<Result<void>> Database::CloseAsync() const {
  std::shared_ptr<State> state = state_;
  co_return co_await state->operations.Run(
      [state](std::stop_token) {
        return state->Close();
      }
  );
}

Task<Result<ExecuteResult>> Database::ExecuteAsync(std::string sql) const {
  return ExecuteEncodedAsync(state_, std::move(sql), std::vector<Value>{});
}

Task<Result<ExecuteResult>> Database::ExecuteAsync(std::string sql, std::vector<Value> parameters) const {
  return ExecuteEncodedAsync(state_, std::move(sql), std::move(parameters));
}

Task<Result<ExecuteResult>> Database::ExecuteEncodedAsync(
    std::shared_ptr<State> state,
    std::string sql,
    Result<std::vector<Value>> parameters
) {
  if (!parameters) {
    co_return parameters.Error();
  }
  co_return co_await state->operations.Run(
      [state](std::stop_token stop_token, std::string owned_sql, std::vector<Value> owned_parameters) {
        return state->Execute(std::move(owned_sql), std::move(owned_parameters), stop_token);
      },
      std::move(sql), std::move(*parameters)
  );
}

Task<Result<ExecuteResult>> Database::ExecuteCrudAsync(
    std::shared_ptr<State> state,
    Result<detail::CrudStatement> statement
) {
  if (!statement) {
    co_return statement.Error();
  }
  co_return co_await ExecuteEncodedAsync(
      std::move(state), std::move(statement->sql), std::move(statement->parameters)
  );
}

Task<Result<InsertResult>> Database::InsertEncodedAsync(
    std::shared_ptr<State> state,
    Result<detail::CrudStatement> statement
) {
  if (!statement) {
    co_return statement.Error();
  }
  const bool returns_generated_key = statement->returns_generated_key;
  auto result = co_await ExecuteEncodedAsync(
      std::move(state), std::move(statement->sql), std::move(statement->parameters)
  );
  if (!result) {
    co_return result.Error();
  }
  co_return detail::MakeInsertResult(returns_generated_key, *result);
}

Task<Result<InsertManyResult>> Database::InsertManyEncodedAsync(
    std::shared_ptr<State> state,
    Result<std::vector<detail::CrudStatement>> statements
) {
  if (!statements) {
    co_return statements.Error();
  }
  if (statements->empty()) {
    co_return InsertManyResult{};
  }
  co_return co_await state->operations.Run(
      [state](
          std::stop_token stop_token,
          std::vector<detail::CrudStatement> owned_statements
      ) {
        return state->InsertMany(std::move(owned_statements), stop_token);
      },
      std::move(*statements)
  );
}

Task<Result<void>> Database::QueryEachAsync(
    std::shared_ptr<State> state,
    std::string sql,
    Result<std::vector<Value>> parameters,
    std::function<Result<void>(const RowView&)> decoder
) {
  if (!parameters) {
    co_return parameters.Error();
  }
  co_return co_await state->operations.Run(
      [state](
          std::stop_token stop_token,
          std::string owned_sql,
          std::vector<Value> owned_parameters,
          std::function<Result<void>(const RowView&)> owned_decoder
      ) {
        return state->Query(
            std::move(owned_sql), std::move(owned_parameters), owned_decoder, stop_token
        );
      },
      std::move(sql), std::move(*parameters), std::move(decoder)
  );
}

Task<Result<void>> Database::RunTransactionAsync(
    std::shared_ptr<State> state,
    std::function<Result<void>(Transaction&)> callback
) {
  co_return co_await state->operations.Run(
      [state](
          std::stop_token stop_token,
          std::function<Result<void>(Transaction&)> owned_callback
      ) {
        return state->RunTransaction(owned_callback, stop_token);
      },
      std::move(callback)
  );
}

Diagnostics GetDiagnostics() {
  Diagnostics diagnostics{
      .sqlite_version = huxer_sqlite_version(),
      .sqlite_source_id = huxer_sqlite_source_id(),
  };
  for (int index = 0;; ++index) {
    const char* option = huxer_sqlite_compile_option(index);
    if (!option) {
      break;
    }
    diagnostics.compile_options.emplace_back(option);
  }
  return diagnostics;
}

} // namespace huxerui::sqlite
