#pragma once

/// @file
/// Typed predicate, ordering, and selection API for HuxerUI SQLite.

#include <algorithm>

#include <huxerui/sqlite.h>

namespace huxerui::sqlite {

/// Selects the ordering direction for one typed column.
enum class SortDirection {
  Ascending,  ///< Sort from lower values to higher values.
  Descending, ///< Sort from higher values to lower values.
};

namespace detail {

struct ColumnExpressionAccess;

struct PredicateData final {
  std::string table_name;
  std::string sql;
  std::vector<Value> parameters;
  std::optional<Error> error;
};

struct OrderData final {
  std::string table_name;
  std::string column_name;
  SortDirection direction = SortDirection::Ascending;
};

struct SelectionSpec final {
  std::optional<PredicateData> predicate;
  std::vector<OrderData> order;
  std::optional<std::int64_t> limit;
  std::optional<std::int64_t> offset;
};

[[nodiscard]] PredicateData BuildComparisonPredicate(
    std::string table_name,
    std::string column_name,
    std::string operation,
    Result<Value> value
);
[[nodiscard]] PredicateData BuildInPredicate(
    std::string table_name,
    std::string column_name,
    Result<std::vector<Value>> values
);
[[nodiscard]] PredicateData BuildNullPredicate(
    std::string table_name,
    std::string column_name,
    bool is_null
);
[[nodiscard]] PredicateData CombinePredicates(
    PredicateData left,
    PredicateData right,
    std::string operation
);
[[nodiscard]] PredicateData NegatePredicate(PredicateData predicate);
void AddWhere(SelectionSpec& spec, const std::string& table_name, PredicateData predicate);
void AddOrder(
    SelectionSpec& spec,
    const std::string& table_name,
    OrderData order
);
[[nodiscard]] Result<CrudStatement> BuildSelectStatement(
    const TableSchema& table,
    const SelectionSpec& spec,
    std::optional<std::int64_t> maximum_rows = std::nullopt
);
[[nodiscard]] Result<CrudStatement>
BuildCountStatement(const TableSchema& table, const SelectionSpec& spec);
[[nodiscard]] Result<CrudStatement>
BuildExistsStatement(const TableSchema& table, const SelectionSpec& spec);

template <class T>
[[nodiscard]] Result<std::vector<Value>> EncodeInValues(const std::vector<T>& values) {
  std::vector<Value> encoded_values;
  encoded_values.reserve(values.size());
  for (const T& value : values) {
    auto encoded = ValueCodec<T>::Encode(value);
    if (!encoded) {
      return encoded.Error();
    }
    encoded_values.push_back(std::move(*encoded));
  }
  return encoded_values;
}

} // namespace detail

/// Immutable, parameterized Boolean expression for one typed table.
///
/// Predicates are created from ColumnExpression comparisons and can be safely composed. Application values are encoded and
/// bound as SQL parameters; they are never interpolated into SQL text.
///
/// @tparam Record Record type associated with the predicate.
template <class Record> class Predicate final {
public:
  Predicate(const Predicate&) = default;
  Predicate(Predicate&&) noexcept = default;
  Predicate& operator=(const Predicate&) = default;
  Predicate& operator=(Predicate&&) noexcept = default;

  /// Combines two predicates with SQL `AND`.
  /// @param left Left-hand predicate.
  /// @param right Right-hand predicate.
  /// @return A predicate that requires both operands to match.
  /// @throws std::invalid_argument If operands belong to different tables.
  [[nodiscard]] friend Predicate operator&&(Predicate left, Predicate right) {
    return Predicate{detail::CombinePredicates(
        std::move(left.data_), std::move(right.data_), "AND"
    )};
  }

  /// Combines two predicates with SQL `OR`.
  /// @param left Left-hand predicate.
  /// @param right Right-hand predicate.
  /// @return A predicate that requires either operand to match.
  /// @throws std::invalid_argument If operands belong to different tables.
  [[nodiscard]] friend Predicate operator||(Predicate left, Predicate right) {
    return Predicate{detail::CombinePredicates(
        std::move(left.data_), std::move(right.data_), "OR"
    )};
  }

  /// Negates a predicate with SQL `NOT`.
  /// @param predicate Predicate to negate.
  /// @return The negated predicate.
  [[nodiscard]] friend Predicate operator!(Predicate predicate) {
    return Predicate{detail::NegatePredicate(std::move(predicate.data_))};
  }

private:
  explicit Predicate(detail::PredicateData data) : data_(std::move(data)) {}

  template <class, auto> friend class ColumnExpression;
  template <class> friend class Selection;
  template <class> friend class TransactionSelection;

  detail::PredicateData data_;
};

/// Typed reference to one declared table column.
///
/// Obtain expressions through Table::Column(). They retain table identity so predicates, ordering, Set(), and OnConflict() can
/// reject accidental cross-table composition.
///
/// @tparam Record Record type associated with the table.
/// @tparam Member Pointer to the represented record member.
///
/// @code
/// const auto active = users.Column<&User::active>();
/// const auto score = users.Column<&User::score>();
/// auto predicate = (active == true) && (score >= 10.0);
/// @endcode
template <class Record, auto Member> class ColumnExpression final {
  static_assert(std::is_member_object_pointer_v<decltype(Member)>);
  static_assert(std::same_as<
                Record,
                typename detail::MemberPointerTraits<decltype(Member)>::RecordType>);

public:
  /// Declared member type, including optionality.
  using MemberType = std::remove_cv_t<
      typename detail::MemberPointerTraits<decltype(Member)>::MemberType>;
  /// Non-optional type used by set membership and affinity checks.
  using StorageType = typename detail::OptionalTraits<MemberType>::Value;

  /// Creates an equality predicate.
  /// @tparam T Value constructible as MemberType.
  /// @param value Application value to encode and bind.
  /// @return A predicate using SQL `=`.
  template <class T>
    requires std::constructible_from<MemberType, T> &&
             (!std::same_as<std::remove_cvref_t<T>, std::nullopt_t>)
  [[nodiscard]] Predicate<Record> operator==(T&& value) const {
    return Compare("=", std::forward<T>(value));
  }

  /// Creates an inequality predicate.
  /// @tparam T Value constructible as MemberType.
  /// @param value Application value to encode and bind.
  /// @return A predicate using SQL `<>`.
  template <class T>
    requires std::constructible_from<MemberType, T> &&
             (!std::same_as<std::remove_cvref_t<T>, std::nullopt_t>)
  [[nodiscard]] Predicate<Record> operator!=(T&& value) const {
    return Compare("<>", std::forward<T>(value));
  }

  /// Creates a less-than predicate.
  /// @tparam T Value constructible as MemberType.
  /// @param value Application value to encode and bind.
  /// @return A predicate using SQL `<`.
  template <class T>
    requires std::constructible_from<MemberType, T> &&
             (!std::same_as<std::remove_cvref_t<T>, std::nullopt_t>)
  [[nodiscard]] Predicate<Record> operator<(T&& value) const {
    return Compare("<", std::forward<T>(value));
  }

  /// Creates a less-than-or-equal predicate.
  /// @tparam T Value constructible as MemberType.
  /// @param value Application value to encode and bind.
  /// @return A predicate using SQL `<=`.
  template <class T>
    requires std::constructible_from<MemberType, T> &&
             (!std::same_as<std::remove_cvref_t<T>, std::nullopt_t>)
  [[nodiscard]] Predicate<Record> operator<=(T&& value) const {
    return Compare("<=", std::forward<T>(value));
  }

  /// Creates a greater-than predicate.
  /// @tparam T Value constructible as MemberType.
  /// @param value Application value to encode and bind.
  /// @return A predicate using SQL `>`.
  template <class T>
    requires std::constructible_from<MemberType, T> &&
             (!std::same_as<std::remove_cvref_t<T>, std::nullopt_t>)
  [[nodiscard]] Predicate<Record> operator>(T&& value) const {
    return Compare(">", std::forward<T>(value));
  }

  /// Creates a greater-than-or-equal predicate.
  /// @tparam T Value constructible as MemberType.
  /// @param value Application value to encode and bind.
  /// @return A predicate using SQL `>=`.
  template <class T>
    requires std::constructible_from<MemberType, T> &&
             (!std::same_as<std::remove_cvref_t<T>, std::nullopt_t>)
  [[nodiscard]] Predicate<Record> operator>=(T&& value) const {
    return Compare(">=", std::forward<T>(value));
  }

  /// Tests membership in an initializer list.
  /// @param values Values encoded and bound to the SQL `IN` expression.
  /// @return A predicate that is false when `values` is empty.
  [[nodiscard]] Predicate<Record> In(std::initializer_list<StorageType> values) const {
    return In(std::vector<StorageType>{values});
  }

  /// Tests membership in an owned vector.
  /// @param values Values encoded and bound to the SQL `IN` expression.
  /// @return A predicate that is false when `values` is empty.
  [[nodiscard]] Predicate<Record> In(std::vector<StorageType> values) const {
    return Predicate<Record>{detail::BuildInPredicate(
        table_name_, column_name_, detail::EncodeInValues(values)
    )};
  }

  /// Creates a SQL `LIKE` predicate for a text-backed column.
  /// @param pattern SQLite LIKE pattern to encode and bind.
  /// @return A parameterized text-pattern predicate.
  [[nodiscard]] Predicate<Record> Like(std::string pattern) const
    requires std::same_as<StorageType, std::string>
  {
    return Predicate<Record>{detail::BuildComparisonPredicate(
        table_name_, column_name_, "LIKE", ValueCodec<std::string>::Encode(pattern)
    )};
  }

  /// Tests whether an optional column contains SQL `NULL`.
  /// @return An `IS NULL` predicate.
  [[nodiscard]] Predicate<Record> IsNull() const
    requires detail::OptionalTraits<MemberType>::optional
  {
    return Predicate<Record>{detail::BuildNullPredicate(table_name_, column_name_, true)};
  }

  /// Tests whether an optional column contains a non-null value.
  /// @return An `IS NOT NULL` predicate.
  [[nodiscard]] Predicate<Record> IsNotNull() const
    requires detail::OptionalTraits<MemberType>::optional
  {
    return Predicate<Record>{detail::BuildNullPredicate(table_name_, column_name_, false)};
  }

private:
  ColumnExpression(std::string table_name, std::string column_name)
      : table_name_(std::move(table_name)), column_name_(std::move(column_name)) {}

  template <class T> [[nodiscard]] Predicate<Record> Compare(std::string operation, T&& value) const {
    MemberType owned_value(std::forward<T>(value));
    return Predicate<Record>{detail::BuildComparisonPredicate(
        table_name_, column_name_, std::move(operation), ValueCodec<MemberType>::Encode(owned_value)
    )};
  }

  friend class Table<Record>;
  friend struct detail::ColumnExpressionAccess;
  template <class> friend class Selection;
  template <class> friend class TransactionSelection;

  std::string table_name_;
  std::string column_name_;
};

/// Immutable asynchronous typed query builder.
///
/// Each modifier returns a new Selection, so a base selection can be reused. Terminal methods queue work on the Database worker
/// and decode complete `Record` values.
///
/// @tparam Record Default-initializable record type declared by the table.
///
/// @code
/// auto rows = co_await database.Select(users)
///     .Where((users.Column<&User::active>() == true) &&
///            (users.Column<&User::score>() >= 10.0))
///     .OrderBy(users.Column<&User::score>(), sqlite::SortDirection::Descending)
///     .Limit(20)
///     .AllAsync();
/// @endcode
template <class Record> class Selection final {
public:
  /// Adds a typed SQL `WHERE` predicate.
  /// @param predicate Predicate belonging to this selection's table.
  /// @return A new selection containing the predicate.
  /// @throws std::invalid_argument If the predicate belongs to another table.
  ///
  /// Calling Where() more than once combines the predicates with SQL `AND`.
  [[nodiscard]] Selection Where(Predicate<Record> predicate) const {
    Selection selection = *this;
    detail::AddWhere(selection.spec_, selection.table_.name, std::move(predicate.data_));
    return selection;
  }

  /// Appends one typed ordering term.
  /// @tparam Member Pointer to the ordered record member.
  /// @param column Column expression belonging to this selection's table.
  /// @param direction Ascending or descending ordering direction.
  /// @return A new selection with the additional ordering term.
  /// @throws std::invalid_argument If the column belongs to another table.
  template <auto Member>
  [[nodiscard]] Selection OrderBy(
      const ColumnExpression<Record, Member>& column,
      SortDirection direction = SortDirection::Ascending
  ) const {
    Selection selection = *this;
    detail::AddOrder(
        selection.spec_, selection.table_.name,
        detail::OrderData{column.table_name_, column.column_name_, direction}
    );
    return selection;
  }

  /// Applies a maximum row count.
  /// @param value Nonnegative maximum number of returned rows.
  /// @return A new selection with this limit.
  /// @throws std::invalid_argument If `value` is negative.
  [[nodiscard]] Selection Limit(std::int64_t value) const {
    if (value < 0) {
      throw std::invalid_argument("HuxerUI SQLite query limit must not be negative");
    }
    Selection selection = *this;
    selection.spec_.limit = value;
    return selection;
  }

  /// Skips rows before producing the result.
  /// @param value Nonnegative number of rows to skip.
  /// @return A new selection with this offset.
  /// @throws std::invalid_argument If `value` is negative.
  [[nodiscard]] Selection Offset(std::int64_t value) const {
    if (value < 0) {
      throw std::invalid_argument("HuxerUI SQLite query offset must not be negative");
    }
    Selection selection = *this;
    selection.spec_.offset = value;
    return selection;
  }

  /// Executes the selection and decodes every matching row.
  /// @return A task producing rows in the requested order, or an error.
  Task<Result<std::vector<Record>>> AllAsync() const {
    return QueryRecordsAsync(
        database_, table_, detail::BuildSelectStatement(table_, spec_)
    );
  }

  /// Executes the selection and requires one matching row.
  /// @return A task producing the first row, or ErrorCode::NotFound when empty.
  Task<Result<Record>> FirstAsync() const {
    return FirstEncodedAsync(
        database_, table_, detail::BuildSelectStatement(table_, spec_, 1)
    );
  }

  /// Executes the selection and optionally returns its first row.
  /// @return A task producing the first row, an empty optional, or an error.
  Task<Result<std::optional<Record>>> FirstOrNullAsync() const {
    return FirstOrNullEncodedAsync(
        database_, table_, detail::BuildSelectStatement(table_, spec_, 1)
    );
  }

  /// Counts rows produced by the filtered, limited, and offset selection.
  /// @return A task producing the row count or an error.
  Task<Result<std::int64_t>> CountAsync() const {
    return ScalarAsync<std::int64_t>(
        database_, detail::BuildCountStatement(table_, spec_), "count typed selection"
    );
  }

  /// Tests whether the filtered, limited, and offset selection has a row.
  /// @return A task producing `true` when at least one row exists, or an error.
  Task<Result<bool>> ExistsAsync() const {
    return ScalarAsync<bool>(
        database_, detail::BuildExistsStatement(table_, spec_), "test typed selection existence"
    );
  }

private:
  Selection(Database database, detail::TableSchema table)
      : database_(std::move(database)), table_(std::move(table)) {}

  static Task<Result<std::vector<Record>>> QueryRecordsAsync(
      Database database,
      detail::TableSchema table,
      Result<detail::CrudStatement> statement
  ) {
    if (!statement) {
      co_return statement.Error();
    }
    auto owned_table = std::make_shared<detail::TableSchema>(std::move(table));
    co_return co_await database.QueryAsync<Record>(
        std::move(statement->sql),
        [owned_table](const RowView& row) -> Result<Record> {
          Record record{};
          auto decoded = detail::DecodeRecord(*owned_table, &record, row);
          if (!decoded) {
            return decoded.Error();
          }
          return record;
        },
        std::move(statement->parameters)
    );
  }

  static Task<Result<Record>> FirstEncodedAsync(
      Database database,
      detail::TableSchema table,
      Result<detail::CrudStatement> statement
  ) {
    auto result = co_await QueryRecordsAsync(
        std::move(database), std::move(table), std::move(statement)
    );
    if (!result) {
      co_return result.Error();
    }
    if (result->empty()) {
      co_return Error{
          ErrorCode::NotFound,
          "Typed selection did not return a row",
          "read first typed selection row",
      };
    }
    co_return std::move(result->front());
  }

  static Task<Result<std::optional<Record>>> FirstOrNullEncodedAsync(
      Database database,
      detail::TableSchema table,
      Result<detail::CrudStatement> statement
  ) {
    auto result = co_await QueryRecordsAsync(
        std::move(database), std::move(table), std::move(statement)
    );
    if (!result) {
      co_return result.Error();
    }
    if (result->empty()) {
      co_return std::optional<Record>{};
    }
    co_return std::optional<Record>{std::move(result->front())};
  }

  template <class T>
  static Task<Result<T>> ScalarAsync(
      Database database,
      Result<detail::CrudStatement> statement,
      std::string operation
  ) {
    if (!statement) {
      co_return statement.Error();
    }
    auto rows = co_await database.QueryAsync<T>(
        std::move(statement->sql),
        [](const RowView& row) {
          return row.Get<T>(0);
        },
        std::move(statement->parameters)
    );
    if (!rows) {
      co_return rows.Error();
    }
    if (rows->size() != 1) {
      co_return Error{
          ErrorCode::Sql,
          "Typed selection scalar query returned an unexpected row count",
          std::move(operation),
      };
    }
    T value = rows->front();
    co_return value;
  }

  friend class Database;

  Database database_;
  detail::TableSchema table_;
  detail::SelectionSpec spec_;
};

/// Immutable synchronous typed query builder for an active Transaction.
///
/// This type mirrors Selection but executes immediately on the transaction's
/// serialized worker callback. It must not outlive that callback.
///
/// @tparam Record Default-initializable record type declared by the table.
template <class Record> class TransactionSelection final {
public:
  /// Adds a typed SQL `WHERE` predicate.
  /// @param predicate Predicate belonging to this selection's table.
  /// @return A new transaction selection containing the predicate.
  /// @throws std::invalid_argument If the predicate belongs to another table.
  ///
  /// Calling Where() more than once combines the predicates with SQL `AND`.
  [[nodiscard]] TransactionSelection Where(Predicate<Record> predicate) const {
    TransactionSelection selection = *this;
    detail::AddWhere(selection.spec_, selection.table_.name, std::move(predicate.data_));
    return selection;
  }

  /// Appends one typed ordering term.
  /// @tparam Member Pointer to the ordered record member.
  /// @param column Column expression belonging to this selection's table.
  /// @param direction Ascending or descending ordering direction.
  /// @return A new transaction selection with the ordering term.
  /// @throws std::invalid_argument If the column belongs to another table.
  template <auto Member>
  [[nodiscard]] TransactionSelection OrderBy(
      const ColumnExpression<Record, Member>& column,
      SortDirection direction = SortDirection::Ascending
  ) const {
    TransactionSelection selection = *this;
    detail::AddOrder(
        selection.spec_, selection.table_.name,
        detail::OrderData{column.table_name_, column.column_name_, direction}
    );
    return selection;
  }

  /// Applies a maximum row count.
  /// @param value Nonnegative maximum number of returned rows.
  /// @return A new transaction selection with this limit.
  /// @throws std::invalid_argument If `value` is negative.
  [[nodiscard]] TransactionSelection Limit(std::int64_t value) const {
    if (value < 0) {
      throw std::invalid_argument("HuxerUI SQLite query limit must not be negative");
    }
    TransactionSelection selection = *this;
    selection.spec_.limit = value;
    return selection;
  }

  /// Skips rows before producing the result.
  /// @param value Nonnegative number of rows to skip.
  /// @return A new transaction selection with this offset.
  /// @throws std::invalid_argument If `value` is negative.
  [[nodiscard]] TransactionSelection Offset(std::int64_t value) const {
    if (value < 0) {
      throw std::invalid_argument("HuxerUI SQLite query offset must not be negative");
    }
    TransactionSelection selection = *this;
    selection.spec_.offset = value;
    return selection;
  }

  /// Executes the selection and decodes every matching row.
  /// @return Rows in the requested order, or an error.
  [[nodiscard]] Result<std::vector<Record>> All() const {
    return QueryRecords(detail::BuildSelectStatement(table_, spec_));
  }

  /// Executes the selection and requires one matching row.
  /// @return The first row, or ErrorCode::NotFound when empty.
  [[nodiscard]] Result<Record> First() const {
    auto result = QueryRecords(detail::BuildSelectStatement(table_, spec_, 1));
    if (!result) {
      return result.Error();
    }
    if (result->empty()) {
      return Error{
          ErrorCode::NotFound,
          "Typed selection did not return a row",
          "read first typed selection row",
      };
    }
    return std::move(result->front());
  }

  /// Executes the selection and optionally returns its first row.
  /// @return The first row, an empty optional, or an error.
  [[nodiscard]] Result<std::optional<Record>> FirstOrNull() const {
    auto result = QueryRecords(detail::BuildSelectStatement(table_, spec_, 1));
    if (!result) {
      return result.Error();
    }
    if (result->empty()) {
      return std::optional<Record>{};
    }
    return std::optional<Record>{std::move(result->front())};
  }

  /// Counts rows produced by the filtered, limited, and offset selection.
  /// @return The row count or an error.
  [[nodiscard]] Result<std::int64_t> Count() const {
    return Scalar<std::int64_t>(
        detail::BuildCountStatement(table_, spec_), "count typed selection"
    );
  }

  /// Tests whether the filtered, limited, and offset selection has a row.
  /// @return `true` when at least one row exists, or an error.
  [[nodiscard]] Result<bool> Exists() const {
    return Scalar<bool>(
        detail::BuildExistsStatement(table_, spec_), "test typed selection existence"
    );
  }

private:
  TransactionSelection(Transaction& transaction, detail::TableSchema table)
      : transaction_(&transaction), table_(std::move(table)) {}

  [[nodiscard]] Result<std::vector<Record>> QueryRecords(
      Result<detail::CrudStatement> statement
  ) const {
    if (!statement) {
      return statement.Error();
    }
    return transaction_->Query<Record>(
        std::move(statement->sql),
        [table = table_](const RowView& row) -> Result<Record> {
          Record record{};
          auto decoded = detail::DecodeRecord(table, &record, row);
          if (!decoded) {
            return decoded.Error();
          }
          return record;
        },
        std::move(statement->parameters)
    );
  }

  template <class T>
  [[nodiscard]] Result<T> Scalar(
      Result<detail::CrudStatement> statement,
      std::string operation
  ) const {
    if (!statement) {
      return statement.Error();
    }
    auto rows = transaction_->Query<T>(
        std::move(statement->sql),
        [](const RowView& row) {
          return row.Get<T>(0);
        },
        std::move(statement->parameters)
    );
    if (!rows) {
      return rows.Error();
    }
    if (rows->size() != 1) {
      return Error{
          ErrorCode::Sql,
          "Typed selection scalar query returned an unexpected row count",
          std::move(operation),
      };
    }
    T value = rows->front();
    return value;
  }

  friend class Transaction;

  Transaction* transaction_;
  detail::TableSchema table_;
  detail::SelectionSpec spec_;
};

template <class Record>
template <auto Member>
ColumnExpression<Record, Member> Table<Record>::Column() const {
  static_assert(std::is_member_object_pointer_v<decltype(Member)>);
  static_assert(std::same_as<
                Record,
                typename detail::MemberPointerTraits<decltype(Member)>::RecordType>);
  const void* member_key = detail::MemberKey<Member>();
  const auto column = std::find_if(
      schema_.columns.begin(), schema_.columns.end(),
      [member_key](const detail::ColumnSchema& candidate) {
        return candidate.member_key == member_key;
      }
  );
  if (column == schema_.columns.end()) {
    throw std::invalid_argument(
        "HuxerUI SQLite column expression references an undeclared record member"
    );
  }
  return ColumnExpression<Record, Member>{schema_.name, column->name};
}

template <class Record>
Selection<Record> Database::Select(const Table<Record>& table) const {
  static_assert(std::default_initializable<Record>);
  return Selection<Record>{*this, table.schema_};
}

template <class Record>
TransactionSelection<Record> Transaction::Select(const Table<Record>& table) {
  static_assert(std::default_initializable<Record>);
  return TransactionSelection<Record>{*this, table.schema_};
}

} // namespace huxerui::sqlite

#include <huxerui/sqlite/write.h>
