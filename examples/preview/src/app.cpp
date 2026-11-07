#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace huxerui;

namespace {

struct Contact {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> note;
  bool active = true;
};

const sqlite::Table<Contact> contacts{
    "contacts",
    sqlite::Column<&Contact::id>{"id", sqlite::PrimaryKey(), sqlite::AutoIncrement()},
    sqlite::Column<&Contact::name>{"name", sqlite::Unique()},
    sqlite::Column<&Contact::note>{"note"},
    sqlite::Column<&Contact::active>{"active", sqlite::Default(true)},
    sqlite::Index<&Contact::active, &Contact::name>{"contacts_active_name"},
};

const sqlite::Schema schema{2, contacts};

const sqlite::Migrations migrations{
    sqlite::Migration{1, 2, [](sqlite::MigrationContext& migration) -> sqlite::Result<void> {
      auto column = migration.Execute("ALTER TABLE contacts ADD COLUMN active INTEGER NOT NULL DEFAULT 1");
      if (!column) {
        return column.Error();
      }
      auto index = migration.Execute("CREATE INDEX contacts_active_name ON contacts(active, name)");
      if (!index) {
        return index.Error();
      }
      return {};
    }},
};

struct Snapshot {
  std::vector<Contact> contacts;
  std::int64_t total = 0;
  bool ada_exists = false;
  std::int64_t raw_active_count = 0;
};

struct PreviewModel {
  std::optional<sqlite::Database> database;
  std::vector<Contact> contacts;
  std::int64_t total = 0;
  bool ada_exists = false;
  std::int64_t raw_active_count = 0;
  std::optional<std::int64_t> last_generated_key;
  bool busy = true;
  std::string status = "Opening the persistent database...";
  std::string error;
};

std::string ErrorCodeName(sqlite::ErrorCode code) {
  switch (code) {
    case sqlite::ErrorCode::Unknown:
      return "Unknown";
    case sqlite::ErrorCode::Sql:
      return "Sql";
    case sqlite::ErrorCode::Constraint:
      return "Constraint";
    case sqlite::ErrorCode::Storage:
      return "Storage";
    case sqlite::ErrorCode::Busy:
      return "Busy";
    case sqlite::ErrorCode::Locked:
      return "Locked";
    case sqlite::ErrorCode::Cancelled:
      return "Cancelled";
    case sqlite::ErrorCode::Migration:
      return "Migration";
    case sqlite::ErrorCode::SchemaMismatch:
      return "SchemaMismatch";
    case sqlite::ErrorCode::Unsupported:
      return "Unsupported";
    case sqlite::ErrorCode::Decode:
      return "Decode";
    case sqlite::ErrorCode::NotFound:
      return "NotFound";
    case sqlite::ErrorCode::Closed:
      return "Closed";
    case sqlite::ErrorCode::Transaction:
      return "Transaction";
  }
  return "Unknown";
}

std::string DescribeError(const sqlite::Error& error) {
  std::string description = ErrorCodeName(error.Code()) + ": " + error.Message();
  if (!error.Operation().empty()) {
    description += " (" + error.Operation() + ")";
  }
  if (error.SqliteExtendedCode()) {
    description += " [SQLite " + std::to_string(*error.SqliteExtendedCode()) + "]";
  }
  return description;
}

void BeginOperation(const State<PreviewModel>& state, std::string status) {
  state.Update([status = std::move(status)](PreviewModel& model) mutable {
    model.busy = true;
    model.status = std::move(status);
    model.error.clear();
  });
}

void FailOperation(const State<PreviewModel>& state, const sqlite::Error& error) {
  const std::string description = DescribeError(error);
  state.Update([description](PreviewModel& model) {
    model.busy = false;
    model.status = "The database operation failed.";
    model.error = description;
  });
}

void FailOperation(const State<PreviewModel>& state, std::string message) {
  state.Update([message = std::move(message)](PreviewModel& model) mutable {
    model.busy = false;
    model.status = "The database operation failed.";
    model.error = std::move(message);
  });
}

Task<sqlite::Result<Snapshot>> LoadSnapshotAsync(sqlite::Database database) {
  auto rows = co_await database.Select(contacts)
                  .OrderBy(contacts.Column<&Contact::active>(), sqlite::SortDirection::Descending)
                  .OrderBy(contacts.Column<&Contact::name>())
                  .AllAsync();
  if (!rows) {
    co_return rows.Error();
  }

  auto total = co_await database.Select(contacts).CountAsync();
  if (!total) {
    co_return total.Error();
  }

  auto ada_exists = co_await database.Select(contacts)
                        .Where(contacts.Column<&Contact::name>() == std::string{"Ada"})
                        .ExistsAsync();
  if (!ada_exists) {
    co_return ada_exists.Error();
  }

  auto active_count = co_await database.QueryAsync<std::int64_t>(
      "SELECT COUNT(*) FROM contacts WHERE active = ?",
      [](const sqlite::RowView& row) { return row.Get<std::int64_t>(0); }, true
  );
  if (!active_count) {
    co_return active_count.Error();
  }
  if (active_count->size() != 1) {
    co_return sqlite::Error{
        sqlite::ErrorCode::Sql,
        "The raw aggregate query returned an unexpected number of rows",
        "load preview statistics",
    };
  }

  co_return Snapshot{
      .contacts = std::move(*rows),
      .total = *total,
      .ada_exists = *ada_exists,
      .raw_active_count = active_count->front(),
  };
}

void ApplySnapshot(const State<PreviewModel>& state, Snapshot snapshot, std::string status) {
  state.Update([snapshot = std::move(snapshot), status = std::move(status)](PreviewModel& model) mutable {
    model.contacts = std::move(snapshot.contacts);
    model.total = snapshot.total;
    model.ada_exists = snapshot.ada_exists;
    model.raw_active_count = snapshot.raw_active_count;
    model.busy = false;
    model.status = std::move(status);
    model.error.clear();
  });
}

sqlite::OpenOptions PersistentOptions() {
  return {
      .journal_mode = sqlite::JournalMode::Wal,
      .busy_timeout = std::chrono::seconds{5},
      .create_parent_directories = true,
  };
}

Task<void> OpenDatabaseAsync(File file, const State<PreviewModel>& state) {
  auto opened = co_await sqlite::Database::OpenAsync(file, schema, migrations, PersistentOptions());
  if (!opened) {
    FailOperation(state, opened.Error());
    co_return;
  }

  sqlite::Database database = *opened;
  state.Update([database](PreviewModel& model) { model.database = database; });
  auto snapshot = co_await LoadSnapshotAsync(database);
  if (!snapshot) {
    FailOperation(state, snapshot.Error());
    co_return;
  }
  ApplySnapshot(state, std::move(*snapshot), "Persistent database opened at schema version 2.");
}

Task<void> RefreshAsync(sqlite::Database database, const State<PreviewModel>& state, std::string status) {
  BeginOperation(state, "Refreshing typed queries and raw statistics...");
  auto snapshot = co_await LoadSnapshotAsync(database);
  if (!snapshot) {
    FailOperation(state, snapshot.Error());
    co_return;
  }
  ApplySnapshot(state, std::move(*snapshot), std::move(status));
}

Task<void> AddContactAsync(sqlite::Database database, std::string name, std::string note,
                           const State<TextEditingValue>& name_input, const State<TextEditingValue>& note_input,
                           const State<PreviewModel>& state) {
  BeginOperation(state, "Inserting a typed record...");
  Contact contact{
      .name = std::move(name),
      .note = note.empty() ? std::nullopt : std::optional<std::string>{std::move(note)},
  };
  auto inserted = co_await database.InsertAsync(contacts, contact);
  if (!inserted) {
    FailOperation(state, inserted.Error());
    co_return;
  }

  state.Update([key = inserted->generated_primary_key](PreviewModel& model) { model.last_generated_key = key; });
  name_input = TextEditingValue::FromText({});
  note_input = TextEditingValue::FromText({});

  std::string status = "Inserted a contact";
  if (inserted->generated_primary_key) {
    status += " with generated key " + std::to_string(*inserted->generated_primary_key);
  }
  status += ".";
  co_await RefreshAsync(database, state, std::move(status));
}

Task<void> SetContactActiveAsync(sqlite::Database database, std::int64_t id, bool active,
                                 const State<PreviewModel>& state) {
  BeginOperation(state, "Updating one field with sqlite::Set()...");
  auto updated = co_await database.UpdateFieldsAsync(
      contacts, id, sqlite::Set(contacts.Column<&Contact::active>(), active)
  );
  if (!updated) {
    FailOperation(state, updated.Error());
    co_return;
  }
  co_await RefreshAsync(database, state, "Partial update completed.");
}

Task<void> DeleteContactAsync(sqlite::Database database, std::int64_t id, const State<PreviewModel>& state) {
  BeginOperation(state, "Deleting a typed record by primary key...");
  auto deleted = co_await database.DeleteAsync(contacts, id);
  if (!deleted) {
    FailOperation(state, deleted.Error());
    co_return;
  }
  co_await RefreshAsync(database, state, "Deleted one contact by primary key.");
}

Task<void> ReopenDatabaseAsync(File file, sqlite::Database database, const State<PreviewModel>& state) {
  BeginOperation(state, "Closing and reopening the persistent database...");
  auto closed = co_await database.CloseAsync();
  if (!closed) {
    FailOperation(state, closed.Error());
    co_return;
  }
  state.Update([](PreviewModel& model) { model.database.reset(); });

  auto reopened = co_await sqlite::Database::OpenAsync(file, schema, migrations, PersistentOptions());
  if (!reopened) {
    FailOperation(state, reopened.Error());
    co_return;
  }
  sqlite::Database reopened_database = *reopened;
  state.Update([reopened_database](PreviewModel& model) { model.database = reopened_database; });
  auto snapshot = co_await LoadSnapshotAsync(reopened_database);
  if (!snapshot) {
    FailOperation(state, snapshot.Error());
    co_return;
  }
  ApplySnapshot(state, std::move(*snapshot), "Closed and reopened the same file; persisted rows were reloaded.");
}

Task<void> VerifyRollbackAsync(sqlite::Database database, const State<PreviewModel>& state) {
  BeginOperation(state, "Running a transaction that must roll back...");
  auto before = co_await database.Select(contacts).CountAsync();
  if (!before) {
    FailOperation(state, before.Error());
    co_return;
  }

  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string probe_name = "Rollback probe " + std::to_string(nonce);
  auto transaction = co_await database.TransactionAsync([probe_name](sqlite::Transaction& value) -> sqlite::Result<void> {
    Contact first{.name = probe_name, .note = std::string{"This row must be rolled back"}};
    auto inserted = value.Insert(contacts, first);
    if (!inserted) {
      return inserted.Error();
    }
    Contact duplicate{.name = probe_name};
    auto rejected = value.Insert(contacts, duplicate);
    if (!rejected) {
      return rejected.Error();
    }
    return {};
  });

  auto after = co_await database.Select(contacts).CountAsync();
  if (!after) {
    FailOperation(state, after.Error());
    co_return;
  }
  if (transaction || transaction.Error().Code() != sqlite::ErrorCode::Constraint || *after != *before) {
    FailOperation(state, "The rollback probe did not preserve the original row count.");
    co_return;
  }

  auto snapshot = co_await LoadSnapshotAsync(database);
  if (!snapshot) {
    FailOperation(state, snapshot.Error());
    co_return;
  }
  ApplySnapshot(
      state, std::move(*snapshot),
      "Rollback verified: the duplicate-name constraint rejected the transaction and no probe row remained."
  );
}

Task<void> VerifyMigrationAsync(ApplicationHandle application, const State<PreviewModel>& state) {
  BeginOperation(state, "Creating a version 1 database and migrating it to version 2...");
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  File probe_file = application.Directories().temporary_directory.Child(
      "huxerui-sqlite-migration-" + std::to_string(nonce) + ".sqlite3"
  );
  sqlite::OpenOptions options{.journal_mode = sqlite::JournalMode::Delete};

  auto legacy_opened = co_await sqlite::Database::OpenAsync(probe_file, options);
  if (!legacy_opened) {
    FailOperation(state, legacy_opened.Error());
    co_return;
  }
  sqlite::Database legacy = *legacy_opened;

  auto table = co_await legacy.ExecuteAsync(
      "CREATE TABLE contacts ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "name TEXT NOT NULL UNIQUE, "
      "note TEXT)"
  );
  if (!table) {
    FailOperation(state, table.Error());
    co_return;
  }
  auto seeded = co_await legacy.ExecuteAsync(
      "INSERT INTO contacts(name, note) VALUES(?, ?)", std::string{"Legacy Ada"},
      std::optional<std::string>{"Created before the active column"}
  );
  if (!seeded) {
    FailOperation(state, seeded.Error());
    co_return;
  }
  auto versioned = co_await legacy.ExecuteAsync("PRAGMA user_version = 1");
  if (!versioned) {
    FailOperation(state, versioned.Error());
    co_return;
  }
  auto closed = co_await legacy.CloseAsync();
  if (!closed) {
    FailOperation(state, closed.Error());
    co_return;
  }

  auto migrated_opened = co_await sqlite::Database::OpenAsync(probe_file, schema, migrations, options);
  if (!migrated_opened) {
    FailOperation(state, migrated_opened.Error());
    co_return;
  }
  sqlite::Database migrated = *migrated_opened;
  auto legacy_contact = co_await migrated.Select(contacts)
                            .Where(contacts.Column<&Contact::name>() == std::string{"Legacy Ada"})
                            .FirstAsync();
  if (!legacy_contact) {
    FailOperation(state, legacy_contact.Error());
    co_return;
  }
  if (!legacy_contact->active) {
    FailOperation(state, "The migrated active column did not receive its declared default.");
    co_return;
  }
  auto migrated_closed = co_await migrated.CloseAsync();
  if (!migrated_closed) {
    FailOperation(state, migrated_closed.Error());
    co_return;
  }
  const bool removed = co_await probe_file.DeleteAsync();
  if (!removed) {
    FailOperation(state, "Migration passed, but its temporary database could not be removed.");
    co_return;
  }

  state.Update([](PreviewModel& model) {
    model.busy = false;
    model.status = "Migration verified: a version 1 row survived the atomic 1 to 2 upgrade.";
    model.error.clear();
  });
}

View ContactList(const std::vector<Contact>& values, const TaskScope& tasks, const State<PreviewModel>& state,
                 bool enabled) {
  if (values.empty()) {
    return Text("No contacts yet. Add one above, then close and reopen the database to verify persistence.");
  }

  return Column {
    ForEach(values, [tasks, state, enabled](const Contact& contact) {
      return Column {
        Text::Format(TextRole::Label, "{} · key {}", contact.name, contact.id),
        Text(contact.note.value_or("No note")),
        Flow {
          Switch("Active", contact.active)
              .OnChanged([tasks, state, id = contact.id](bool active) {
                if (state->busy || !state->database) {
                  return;
                }
                sqlite::Database database = *state->database;
                tasks.Launch([database, state, id, active]() -> Task<void> {
                  co_await SetContactActiveAsync(database, id, active, state);
                });
              })
              .With(Enabled{enabled}),
          Button("Delete")
              .OnClick([tasks, state, id = contact.id] {
                if (state->busy || !state->database) {
                  return;
                }
                sqlite::Database database = *state->database;
                tasks.Launch([database, state, id]() -> Task<void> {
                  co_await DeleteContactAsync(database, id, state);
                });
              })
              .With(Enabled{enabled}),
        }.With(Spacing{8.0F}),
        Divider(),
      }.With(Spacing{8.0F}).Key(contact.id);
    }),
  }.With(Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch});
}

} // namespace

View App() {
  auto application = UseApplication();
  auto tasks = UseTaskScope();
  auto state = UseState(PreviewModel{});
  auto name_input = UseState(TextEditingValue::FromText({}));
  auto note_input = UseState(TextEditingValue::FromText({}));
  const File database_file =
      application.Directories().data_directory.Child("sqlite-preview").Child("contacts.sqlite3");

  Lifecycle([tasks, state, database_file] {
    tasks.Launch([database_file, state]() -> Task<void> { co_await OpenDatabaseAsync(database_file, state); });
  });

  const bool ready = state->database.has_value() && !state->busy;
  const bool can_add = ready && !name_input->text.empty();
  const auto add_contact = [tasks, state, name_input, note_input] {
    if (state->busy || !state->database || name_input->text.empty()) {
      return;
    }
    sqlite::Database database = *state->database;
    const std::string name = name_input->text;
    const std::string note = note_input->text;
    tasks.Launch([database, name, note, name_input, note_input, state]() -> Task<void> {
      co_await AddContactAsync(database, name, note, name_input, note_input, state);
    });
  };

  const sqlite::Diagnostics diagnostics = sqlite::GetDiagnostics();
  View content = Column {
    Text("HuxerUI SQLite", TextRole::Title),
    Text("A persistent installed-API example with typed records, migrations, raw SQL, and rollback."),
    Text::Format("SQLite {} · Schema {} · {} compile options", diagnostics.sqlite_version, schema.Version(),
                 diagnostics.compile_options.size()),
    Divider(),
    Text("Persistent database", TextRole::Label),
    Text(database_file.Path()),
    Text(state->busy ? "Working..." : state->status),
    Text(state->error.empty() ? "Last error: none" : "Last error: " + state->error),
    Divider(),
    Text("Add a contact", TextRole::Label),
    TextField(name_input)
        .Label("Unique name")
        .Placeholder("Ada")
        .MaxLength(80)
        .OnChanged([name_input](const TextEditingValue& value) { name_input = value; })
        .OnSubmitted(add_contact)
        .With(Enabled{ready}),
    TextField(note_input)
        .Label("Optional note")
        .Placeholder("Stored as SQL NULL when empty")
        .MaxLength(160)
        .OnChanged([note_input](const TextEditingValue& value) { note_input = value; })
        .With(Enabled{ready}),
    Button("Insert and return generated key").OnClick(add_contact).With(Enabled{can_add}),
    Text(state->last_generated_key
             ? "Last generated key: " + std::to_string(*state->last_generated_key)
             : "Last generated key: none"),
    Divider(),
    Text("Query results", TextRole::Label),
    Flow {
      Text::Format("Typed CountAsync: {}", state->total),
      Text::Format("Typed ExistsAsync(\"Ada\"): {}", state->ada_exists ? "true" : "false"),
      Text::Format("Parameterized raw active count: {}", state->raw_active_count),
    }.With(Spacing{16.0F}),
    ContactList(state->contacts, tasks, state, ready),
    Text("Verification actions", TextRole::Label),
    Flow {
      Button("Refresh queries")
          .OnClick([tasks, state] {
            if (state->busy || !state->database) {
              return;
            }
            sqlite::Database database = *state->database;
            tasks.Launch([database, state]() -> Task<void> {
              co_await RefreshAsync(database, state, "Typed and raw queries refreshed.");
            });
          })
          .With(Enabled{ready}),
      Button("Close and reopen")
          .OnClick([tasks, state, database_file] {
            if (state->busy || !state->database) {
              return;
            }
            sqlite::Database database = *state->database;
            tasks.Launch([database_file, database, state]() -> Task<void> {
              co_await ReopenDatabaseAsync(database_file, database, state);
            });
          })
          .With(Enabled{ready}),
      Button("Verify transaction rollback")
          .OnClick([tasks, state] {
            if (state->busy || !state->database) {
              return;
            }
            sqlite::Database database = *state->database;
            tasks.Launch([database, state]() -> Task<void> { co_await VerifyRollbackAsync(database, state); });
          })
          .With(Enabled{ready}),
      Button("Verify migration 1 to 2")
          .OnClick([tasks, state, application] {
            if (state->busy) {
              return;
            }
            tasks.Launch([application, state]() -> Task<void> { co_await VerifyMigrationAsync(application, state); });
          })
          .With(Enabled{!state->busy}),
    }.With(Spacing{8.0F}),
  }.With(Padding{24.0F}, Spacing{12.0F}, CrossAlign{CrossAxisAlignment::Stretch});

  return MaterialTheme(
      ScrollView(content).With(Frame{.max_width = 880.0F}, ScrollBar{}, SafeAreaPadding{})
  );
}

const Application application{
    App,
    {
        .window = {
            .title = "SQLite Preview",
            .initial_size = {920.0F, 760.0F},
        },
    },
};
