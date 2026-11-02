#include "library/Sqlite.h"

#include <sqlite3.h>

#include <utility>

namespace lattice::library
{

SqliteDb::~SqliteDb()
{
    close();
}

juce::Result SqliteDb::open(const juce::String& path)
{
    close();
    const auto name = path.isEmpty() ? juce::String(":memory:") : path;
    const int code = sqlite3_open_v2(name.toRawUTF8(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                     nullptr);
    if (code != SQLITE_OK)
    {
        const auto error = db != nullptr ? juce::String::fromUTF8(sqlite3_errmsg(db)) : juce::String("out of memory");
        close();
        return juce::Result::fail("Couldn't open the library index (" + error + ")");
    }
    sqlite3_busy_timeout(db, 2000);
    return juce::Result::ok();
}

void SqliteDb::close()
{
    if (db != nullptr)
        sqlite3_close_v2(db);
    db = nullptr;
}

juce::Result SqliteDb::exec(const char* sql)
{
    if (db == nullptr)
        return juce::Result::fail("The library index isn't open");
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        const auto error = juce::String::fromUTF8(message != nullptr ? message : "unknown error");
        sqlite3_free(message);
        return juce::Result::fail(error);
    }
    return juce::Result::ok();
}

std::int64_t SqliteDb::lastInsertId() const noexcept
{
    return db != nullptr ? sqlite3_last_insert_rowid(db) : 0;
}

int SqliteDb::changes() const noexcept
{
    return db != nullptr ? sqlite3_changes(db) : 0;
}

juce::String SqliteDb::lastError() const
{
    return db != nullptr ? juce::String::fromUTF8(sqlite3_errmsg(db)) : juce::String("not open");
}

// --- Statement ------------------------------------------------------------------------------------------

SqliteDb::Statement::Statement(SqliteDb& database, const char* sql) : owner(database.db)
{
    if (owner == nullptr)
        return;
    lastCode = sqlite3_prepare_v2(owner, sql, -1, &stmt, nullptr);
    jassert(lastCode == SQLITE_OK); // a mistake in the SQL
    if (lastCode != SQLITE_OK)
        stmt = nullptr;
}

SqliteDb::Statement::~Statement()
{
    if (stmt != nullptr)
        sqlite3_finalize(stmt);
}

SqliteDb::Statement::Statement(Statement&& other) noexcept
    : stmt(std::exchange(other.stmt, nullptr)), owner(other.owner), lastCode(other.lastCode)
{
}

SqliteDb::Statement& SqliteDb::Statement::operator=(Statement&& other) noexcept
{
    if (this != &other)
    {
        if (stmt != nullptr)
            sqlite3_finalize(stmt);
        stmt = std::exchange(other.stmt, nullptr);
        owner = other.owner;
        lastCode = other.lastCode;
    }
    return *this;
}

SqliteDb::Statement& SqliteDb::Statement::bind(int index, const juce::String& value)
{
    if (stmt != nullptr)
        sqlite3_bind_text(stmt, index, value.toRawUTF8(), -1, SQLITE_TRANSIENT);
    return *this;
}

SqliteDb::Statement& SqliteDb::Statement::bind(int index, std::int64_t value)
{
    if (stmt != nullptr)
        sqlite3_bind_int64(stmt, index, value);
    return *this;
}

SqliteDb::Statement& SqliteDb::Statement::bind(int index, double value)
{
    if (stmt != nullptr)
        sqlite3_bind_double(stmt, index, value);
    return *this;
}

SqliteDb::Statement& SqliteDb::Statement::bindNull(int index)
{
    if (stmt != nullptr)
        sqlite3_bind_null(stmt, index);
    return *this;
}

bool SqliteDb::Statement::step()
{
    if (stmt == nullptr)
        return false;
    lastCode = sqlite3_step(stmt);
    return lastCode == SQLITE_ROW;
}

juce::Result SqliteDb::Statement::run()
{
    if (stmt == nullptr)
        return juce::Result::fail("Invalid statement");
    lastCode = sqlite3_step(stmt);
    const bool good = lastCode == SQLITE_DONE || lastCode == SQLITE_ROW;
    const auto error = good ? juce::String() : juce::String::fromUTF8(sqlite3_errmsg(owner));
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return good ? juce::Result::ok() : juce::Result::fail(error);
}

void SqliteDb::Statement::reset()
{
    if (stmt == nullptr)
        return;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    lastCode = 0;
}

juce::String SqliteDb::Statement::text(int column) const
{
    if (stmt == nullptr)
        return {};
    const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    return t != nullptr ? juce::String::fromUTF8(t, sqlite3_column_bytes(stmt, column)) : juce::String();
}

std::int64_t SqliteDb::Statement::integer(int column) const
{
    return stmt != nullptr ? sqlite3_column_int64(stmt, column) : 0;
}

double SqliteDb::Statement::real(int column) const
{
    return stmt != nullptr ? sqlite3_column_double(stmt, column) : 0.0;
}

bool SqliteDb::Statement::isNull(int column) const
{
    return stmt == nullptr || sqlite3_column_type(stmt, column) == SQLITE_NULL;
}

// --- Transaction ----------------------------------------------------------------------------------------

SqliteDb::Transaction::Transaction(SqliteDb& db) : database(db)
{
    database.exec("BEGIN");
}

SqliteDb::Transaction::~Transaction()
{
    if (!done)
        database.exec("ROLLBACK");
}

juce::Result SqliteDb::Transaction::commit()
{
    done = true;
    return database.exec("COMMIT");
}

} // namespace lattice::library
