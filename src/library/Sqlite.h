#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>

struct sqlite3;
struct sqlite3_stmt;

namespace lattice::library
{

/** One SQLite connection (docs/05-m4-ui.md, 4e: the library index). Built with
    SQLITE_THREADSAFE=2: a connection may be used from any thread, but by one thread at a time,
    so its owner guards it. Errors come back as juce::Result; nothing throws. */
class SqliteDb
{
public:
    SqliteDb() = default;
    ~SqliteDb();
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    /** Opens (creating) a database file, or an in-memory one for an empty path. */
    juce::Result open(const juce::String& path);
    void close();
    bool isOpen() const noexcept { return db != nullptr; }

    /** Runs one or more statements that return nothing. */
    juce::Result exec(const char* sql);

    std::int64_t lastInsertId() const noexcept;
    int changes() const noexcept;
    juce::String lastError() const;

    /** A prepared statement. Bind (1-based), step(), read the columns (0-based), reset() to run again. */
    class Statement
    {
    public:
        Statement() = default;
        Statement(SqliteDb& db, const char* sql);
        ~Statement();
        Statement(Statement&& other) noexcept;
        Statement& operator=(Statement&& other) noexcept;
        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        bool isValid() const noexcept { return stmt != nullptr; }

        Statement& bind(int index, const juce::String& value);
        Statement& bind(int index, std::int64_t value);
        Statement& bind(int index, int value) { return bind(index, static_cast<std::int64_t>(value)); }
        Statement& bind(int index, double value);
        Statement& bindNull(int index);

        /** True while there is a row to read; false when done (or on an error: see ok()). */
        bool step();
        /** Runs a statement that returns no rows. */
        juce::Result run();
        bool ok() const noexcept { return lastCode == 0 || lastCode == 100 || lastCode == 101; }
        void reset();

        juce::String text(int column) const;
        std::int64_t integer(int column) const;
        double real(int column) const;
        bool isNull(int column) const;

    private:
        sqlite3_stmt* stmt = nullptr;
        sqlite3* owner = nullptr;
        int lastCode = 0;
    };

    /** BEGIN ... COMMIT, or ROLLBACK if not committed. */
    class Transaction
    {
    public:
        explicit Transaction(SqliteDb& db);
        ~Transaction();
        juce::Result commit();

    private:
        SqliteDb& database;
        bool done = false;
    };

private:
    sqlite3* db = nullptr;
};

} // namespace lattice::library
