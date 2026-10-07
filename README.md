# HuxerUI SQLite

This downstream fork maintains application fixes on `farna/main`: read-only startup queries, per-value invalid UTF-8 decoding and primary-key schema validation. The official baseline is tracked through `upstream/main` from `HuxerUI/Lib-SQLite`. The SQLite engine remains bundled by this library; applications consume its C++ API. Consumers should pin a validated commit.

HuxerUI SQLite is an optional native persistence library for HuxerUI applications. It combines asynchronous SQLite access with plain C++ record mapping, typed queries, explicit transactions, and versioned schema migrations.

The library is separate from the HuxerUI runtime. Applications opt in through the `HuxerUI::SQLite` CMake target and include its public API explicitly:

```cpp
#include <huxerui/sqlite.h>
```

## Features

- One serialized, asynchronous SQLite connection per `sqlite::Database`
- Plain C++ records without base classes, macros, generated entities, or runtime reflection
- Typed insert, batch insert, find, update, partial update, upsert, delete, and selection operations
- Composable predicates, ordering, limits, offsets, counts, and existence checks
- Generated integer primary keys
- Parameterized raw SQL with typed row decoding
- Synchronous transaction callbacks with automatic commit or rollback
- Explicit, atomic, adjacent schema migrations
- Typed result and error values for expected database failures
- HuxerUI `File`, `Task`, `WorkerSequence`, and lifecycle integration
- A pinned SQLite amalgamation with consistent native-platform configuration

## Platform support

| Platform | Support |
| --- | --- |
| Android | Supported |
| iOS | Supported |
| macOS | Supported |
| Windows | Supported |
| Linux | Supported |
| Web | Not supported |

Persistent Web storage is intentionally outside the current library boundary. A Web implementation requires a separate design for SQLite WASM workers, OPFS ownership, browser lifecycle, and cross-origin isolation.

## Add the library to an application

Register the tagged Git repository with `huxerui_use_library` after creating the application target:

```cmake
huxerui_add_app(my_app
        SOURCES
            src/app.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
)

huxerui_use_library(my_app
        TARGET HuxerUI::SQLite
        URL "https://github.com/HuxerUI/Lib-SQLite.git"
        TAG "v0.1.0"
)
```

Use the tag for the release you intend to consume rather than a branch name.

The dependency is not included by `<huxerui/huxerui.h>`. Include `<huxerui/sqlite.h>` in every source file that uses the database API.

## Quick start

Declare an ordinary record and map its members to a table:

```cpp
#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>

#include <cstdint>
#include <optional>
#include <string>

using namespace huxerui;

struct User {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> avatar_url;
  bool active = true;
};

const sqlite::Table<User> users{
    "users",
    sqlite::Column<&User::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
    sqlite::Column<&User::name>{"name", sqlite::Unique{}},
    sqlite::Column<&User::avatar_url>{"avatar_url"},
    sqlite::Column<&User::active>{"active", sqlite::Default{true}},
    sqlite::Index<&User::active, &User::name>{"users_active_name"},
};

const sqlite::Schema schema{2, users};
```

Open the database under the application's data directory rather than using a hard-coded platform path:

```cpp
auto file_system = UseService<FileSystem>();
auto tasks = UseTaskScope();
auto database = UseState(std::optional<sqlite::Database>{});
auto error = UseState(std::string{});

const File database_file = file_system->Directories().data_directory.Child("app.sqlite3");

Lifecycle([tasks, database, error, database_file] {
  tasks.Launch([database, error, database_file]() -> Task<void> {
    sqlite::OpenOptions options{
        .journal_mode = sqlite::JournalMode::Wal,
        .busy_timeout = std::chrono::seconds{5},
        .create_parent_directories = true,
    };

    auto opened = co_await sqlite::Database::OpenAsync(database_file, schema, {}, options);
    if (!opened) {
      error = opened.Error().Message();
      co_return;
    }

    database = *opened;
  });
});
```

`UseService<FileSystem>()`, `UseTaskScope()`, `UseState()`, and `Lifecycle()` are composition-bound HuxerUI APIs. Keep this setup inside a component or move the database opening logic into a helper that receives the captured service, file, and state values.

## Typed writes and generated keys

Typed values are encoded before work enters the database queue. Application data is bound as statement parameters rather than interpolated into SQL.

```cpp
User user{
    .name = "Ada",
    .avatar_url = std::nullopt,
};

auto inserted = co_await database.InsertAsync(users, user);
if (!inserted) {
  co_return inserted.Error();
}

const std::optional<std::int64_t> user_id = inserted->generated_primary_key;
```

An auto-increment integer primary key is omitted from the generated insert statement. The database reports it through `InsertResult::generated_primary_key`; it does not mutate the input record.

Replace every non-primary-key field with `UpdateAsync`, or update only selected fields with `Set`:

```cpp
auto updated = co_await database.UpdateFieldsAsync(
    users,
    *user_id,
    sqlite::Set(users.Column<&User::name>(), "Ada Lovelace"),
    sqlite::Set(users.Column<&User::active>(), true)
);

if (!updated) {
  co_return updated.Error();
}
```

Other typed write operations include:

```cpp
auto found = co_await database.FindAsync(users, *user_id);
auto deleted = co_await database.DeleteAsync(users, *user_id);

std::vector<User> batch{
    User{.name = "Grace"},
    User{.name = "Margaret"},
};
auto inserted_many = co_await database.InsertManyAsync(users, batch);
```

`InsertManyAsync` is atomic. A failure rolls back all changes made by that batch.

For upserts, name the declared uniqueness constraint explicitly:

```cpp
User incoming{.name = "Ada", .active = true};

auto upserted = co_await database.UpsertAsync(
    users,
    incoming,
    sqlite::OnConflict(users.Column<&User::name>()),
    sqlite::Set(users.Column<&User::active>(), incoming.active)
);
```

The conflict target must match the primary key, a `Unique` column, or the ordered columns of a unique `Index`.

## Typed queries

Selections are immutable builders. Each modifier returns a new selection, and terminal operations execute asynchronously:

```cpp
auto active_users = co_await database.Select(users)
    .Where(
        (users.Column<&User::active>() == true) && users.Column<&User::name>().Like("A%")
    )
    .OrderBy(users.Column<&User::name>(), sqlite::SortDirection::Ascending)
    .Limit(50)
    .AllAsync();

if (!active_users) {
  co_return active_users.Error();
}
```

Available terminal operations are `AllAsync()`, `FirstAsync()`, `FirstOrNullAsync()`, `CountAsync()`, and `ExistsAsync()`. Predicates support comparisons, `&&`, `||`, `!`, `In(...)`, `Like(...)`, `IsNull()`, and `IsNotNull()` where the column type permits them.

```cpp
auto count = co_await database.Select(users)
    .Where(users.Column<&User::active>() == true)
    .CountAsync();

auto has_ada = co_await database.Select(users)
    .Where(users.Column<&User::name>() == std::string{"Ada"})
    .ExistsAsync();
```

## Parameterized raw SQL

Use raw SQL for joins, aggregates, common table expressions, application-specific DDL, and other behavior that is deliberately outside the typed API. Continue to pass application values as parameters:

```cpp
auto names = co_await database.QueryAsync<std::string>(
    "SELECT name FROM users WHERE active = ? ORDER BY name",
    [](const sqlite::RowView& row) {
      return row.Get<std::string>(0);
    },
    true);
```

A decoder returns `sqlite::Result<T>`. `RowView` is valid only during the decoder call, so copy or decode every required value before returning.

Statements that do not return rows use `ExecuteAsync`:

```cpp
auto changed = co_await database.ExecuteAsync(
    "UPDATE users SET active = ? WHERE name = ?",
    false,
    std::string{"Ada"}
);
```

Each raw call accepts exactly one SQL statement. Do not construct SQL by concatenating untrusted values.

## Transactions

`TransactionAsync` queues the complete transaction as one database operation. Its callback runs synchronously on the database worker and uses the transaction's synchronous API:

```cpp
auto result = co_await database.TransactionAsync(
    [](sqlite::Transaction& transaction) -> sqlite::Result<void> {
      User grace{.name = "Grace"};
      auto first = transaction.Insert(users, grace);
      if (!first) {
        return first.Error();
      }

      User duplicate{.name = "Grace"};
      auto second = transaction.Insert(users, duplicate);
      if (!second) {
        return second.Error();
      }

      return {};
    }
);
```

The duplicate unique value makes this example return a constraint error and rolls back both inserts. A transaction commits only when the callback returns success. Returned errors, exceptions, cancellation, or an unexpected SQLite transaction state cause rollback.

The callback cannot be a coroutine, must not suspend, and must not retain the `sqlite::Transaction` reference after it returns. Nested transactions are unsupported.

## Schema migrations

Increment the schema version when a released database structure changes, and provide an explicit migration for every adjacent version:

```cpp
const sqlite::Migrations migrations{
    sqlite::Migration{1, 2, [](sqlite::MigrationContext& migration) -> sqlite::Result<void> {
        auto column = migration.Execute("ALTER TABLE users ADD COLUMN active INTEGER NOT NULL DEFAULT 1");
        if (!column) {
            return column.Error();
        }

        auto index = migration.Execute("CREATE INDEX users_active_name ON users(active, name)");
        if (!index) {
            return index.Error();
        }

        return {};
    }},
};

auto opened = co_await sqlite::Database::OpenAsync(
    database_file, schema, migrations, {.create_parent_directories = true}
);
```

Fresh databases are created directly at the latest schema version; historical migrations are not replayed. Existing databases run only the required transitions in ascending order. The complete upgrade is atomic, and `PRAGMA user_version` advances only after each successful transition.

Migration callbacks are synchronous and already run inside the migration transaction. They may execute parameterized statements and query old rows through `sqlite::MigrationContext`, but they cannot suspend or return a `Task`.

The library never infers destructive changes or repairs a mismatched schema automatically. Test every migration path against representative copies of production databases, including rollback on failure and attempts to open a database created by a newer application version.

## Value mapping and custom codecs

Built-in codecs support signed integers, floating-point values, `bool`, UTF-8 `std::string`, `Bytes`, `std::optional<T>`, and explicitly opted-in enums. `std::optional<T>` maps to SQL `NULL`; decoding `NULL` into a required member returns an error.

Opt an enum into integer storage by specializing its codec:

```cpp
enum class UserState : std::int32_t {
  Active,
  Disabled,
};

template <>
struct sqlite::ValueCodec<UserState>
    : sqlite::EnumValueCodec<UserState> {};
```

Application value types can specialize `sqlite::ValueCodec<T>` with a storage affinity and deterministic `Encode` and `Decode` functions. Codec failures use `sqlite::Result` and never require access to `sqlite3*` or `sqlite3_stmt*`.

## Open options

`sqlite::OpenOptions` makes storage policy explicit:

| Option | Default | Behavior |
| --- | --- | --- |
| `mode` | `ReadWriteCreate` | Selects read-only, read-write, or read-write-create access. |
| `journal_mode` | `Wal` | Selects write-ahead logging or the traditional delete journal. |
| `busy_timeout` | 5 seconds | Limits how long SQLite waits for a locked database. |
| `create_parent_directories` | `false` | Creates missing parent directories before a writable open. |

The database location must be a local HuxerUI `File`. `FileReference` is intentionally not accepted because externally granted files may provide temporary access or may not support long-lived SQLite locking.

WAL is a good default for most application databases, but it creates `-wal` and `-shm` sidecar files and requires a filesystem that supports SQLite locking. Use `JournalMode::Delete` when the storage environment or operational policy requires a rollback journal.

## Concurrency, tasks, and lifetime

`sqlite::Database` is a copyable handle. All copies share one SQLite connection, one serialized worker sequence, and one close state. Operations are queued in call order, and blocking SQLite work never runs on the owning UI thread.

When an operation is awaited from a HuxerUI task, its continuation resumes on that task's owning execution context. A UI-affine task may therefore update captured `State` directly after `co_await`; it does not need `TaskScope::Post()`.

Launch application database flows from `UseTaskScope()` so cancellation follows the mounted composition lifetime. Cancellation skips queued work or requests interruption of running work. An interrupted transaction is rolled back before later queued operations continue, and retired UI work is not resumed.

Calling `CloseAsync()` through any handle copy closes the shared connection after earlier queued work completes. Repeated close calls succeed. Later database operations return `sqlite::ErrorCode::Closed`.

## Errors

Expected failures are returned as `sqlite::Result<T>`. Test the result before reading its value:

```cpp
auto result = co_await database.FindAsync(users, user_id);
if (!result) {
  const sqlite::Error& error = result.Error();
  ShowDatabaseError(error.Message());
  co_return;
}
```

`sqlite::Error` provides a stable `ErrorCode`, a human-readable message, operation context, and optional SQLite primary and extended result codes. Stable categories include constraint, storage, busy, locked, cancelled, migration, schema mismatch, unsupported, decode, not found, closed, and transaction failures.

Invalid table, column, index, query, option, or migration declarations may throw `std::invalid_argument` at the declaration boundary. Routine database failures do not require `try`/`catch` at every call site.

## Backup and sensitive data

The library does not define an application backup policy or expose native SQLite handles. In WAL mode, copying only the main database file while the connection is open can produce an incomplete backup because committed data may still be present in the `-wal` file.

Coordinate backups at the application level. Close the shared database before a file-level copy, or use a separately designed SQLite-consistent backup workflow that respects the target platform's sandbox and lifecycle rules. Account for the main database and any journal sidecars when integrating with native backup systems.

HuxerUI SQLite does not provide automatic encryption or SQLCipher integration. Store databases in the platform application-data directory, exclude sensitive files from platform or cloud backups when required, avoid placing secrets in diagnostic messages, and apply the operating system's data-protection facilities appropriate to your threat model.

Parameterized typed and raw operations protect value binding, but they do not replace authorization, data-retention, or encryption policy.

## Bundled SQLite configuration

The repository currently pins SQLite **3.53.4**. The native library is compiled with:

```text
SQLITE_DEFAULT_FOREIGN_KEYS=1
SQLITE_DQS=0
SQLITE_OMIT_DEPRECATED
SQLITE_OMIT_LOAD_EXTENSION
SQLITE_THREADSAFE=1
```

Foreign-key enforcement and extended result codes are also enabled when a database connection is opened. Query the exact runtime version, source identifier, and complete compile-option list without opening a database:

```cpp
const sqlite::Diagnostics diagnostics = sqlite::GetDiagnostics();
```

## Preview application

[`examples/preview`](examples/preview) is a public-API-only HuxerUI application demonstrating persistent application-data storage, fresh schema creation, a real version migration, typed CRUD, generated keys, partial updates, selection, count and existence queries, parameterized raw SQL, transaction rollback, explicit errors, and close/reopen persistence.

With a compatible HuxerUI SDK installed, run it from the preview directory on a supported host platform. For example, on macOS:

```sh
cd examples/preview
huxerui run macos --profile debug
```

## Development

The repository tests value conversion, schema and SQL construction, the native SQLite bridge, and the public asynchronous API. The integration test opens an isolated temporary database through a HuxerUI `TaskScope` and exercises typed inserts, batch inserts, updates, lookups, selections, raw queries, transaction rollback, and close behavior. Configure a native build against a compatible installed HuxerUI SDK, then run CTest:

```sh
cmake -S . -B build -DHUXERUI_HOME=/path/to/HuxerUI
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

HuxerUI SQLite is available under the [MIT License](LICENSE). The bundled SQLite amalgamation is in the public domain; its provenance and upstream references are recorded in [`third_party/sqlite/README.md`](third_party/sqlite/README.md).
