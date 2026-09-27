#include <huxerui/sqlite.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sqlite = huxerui::sqlite;

enum class Status : std::int32_t {
  Ready = 7,
};

struct Organization {
  std::int64_t id = 0;
};

struct User {
  std::int64_t id = 0;
  std::int64_t organization_id = 0;
  std::string name;
  std::optional<std::string> avatar_url;
  bool active = true;
};

struct UserCode {
  std::string value;

  bool operator==(const UserCode&) const = default;
};

template <> struct sqlite::ValueCodec<Status> : sqlite::EnumValueCodec<Status> {};

template <> struct sqlite::ValueCodec<UserCode> {
  static constexpr sqlite::StorageAffinity affinity = sqlite::StorageAffinity::Text;

  static sqlite::Result<sqlite::Value> Encode(const UserCode& value) {
    return sqlite::ValueCodec<std::string>::Encode(value.value);
  }

  static sqlite::Result<UserCode> Decode(const sqlite::Value& value) {
    auto decoded = sqlite::ValueCodec<std::string>::Decode(value);
    if (!decoded) {
      return decoded.Error();
    }
    return UserCode{std::move(*decoded)};
  }
};

sqlite::Result<std::int64_t> DecodeId(const sqlite::RowView&);
sqlite::Result<void> UpdateTransaction(sqlite::Transaction&);
huxerui::Task<sqlite::Result<void>> SuspendTransaction(sqlite::Transaction&);
sqlite::Result<void> RunMigration(sqlite::MigrationContext&) {
  return {};
}
huxerui::Task<sqlite::Result<void>> SuspendMigration(sqlite::MigrationContext&);

template <class Callback>
concept SupportsTransaction = requires(const sqlite::Database& database, Callback callback) {
  database.TransactionAsync(std::move(callback));
};

template <class Callback>
concept SupportsMigration = requires(Callback callback) {
  sqlite::Migration{1, 2, std::move(callback)};
};

static_assert(std::same_as<
              decltype(sqlite::Database::OpenAsync(std::declval<huxerui::File>())),
              huxerui::Task<sqlite::Result<sqlite::Database>>>);
static_assert(std::same_as<
              decltype(sqlite::Database::OpenAsync(
                  std::declval<huxerui::File>(), std::declval<sqlite::Schema>()
              )),
              huxerui::Task<sqlite::Result<sqlite::Database>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().ExecuteAsync(
                  std::declval<std::string>(), std::int64_t{}, std::declval<std::string>()
              )),
              huxerui::Task<sqlite::Result<sqlite::ExecuteResult>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().CloseAsync()),
              huxerui::Task<sqlite::Result<void>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().QueryAsync<std::int64_t>(
                  std::declval<std::string>(), DecodeId
              )),
              huxerui::Task<sqlite::Result<std::vector<std::int64_t>>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().TransactionAsync(UpdateTransaction)),
              huxerui::Task<sqlite::Result<void>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().InsertAsync(
                  std::declval<const sqlite::Table<User>&>(), std::declval<const User&>()
              )),
              huxerui::Task<sqlite::Result<sqlite::InsertResult>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().InsertManyAsync(
                  std::declval<const sqlite::Table<User>&>(),
                  std::declval<const std::vector<User>&>()
              )),
              huxerui::Task<sqlite::Result<sqlite::InsertManyResult>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().FindAsync(
                  std::declval<const sqlite::Table<User>&>(), std::int64_t{}
              )),
              huxerui::Task<sqlite::Result<std::optional<User>>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().UpdateAsync(
                  std::declval<const sqlite::Table<User>&>(), std::declval<const User&>()
              )),
              huxerui::Task<sqlite::Result<sqlite::ExecuteResult>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().DeleteAsync(
                  std::declval<const sqlite::Table<User>&>(), std::int64_t{}
              )),
              huxerui::Task<sqlite::Result<sqlite::ExecuteResult>>>);
static_assert(std::same_as<
              decltype(std::declval<sqlite::Transaction&>().Insert(
                  std::declval<const sqlite::Table<User>&>(), std::declval<const User&>()
              )),
              sqlite::Result<sqlite::InsertResult>>);
static_assert(std::same_as<
              decltype(std::declval<sqlite::Transaction&>().InsertMany(
                  std::declval<const sqlite::Table<User>&>(),
                  std::declval<const std::vector<User>&>()
              )),
              sqlite::Result<sqlite::InsertManyResult>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Table<User>&>().Column<&User::active>() == true),
              sqlite::Predicate<User>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>()
                           .Select(std::declval<const sqlite::Table<User>&>())
                           .AllAsync()),
              huxerui::Task<sqlite::Result<std::vector<User>>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>()
                           .Select(std::declval<const sqlite::Table<User>&>())
                           .FirstAsync()),
              huxerui::Task<sqlite::Result<User>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>()
                           .Select(std::declval<const sqlite::Table<User>&>())
                           .FirstOrNullAsync()),
              huxerui::Task<sqlite::Result<std::optional<User>>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>()
                           .Select(std::declval<const sqlite::Table<User>&>())
                           .CountAsync()),
              huxerui::Task<sqlite::Result<std::int64_t>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>()
                           .Select(std::declval<const sqlite::Table<User>&>())
                           .ExistsAsync()),
              huxerui::Task<sqlite::Result<bool>>>);
static_assert(std::same_as<
              decltype(std::declval<sqlite::Transaction&>()
                           .Select(std::declval<const sqlite::Table<User>&>())
                           .All()),
              sqlite::Result<std::vector<User>>>);
static_assert(std::same_as<
              decltype(sqlite::Set(
                  std::declval<const sqlite::Table<User>&>().Column<&User::name>(),
                  std::declval<std::string>()
              )),
              sqlite::FieldAssignment<User>>);
static_assert(std::same_as<
              decltype(sqlite::OnConflict(
                  std::declval<const sqlite::Table<User>&>().Column<&User::name>()
              )),
              sqlite::ConflictTarget<User>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().UpdateFieldsAsync(
                  std::declval<const sqlite::Table<User>&>(), std::int64_t{},
                  sqlite::Set(
                      std::declval<const sqlite::Table<User>&>().Column<&User::active>(), true
                  )
              )),
              huxerui::Task<sqlite::Result<sqlite::ExecuteResult>>>);
static_assert(std::same_as<
              decltype(std::declval<sqlite::Transaction&>().UpdateFields(
                  std::declval<const sqlite::Table<User>&>(), std::int64_t{},
                  sqlite::Set(
                      std::declval<const sqlite::Table<User>&>().Column<&User::active>(), true
                  )
              )),
              sqlite::Result<sqlite::ExecuteResult>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().UpsertAsync(
                  std::declval<const sqlite::Table<User>&>(), std::declval<const User&>(),
                  sqlite::OnConflict(
                      std::declval<const sqlite::Table<User>&>().Column<&User::name>()
                  ),
                  sqlite::Set(
                      std::declval<const sqlite::Table<User>&>().Column<&User::active>(), true
                  )
              )),
              huxerui::Task<sqlite::Result<sqlite::ExecuteResult>>>);
static_assert(std::same_as<
              decltype(std::declval<const sqlite::Database&>().UpsertAsync(
                  std::declval<const sqlite::Table<User>&>(), std::declval<const User&>(),
                  sqlite::OnConflict(
                      std::declval<const sqlite::Table<User>&>().Column<&User::name>()
                  ),
                  sqlite::DoNothing{}
              )),
              huxerui::Task<sqlite::Result<sqlite::ExecuteResult>>>);
static_assert(std::same_as<
              decltype(std::declval<sqlite::Transaction&>().Upsert(
                  std::declval<const sqlite::Table<User>&>(), std::declval<const User&>(),
                  sqlite::OnConflict(
                      std::declval<const sqlite::Table<User>&>().Column<&User::name>()
                  ),
                  sqlite::Set(
                      std::declval<const sqlite::Table<User>&>().Column<&User::active>(), true
                  )
              )),
              sqlite::Result<sqlite::ExecuteResult>>);
static_assert(SupportsTransaction<decltype(&UpdateTransaction)>);
static_assert(!SupportsTransaction<decltype(&SuspendTransaction)>);
static_assert(SupportsMigration<decltype(&RunMigration)>);
static_assert(!SupportsMigration<decltype(&SuspendMigration)>);
static_assert(!std::copy_constructible<sqlite::RowView>);
static_assert(!std::move_constructible<sqlite::RowView>);

int main() {
  sqlite::Result<std::string> success{std::string{"value"}};
  assert(success);
  assert(*success == "value");

  sqlite::Result<std::string> failure{
      sqlite::Error{sqlite::ErrorCode::Storage, "failure", "test"},
  };
  assert(!failure);
  assert(failure.Error().Code() == sqlite::ErrorCode::Storage);
  assert(failure.Error().Operation() == "test");

  auto integer = sqlite::ValueCodec<std::int32_t>::Encode(42);
  assert(integer);
  auto decoded_integer = sqlite::ValueCodec<std::int32_t>::Decode(*integer);
  assert(decoded_integer && *decoded_integer == 42);

  auto boolean = sqlite::ValueCodec<bool>::Decode(sqlite::Value{std::int64_t{1}});
  assert(boolean && *boolean);

  auto null_value = sqlite::ValueCodec<std::optional<std::string>>::Encode(std::nullopt);
  assert(null_value && std::holds_alternative<sqlite::Null>(*null_value));

  auto status = sqlite::ValueCodec<Status>::Encode(Status::Ready);
  assert(status);
  auto decoded_status = sqlite::ValueCodec<Status>::Decode(*status);
  assert(decoded_status && *decoded_status == Status::Ready);

  const UserCode expected_code{"用户-42"};
  auto user_code = sqlite::ValueCodec<UserCode>::Encode(expected_code);
  assert(user_code);
  auto decoded_user_code = sqlite::ValueCodec<UserCode>::Decode(*user_code);
  assert(decoded_user_code && *decoded_user_code == expected_code);

  const std::string invalid_utf8{"\xC3\x28", 2};
  auto rejected_text = sqlite::ValueCodec<std::string>::Encode(invalid_utf8);
  assert(!rejected_text && rejected_text.Error().Code() == sqlite::ErrorCode::Decode);

  // Text values read back from the database are validated when they are decoded, not when the row
  // is materialized: a value nobody decodes must not fail the query, and the raw bytes stay
  // reachable through ValueCodec<Bytes>.
  auto decoded_invalid_text = sqlite::ValueCodec<std::string>::Decode(sqlite::Value{invalid_utf8});
  assert(!decoded_invalid_text && decoded_invalid_text.Error().Code() == sqlite::ErrorCode::Decode);

  auto raw_invalid_text = sqlite::ValueCodec<huxerui::Bytes>::Decode(sqlite::Value{invalid_utf8});
  assert(raw_invalid_text);
  assert(raw_invalid_text->size() == 2);
  assert((*raw_invalid_text)[0] == std::byte{0xC3} && (*raw_invalid_text)[1] == std::byte{0x28});

  auto raw_valid_text = sqlite::ValueCodec<huxerui::Bytes>::Decode(sqlite::Value{std::string{"用户-42"}});
  assert(raw_valid_text && raw_valid_text->size() == std::string{"用户-42"}.size());

  const sqlite::Table<Organization> organizations{
      "organizations",
      sqlite::Column<&Organization::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
  };
  const sqlite::Table<User> users{
      "users",
      sqlite::Column<&User::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
      sqlite::Column<&User::organization_id>{
          "organization_id", sqlite::References{"organizations", "id"}
      },
      sqlite::Column<&User::name>{"name", sqlite::Unique{}},
      sqlite::Column<&User::avatar_url>{"avatar_url"},
      sqlite::Column<&User::active>{"active", sqlite::Default{true}},
      sqlite::Index<&User::active, &User::name>{"users_active_name"},
  };
  const sqlite::Schema schema{3, organizations, users};
  assert(schema.Version() == 3);

  bool rejected_missing_primary_key = false;
  try {
    static_cast<void>(sqlite::Table<User>{
        "users_without_primary_key",
        sqlite::Column<&User::name>{"name"},
    });
  } catch (const std::invalid_argument&) {
    rejected_missing_primary_key = true;
  }
  assert(rejected_missing_primary_key);

  const User record{
      .id = 5,
      .organization_id = 1,
      .name = "Robert'); DROP TABLE users;--",
      .avatar_url = std::nullopt,
      .active = true,
  };
  const auto& user_schema = sqlite::detail::SchemaAccess::Tables(schema).at(1);
  auto insert_statement = sqlite::detail::BuildInsertStatement(
      user_schema, &record, sqlite::ConflictPolicy::Abort
  );
  assert(insert_statement);
  assert(insert_statement->sql.find(record.name) == std::string::npos);
  assert(insert_statement->parameters.size() == 4);
  assert(insert_statement->returns_generated_key);

  const std::vector<User> batch_records{
      record,
      User{
          .organization_id = 1,
          .name = "second",
          .avatar_url = std::string{"avatar"},
          .active = false,
      },
  };
  auto batch_statements = sqlite::detail::BuildInsertManyStatements(
      user_schema, batch_records, sqlite::ConflictPolicy::Abort
  );
  assert(batch_statements && batch_statements->size() == 2);
  assert((*batch_statements)[0].sql == (*batch_statements)[1].sql);
  assert((*batch_statements)[0].parameters.size() == 4);
  assert((*batch_statements)[1].parameters.size() == 4);

  auto empty_batch_statements = sqlite::detail::BuildInsertManyStatements(
      user_schema, std::vector<User>{}, sqlite::ConflictPolicy::Abort
  );
  assert(empty_batch_statements && empty_batch_statements->empty());

  auto update_statement = sqlite::detail::BuildUpdateStatement(user_schema, &record);
  assert(update_statement && update_statement->parameters.size() == 5);
  assert(std::get<std::int64_t>(update_statement->parameters.back()) == record.id);

  auto find_statement = sqlite::detail::BuildFindStatement(
      user_schema, sqlite::ValueCodec<std::int64_t>::Encode(record.id)
  );
  auto delete_statement = sqlite::detail::BuildDeleteStatement(
      user_schema, sqlite::ValueCodec<std::int64_t>::Encode(record.id)
  );
  assert(find_statement && find_statement->parameters.size() == 1);
  assert(delete_statement && delete_statement->parameters.size() == 1);

  const sqlite::InsertResult generated = sqlite::detail::MakeInsertResult(
      true, sqlite::ExecuteResult{.rows_affected = 1, .last_insert_row_id = 9}
  );
  assert(generated.rows_affected == 1);
  assert(generated.generated_primary_key == std::optional<std::int64_t>{9});

  std::vector<sqlite::detail::AssignmentData> partial_assignments;
  sqlite::detail::AssignmentAccess::Append(
      partial_assignments,
      sqlite::Set(users.Column<&User::name>(), std::string{"updated'; DROP TABLE users;--"})
  );
  sqlite::detail::AssignmentAccess::Append(
      partial_assignments, sqlite::Set(users.Column<&User::active>(), false)
  );
  auto partial_update = sqlite::detail::BuildUpdateFieldsStatement(
      user_schema, sqlite::ValueCodec<std::int64_t>::Encode(record.id),
      std::move(partial_assignments)
  );
  assert(partial_update && partial_update->parameters.size() == 3);
  assert(partial_update->sql.find("DROP TABLE") == std::string::npos);

  std::vector<sqlite::detail::AssignmentData> upsert_assignments;
  sqlite::detail::AssignmentAccess::Append(
      upsert_assignments, sqlite::Set(users.Column<&User::active>(), false)
  );
  auto upsert = sqlite::detail::BuildUpsertStatement(
      user_schema, &record,
      sqlite::detail::ConflictTargetAccess::Take(
          sqlite::OnConflict(users.Column<&User::name>())
      ),
      std::move(upsert_assignments)
  );
  assert(upsert && upsert->parameters.size() == 5);
  assert(upsert->sql.find("ON CONFLICT (\"name\") DO UPDATE SET \"active\" = ?") !=
         std::string::npos);
  assert(upsert->sql.find(record.name) == std::string::npos);

  auto upsert_do_nothing = sqlite::detail::BuildUpsertDoNothingStatement(
      user_schema, &record,
      sqlite::detail::ConflictTargetAccess::Take(
          sqlite::OnConflict(users.Column<&User::name>())
      )
  );
  assert(upsert_do_nothing && upsert_do_nothing->parameters.size() == 4);
  assert(upsert_do_nothing->sql.ends_with("ON CONFLICT (\"name\") DO NOTHING"));

  bool rejected_non_unique_conflict_target = false;
  try {
    static_cast<void>(sqlite::detail::BuildUpsertDoNothingStatement(
        user_schema, &record,
        sqlite::detail::ConflictTargetAccess::Take(
            sqlite::OnConflict(users.Column<&User::active>())
        )
    ));
  } catch (const std::invalid_argument&) {
    rejected_non_unique_conflict_target = true;
  }
  assert(rejected_non_unique_conflict_target);

  bool rejected_auto_increment_conflict_target = false;
  try {
    static_cast<void>(sqlite::detail::BuildUpsertDoNothingStatement(
        user_schema, &record,
        sqlite::detail::ConflictTargetAccess::Take(
            sqlite::OnConflict(users.Column<&User::id>())
        )
    ));
  } catch (const std::invalid_argument&) {
    rejected_auto_increment_conflict_target = true;
  }
  assert(rejected_auto_increment_conflict_target);

  bool rejected_cross_table_conflict_target = false;
  try {
    const sqlite::Table<User> archived_users{
        "archived_users",
        sqlite::Column<&User::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
        sqlite::Column<&User::organization_id>{"organization_id"},
        sqlite::Column<&User::name>{"name", sqlite::Unique{}},
        sqlite::Column<&User::avatar_url>{"avatar_url"},
        sqlite::Column<&User::active>{"active"},
    };
    static_cast<void>(sqlite::OnConflict(
        users.Column<&User::name>(), archived_users.Column<&User::name>()
    ));
  } catch (const std::invalid_argument&) {
    rejected_cross_table_conflict_target = true;
  }
  assert(rejected_cross_table_conflict_target);

  bool rejected_primary_key_assignment = false;
  try {
    std::vector<sqlite::detail::AssignmentData> assignments;
    sqlite::detail::AssignmentAccess::Append(
        assignments, sqlite::Set(users.Column<&User::id>(), std::int64_t{8})
    );
    static_cast<void>(sqlite::detail::BuildUpdateFieldsStatement(
        user_schema, sqlite::ValueCodec<std::int64_t>::Encode(record.id),
        std::move(assignments)
    ));
  } catch (const std::invalid_argument&) {
    rejected_primary_key_assignment = true;
  }
  assert(rejected_primary_key_assignment);

  bool rejected_duplicate_assignment = false;
  try {
    std::vector<sqlite::detail::AssignmentData> assignments;
    sqlite::detail::AssignmentAccess::Append(
        assignments, sqlite::Set(users.Column<&User::active>(), true)
    );
    sqlite::detail::AssignmentAccess::Append(
        assignments, sqlite::Set(users.Column<&User::active>(), false)
    );
    static_cast<void>(sqlite::detail::BuildUpdateFieldsStatement(
        user_schema, sqlite::ValueCodec<std::int64_t>::Encode(record.id),
        std::move(assignments)
    ));
  } catch (const std::invalid_argument&) {
    rejected_duplicate_assignment = true;
  }
  assert(rejected_duplicate_assignment);

  const sqlite::Migrations migrations{
      sqlite::Migration{1, 2, RunMigration},
      sqlite::Migration{2, 3, RunMigration},
  };
  static_cast<void>(migrations);

  bool rejected_invalid_column = false;
  try {
    const sqlite::Table<User> invalid{
        "users",
        sqlite::Column<&User::id>{"id", sqlite::AutoIncrement{}},
    };
    static_cast<void>(invalid);
  } catch (const std::invalid_argument&) {
    rejected_invalid_column = true;
  }
  assert(rejected_invalid_column);

  bool rejected_missing_migration = false;
  try {
    const sqlite::Migrations invalid{
        sqlite::Migration{1, 2, RunMigration},
        sqlite::Migration{3, 4, RunMigration},
    };
    static_cast<void>(invalid);
  } catch (const std::invalid_argument&) {
    rejected_missing_migration = true;
  }
  assert(rejected_missing_migration);

  const sqlite::Diagnostics diagnostics = sqlite::GetDiagnostics();
  assert(diagnostics.sqlite_version == "3.53.4");
  const auto has_option = [&diagnostics](std::string_view expected) {
    for (const std::string& option : diagnostics.compile_options) {
      if (option == expected) {
        return true;
      }
    }
    return false;
  };
  assert(has_option("DEFAULT_FOREIGN_KEYS"));
  assert(has_option("DQS=0"));
  assert(has_option("OMIT_DEPRECATED"));
  assert(has_option("OMIT_LOAD_EXTENSION"));
  assert(has_option("THREADSAFE=1"));
}
