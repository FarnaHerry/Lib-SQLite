#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace huxerui;
namespace sqlite = huxerui::sqlite;

namespace {

struct User {
  std::int64_t id = 0;
  std::string name;
  bool active = true;
};

const sqlite::Table<User> users{
    "users",
    sqlite::Column<&User::id>{"id", sqlite::PrimaryKey{}, sqlite::AutoIncrement{}},
    sqlite::Column<&User::name>{"name", sqlite::Unique{}},
    sqlite::Column<&User::active>{"active", sqlite::Default{true}},
};

const sqlite::Schema schema{1, users};

struct Harness {
  std::mutex mutex;
  bool finished = false;
  std::string failure;

  void Finish(std::string message = {}) {
    {
      std::lock_guard lock(mutex);
      finished = true;
      failure = std::move(message);
    }
  }
};

Harness* harness = nullptr;
std::string database_path;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string{message});
  }
}

template <class T> T Require(sqlite::Result<T> result, std::string_view operation) {
  if (!result) {
    throw std::runtime_error(
        std::string{operation} + ": " + result.Error().Message()
    );
  }
  return std::move(*result);
}

void Require(sqlite::Result<void> result, std::string_view operation) {
  if (!result) {
    throw std::runtime_error(
        std::string{operation} + ": " + result.Error().Message()
    );
  }
}

Task<void> RunDatabaseTest() {
  try {
    sqlite::Database database = Require(
        co_await sqlite::Database::OpenAsync(File{database_path}, schema),
        "open temporary database"
    );

    const sqlite::InsertResult ada_insert = Require(
        co_await database.InsertAsync(users, User{.name = "Ada"}),
        "insert Ada"
    );
    Check(ada_insert.generated_primary_key.has_value(), "insert did not return Ada's primary key");

    const std::int64_t ada_id = *ada_insert.generated_primary_key;
    Require(
        co_await database.InsertManyAsync(
            users,
            std::vector<User>{
                User{.name = "Grace"},
                User{.name = "Linus", .active = false},
            }
        ),
        "insert users"
    );

    Require(
        co_await database.UpdateFieldsAsync(
            users, ada_id, sqlite::Set(users.Column<&User::active>(), false)
        ),
        "update Ada"
    );

    const std::optional<User> ada = Require(
        co_await database.FindAsync(users, ada_id),
        "find Ada"
    );
    Check(ada && ada->name == "Ada" && !ada->active, "updated Ada row did not round-trip");

    const std::int64_t inactive_ada = Require(
        co_await database.Select(users)
            .Where(users.Column<&User::active>() == false)
            .Where(users.Column<&User::name>() == std::string{"Ada"})
            .CountAsync(),
        "count inactive Ada rows"
    );
    Check(inactive_ada == 1, "successive Where calls did not combine with AND");

    const auto names = Require(
        co_await database.QueryAsync<std::string>(
            "SELECT name FROM users WHERE active = ? ORDER BY name",
            [](const sqlite::RowView& row) { return row.Get<std::string>(0); },
            false
        ),
        "query inactive names"
    );
    Check(names.size() == 2, "raw query returned an unexpected row count");
    Check(names[0] == "Ada" && names[1] == "Linus", "raw query returned unexpected names");

    const sqlite::Result<void> rolled_back = co_await database.TransactionAsync(
        [](sqlite::Transaction& transaction) -> sqlite::Result<void> {
          auto inserted = transaction.Insert(users, User{.name = "Rolled back"});
          if (!inserted) {
            return inserted.Error();
          }
          return sqlite::Error{
              sqlite::ErrorCode::Transaction,
              "request rollback",
              "test transaction rollback",
          };
        }
    );
    Check(!rolled_back, "transaction rollback request unexpectedly succeeded");

    const bool rollback_visible = Require(
        co_await database.Select(users)
            .Where(users.Column<&User::name>() == std::string{"Rolled back"})
            .ExistsAsync(),
        "check transaction rollback"
    );
    Check(!rollback_visible, "rolled-back row remained visible");

    // Invalid UTF-8 in a text column (written by another tool, by an older version of an
    // application, or by hand) is a property of that value, not of the query: it must not fail
    // rows or columns that are readable, and the raw bytes must stay reachable.
    Require(
        co_await database.ExecuteAsync("CREATE TABLE legacy_notes (body TEXT NOT NULL)"),
        "create legacy notes"
    );
    Require(
        co_await database.ExecuteAsync(
            "INSERT INTO legacy_notes (body) VALUES (CAST(x'FF41' AS TEXT))"
        ),
        "insert legacy note"
    );

    const auto unread_bad_column = Require(
        co_await database.QueryAsync<std::int64_t>(
            "SELECT 1, body FROM legacy_notes",
            [](const sqlite::RowView& row) { return row.Get<std::int64_t>(0); }
        ),
        "query a row whose other column holds invalid text"
    );
    Check(
        unread_bad_column.size() == 1 && unread_bad_column[0] == 1,
        "a text column the decoder never read failed the whole query"
    );

    const auto bad_as_string = co_await database.QueryAsync<std::string>(
        "SELECT body FROM legacy_notes",
        [](const sqlite::RowView& row) { return row.Get<std::string>(0); }
    );
    Check(
        !bad_as_string && bad_as_string.Error().Code() == sqlite::ErrorCode::Decode,
        "decoding invalid text did not report ErrorCode::Decode"
    );

    const auto bad_as_bytes = Require(
        co_await database.QueryAsync<Bytes>(
            "SELECT body FROM legacy_notes",
            [](const sqlite::RowView& row) { return row.Get<Bytes>(0); }
        ),
        "read invalid text as bytes"
    );
    Check(
        bad_as_bytes.size() == 1 && bad_as_bytes[0].size() == 2 &&
            bad_as_bytes[0][0] == std::byte{0xFF} && bad_as_bytes[0][1] == std::byte{0x41},
        "raw bytes of an invalid text value did not round-trip"
    );

    Require(co_await database.CloseAsync(), "close database");
    auto closed_query = co_await database.Select(users).CountAsync();
    Check(
        !closed_query && closed_query.Error().Code() == sqlite::ErrorCode::Closed,
        "query after close did not report ErrorCode::Closed"
    );

    harness->Finish();
  } catch (const std::exception& exception) {
    harness->Finish(exception.what());
  } catch (...) {
    harness->Finish("unknown integration test failure");
  }
}

View TestApplication() {
  const TaskScope tasks = UseTaskScope();
  Lifecycle([tasks] {
    tasks.Launch([]() -> Task<void> { co_await RunDatabaseTest(); });
  });
  return Spacer();
}

class TestPlatform final : public PlatformAdapter {
public:
  TestPlatform()
      : PlatformAdapter([this](std::function<void()> callback) {
          {
            std::lock_guard lock(mutex_);
            callbacks_.push_back(std::move(callback));
          }
          changed_.notify_all();
        }) {}

  void RequestFrameAt(double) override {
    {
      std::lock_guard lock(mutex_);
      frame_requested_ = true;
    }
    changed_.notify_all();
  }

  double Now() const noexcept override {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()
    ).count();
  }

  FontMetrics Metrics(const Font&) override {
    return {};
  }

  TextRunMetrics MeasureRun(
      std::string_view, const TextStyle&, const TextShapingOptions&
  ) override {
    return {};
  }

  TextLayoutMetrics MeasureText(
      std::string_view, const TextStyle&, float, const TextLayoutOptions&
  ) override {
    return {};
  }

  bool RunNext(Runtime& runtime, std::chrono::steady_clock::time_point deadline) {
    std::function<void()> callback;
    bool build_frame = false;
    {
      std::unique_lock lock(mutex_);
      changed_.wait_until(lock, deadline, [this] {
        return !callbacks_.empty() || frame_requested_;
      });
      if (!callbacks_.empty()) {
        callback = std::move(callbacks_.front());
        callbacks_.pop_front();
      } else if (frame_requested_) {
        frame_requested_ = false;
        build_frame = true;
      } else {
        return false;
      }
    }
    if (callback) {
      callback();
    } else if (build_frame) {
      static_cast<void>(runtime.BuildFrame());
    }
    return true;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::function<void()>> callbacks_;
  bool frame_requested_ = false;
};

} // namespace

int main() {
  const auto unique_value = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto random_value = std::random_device{}();
  database_path = (
      std::filesystem::temp_directory_path() /
      ("huxerui-sqlite-integration-" + std::to_string(unique_value) + "-" +
       std::to_string(random_value) + ".sqlite3")
  ).string();

  Harness test_harness;
  harness = &test_harness;

  {
    const Application application{TestApplication};
    TestPlatform platform;
    Runtime runtime{application, platform};
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    static_cast<void>(runtime.BuildFrame());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (true) {
      {
        std::lock_guard lock(test_harness.mutex);
        if (test_harness.finished) {
          break;
        }
      }
      if (!platform.RunNext(runtime, deadline)) {
        test_harness.Finish("timed out waiting for the database integration test");
        break;
      }
    }
  }

  std::string failure;
  {
    std::lock_guard lock(test_harness.mutex);
    failure = test_harness.failure;
  }

  std::error_code cleanup_error;
  for (const std::string_view suffix : {"", "-wal", "-shm", "-journal"}) {
    std::filesystem::remove(database_path + std::string{suffix}, cleanup_error);
    if (cleanup_error && failure.empty()) {
      failure = "remove temporary database: " + cleanup_error.message();
    }
    cleanup_error.clear();
  }

  if (!failure.empty()) {
    throw std::runtime_error(failure);
  }
}
