#include "GB_Sqlite.h"

#include "GB_ReadWriteLock.h"

#include <sqlite3.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <list>
#include <limits>
#include <mutex>
#include <set>
#include <new>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace
{
    const int GB_SqliteOk = SQLITE_OK;
    const std::size_t GB_SqliteMaxInitialRowReserveCount = 4096;


    static std::string ToString(int value)
    {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }

    static std::string ToString(long long value)
    {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }

    static bool IsParameterPrefix(char ch)
    {
        return ch == ':' || ch == '@' || ch == '$' || ch == '?';
    }

    static bool HasParameterPrefix(const std::string& nameUtf8)
    {
        return !nameUtf8.empty() && IsParameterPrefix(nameUtf8[0]);
    }

    static std::string StripParameterPrefix(const std::string& nameUtf8)
    {
        if (!nameUtf8.empty() && IsParameterPrefix(nameUtf8[0]))
        {
            return nameUtf8.substr(1);
        }

        return nameUtf8;
    }

    static const char* ToSynchronousPragmaValue(GB_SqliteSynchronousMode mode)
    {
        switch (mode)
        {
        case GB_SqliteSynchronousMode::Off:
            return "OFF";
        case GB_SqliteSynchronousMode::Normal:
            return "NORMAL";
        case GB_SqliteSynchronousMode::Full:
            return "FULL";
        case GB_SqliteSynchronousMode::Extra:
            return "EXTRA";
        default:
            return "NORMAL";
        }
    }

    static const char* ToTransactionBeginSql(GB_SqliteTransactionMode mode)
    {
        switch (mode)
        {
        case GB_SqliteTransactionMode::Deferred:
            return "BEGIN DEFERRED TRANSACTION";
        case GB_SqliteTransactionMode::Immediate:
            return "BEGIN IMMEDIATE TRANSACTION";
        case GB_SqliteTransactionMode::Exclusive:
            return "BEGIN EXCLUSIVE TRANSACTION";
        default:
            return "BEGIN IMMEDIATE TRANSACTION";
        }
    }

    static std::size_t GetDefaultReadConnectionCount()
    {
        const unsigned int hardwareThreadCount = std::thread::hardware_concurrency();
        if (hardwareThreadCount == 0)
        {
            return 4;
        }

        const std::size_t count = static_cast<std::size_t>(hardwareThreadCount);
        return std::min<std::size_t>(std::max<std::size_t>(count, 2), 8);
    }

    static std::size_t NormalizeReadConnectionCount(std::size_t readConnectionCount)
    {
        if (readConnectionCount == 0)
        {
            return GetDefaultReadConnectionCount();
        }

        return std::min<std::size_t>(std::max<std::size_t>(readConnectionCount, 1), 64);
    }

    static char ToLowerAscii(char ch)
    {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    }

    static std::string ToLowerAsciiString(const std::string& textUtf8)
    {
        std::string result = textUtf8;
        for (std::size_t index = 0; index < result.size(); index++)
        {
            result[index] = ToLowerAscii(result[index]);
        }
        return result;
    }

    static bool StartsWithAsciiNoCase(const std::string& textUtf8, const std::string& prefixUtf8)
    {
        if (textUtf8.size() < prefixUtf8.size())
        {
            return false;
        }

        for (std::size_t index = 0; index < prefixUtf8.size(); index++)
        {
            if (ToLowerAscii(textUtf8[index]) != ToLowerAscii(prefixUtf8[index]))
            {
                return false;
            }
        }

        return true;
    }

    static bool IsInMemoryDatabasePath(const std::string& databasePathUtf8)
    {
        if (databasePathUtf8 == ":memory:")
        {
            return true;
        }

        const std::string lowerPathUtf8 = ToLowerAsciiString(databasePathUtf8);
        if (StartsWithAsciiNoCase(lowerPathUtf8, "file::memory:"))
        {
            return true;
        }

        return StartsWithAsciiNoCase(lowerPathUtf8, "file:") && lowerPathUtf8.find("mode=memory") != std::string::npos;
    }

    static bool ShouldUseSingleConnectionForReads(const std::string& databasePathUtf8)
    {
        return IsInMemoryDatabasePath(databasePathUtf8);
    }

    static bool CanUseWalForDatabasePath(const std::string& databasePathUtf8)
    {
        return !IsInMemoryDatabasePath(databasePathUtf8);
    }

    static int BuildOpenFlags(GB_SqliteOpenMode openMode, bool readConnection, const GB_SqliteOptions& options)
    {
        int flags = options.useFullMutex ? SQLITE_OPEN_FULLMUTEX : SQLITE_OPEN_NOMUTEX;
#ifdef SQLITE_OPEN_EXRESCODE
        flags |= SQLITE_OPEN_EXRESCODE;
#endif
        if (options.enableUri)
        {
            flags |= SQLITE_OPEN_URI;
        }

        if (readConnection)
        {
            // 内部读连接在可写模式下使用 READWRITE + query_only，避免 WAL 场景下 -wal/-shm 尚未创建导致只读连接打开失败。
            flags |= openMode == GB_SqliteOpenMode::ReadOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
            return flags;
        }

        switch (openMode)
        {
        case GB_SqliteOpenMode::ReadOnly:
            flags |= SQLITE_OPEN_READONLY;
            break;
        case GB_SqliteOpenMode::ReadWrite:
            flags |= SQLITE_OPEN_READWRITE;
            break;
        case GB_SqliteOpenMode::ReadWriteCreate:
            flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
            break;
        default:
            flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
            break;
        }

        return flags;
    }

    static std::string SafeString(const char* text)
    {
        return text == nullptr ? std::string() : std::string(text);
    }


    static bool IsSqlWhitespace(char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
    }

    static bool SkipSqlComment(const char*& currentPtr)
    {
        if (currentPtr == nullptr || currentPtr[0] == '\0')
        {
            return false;
        }

        if (currentPtr[0] == '-' && currentPtr[1] == '-')
        {
            currentPtr += 2;
            while (*currentPtr != '\0' && *currentPtr != '\r' && *currentPtr != '\n')
            {
                currentPtr++;
            }
            return true;
        }

        if (currentPtr[0] == '/' && currentPtr[1] == '*')
        {
            currentPtr += 2;
            while (currentPtr[0] != '\0')
            {
                if (currentPtr[0] == '*' && currentPtr[1] == '/')
                {
                    currentPtr += 2;
                    return true;
                }
                currentPtr++;
            }
            return true;
        }

        return false;
    }

    static bool HasTrailingSqlStatement(const char* tailPtr)
    {
        const char* currentPtr = tailPtr;
        while (currentPtr != nullptr && *currentPtr != '\0')
        {
            while (*currentPtr != '\0' && (IsSqlWhitespace(*currentPtr) || *currentPtr == ';'))
            {
                currentPtr++;
            }

            if (SkipSqlComment(currentPtr))
            {
                continue;
            }

            return *currentPtr != '\0';
        }

        return false;
    }

    static GB_SqliteError MakeError(sqlite3* database, int code, const std::string& sqlUtf8, const std::string& fallbackMessageUtf8 = std::string())
    {
        GB_SqliteError error;
        error.code = code;
        error.extendedCode = database == nullptr ? code : sqlite3_extended_errcode(database);
        error.sqlUtf8 = sqlUtf8;

        if (database != nullptr)
        {
            error.messageUtf8 = SafeString(sqlite3_errmsg(database));
        }

        if (error.messageUtf8.empty())
        {
            error.messageUtf8 = fallbackMessageUtf8;
        }

        if (error.messageUtf8.empty())
        {
            error.messageUtf8 = "SQLite error code: " + ToString(code);
        }

        return error;
    }

    static GB_SqliteError MakeUserError(const std::string& messageUtf8, const std::string& sqlUtf8 = std::string())
    {
        GB_SqliteError error;
        error.code = SQLITE_MISUSE;
        error.extendedCode = SQLITE_MISUSE;
        error.messageUtf8 = messageUtf8;
        error.sqlUtf8 = sqlUtf8;
        return error;
    }

    static GB_SqliteError MakeExceptionError(const std::string& operationUtf8, const std::string& sqlUtf8 = std::string())
    {
        return MakeUserError("SQLite operation failed because an unexpected C++ exception was thrown: " + operationUtf8, sqlUtf8);
    }

    static GB_SqliteError MakeBadAllocError(const std::string& operationUtf8, const std::string& sqlUtf8 = std::string())
    {
        return MakeUserError("SQLite operation failed because memory allocation failed: " + operationUtf8, sqlUtf8);
    }

    static GB_SqliteError MakeOkError()
    {
        return GB_SqliteError();
    }

    static std::string ReadFirstSqlKeywordLower(const std::string& sqlUtf8)
    {
        const char* currentPtr = sqlUtf8.c_str();
        while (currentPtr != nullptr && *currentPtr != '\0')
        {
            while (*currentPtr != '\0' && (IsSqlWhitespace(*currentPtr) || *currentPtr == ';'))
            {
                currentPtr++;
            }

            if (SkipSqlComment(currentPtr))
            {
                continue;
            }

            break;
        }

        std::string keyword;
        while (*currentPtr != '\0')
        {
            const char ch = *currentPtr;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'))
            {
                break;
            }

            keyword.push_back(ToLowerAscii(ch));
            currentPtr++;
        }

        return keyword;
    }

    static bool IsTransactionControlSql(const std::string& sqlUtf8)
    {
        const std::string keyword = ReadFirstSqlKeywordLower(sqlUtf8);
        return keyword == "begin" || keyword == "commit" || keyword == "rollback" || keyword == "savepoint" || keyword == "release" || keyword == "end";
    }

    static bool IsConnectionStateSql(const std::string& sqlUtf8)
    {
        const std::string keyword = ReadFirstSqlKeywordLower(sqlUtf8);
        return IsTransactionControlSql(sqlUtf8) || keyword == "attach" || keyword == "detach";
    }

    static bool IsProbablySchemaChangingSql(const std::string& sqlUtf8)
    {
        const std::string keyword = ReadFirstSqlKeywordLower(sqlUtf8);
        return keyword == "create" || keyword == "alter" || keyword == "drop" || keyword == "vacuum" || keyword == "reindex" || keyword == "analyze" || keyword == "pragma";
    }

    static std::size_t GetResultReserveCount(std::size_t maxRowCount)
    {
        if (maxRowCount == 0)
        {
            return 0;
        }

        return std::min<std::size_t>(maxRowCount, GB_SqliteMaxInitialRowReserveCount);
    }

    static bool IsReadonlyOpenMode(GB_SqliteOpenMode openMode)
    {
        return openMode == GB_SqliteOpenMode::ReadOnly;
    }


    static bool ContainsNullCharacter(const std::string& textUtf8)
    {
        return textUtf8.find('\0') != std::string::npos;
    }

    static std::string NormalizeSchemaName(const std::string& schemaNameUtf8)
    {
        return schemaNameUtf8.empty() ? std::string("main") : schemaNameUtf8;
    }

    static std::string QuoteSqlIdentifierUnchecked(const std::string& identifierUtf8)
    {
        std::string result;
        result.reserve(identifierUtf8.size() + 2);
        result.push_back('"');
        for (std::size_t index = 0; index < identifierUtf8.size(); index++)
        {
            const char ch = identifierUtf8[index];
            if (ch == '"')
            {
                result.push_back('"');
                result.push_back('"');
            }
            else
            {
                result.push_back(ch);
            }
        }
        result.push_back('"');
        return result;
    }

    static std::string QuoteSqlStringLiteralUnchecked(const std::string& textUtf8)
    {
        std::string result;
        result.reserve(textUtf8.size() + 2);
        result.push_back('\'');
        for (std::size_t index = 0; index < textUtf8.size(); index++)
        {
            const char ch = textUtf8[index];
            if (ch == '\'')
            {
                result.push_back('\'');
                result.push_back('\'');
            }
            else
            {
                result.push_back(ch);
            }
        }
        result.push_back('\'');
        return result;
    }

    static bool IsMainOrTempSchemaName(const std::string& schemaNameUtf8)
    {
        const std::string lowerSchemaNameUtf8 = ToLowerAsciiString(schemaNameUtf8);
        return lowerSchemaNameUtf8 == "main" || lowerSchemaNameUtf8 == "temp";
    }

    static void SkipSqlQuotedContent(const char*& currentPtr)
    {
        if (currentPtr == nullptr || *currentPtr == '\0')
        {
            return;
        }

        const char quoteChar = *currentPtr;
        if (quoteChar == '\'' || quoteChar == '"' || quoteChar == '`')
        {
            currentPtr++;
            while (*currentPtr != '\0')
            {
                if (*currentPtr == quoteChar)
                {
                    if (currentPtr[1] == quoteChar)
                    {
                        currentPtr += 2;
                        continue;
                    }

                    currentPtr++;
                    return;
                }

                currentPtr++;
            }
            return;
        }

        if (quoteChar == '[')
        {
            currentPtr++;
            while (*currentPtr != '\0')
            {
                if (*currentPtr == ']')
                {
                    currentPtr++;
                    return;
                }

                currentPtr++;
            }
        }
    }

    static std::string ReadSqlKeywordLowerAtCurrent(const char*& currentPtr)
    {
        std::string keyword;
        while (currentPtr != nullptr && *currentPtr != '\0')
        {
            const char ch = *currentPtr;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'))
            {
                break;
            }

            keyword.push_back(ToLowerAscii(ch));
            currentPtr++;
        }

        return keyword;
    }

    static void SkipToNextSqlStatement(const char*& currentPtr)
    {
        while (currentPtr != nullptr && *currentPtr != '\0')
        {
            if (SkipSqlComment(currentPtr))
            {
                continue;
            }

            if (*currentPtr == '\'' || *currentPtr == '"' || *currentPtr == '`' || *currentPtr == '[')
            {
                SkipSqlQuotedContent(currentPtr);
                continue;
            }

            if (*currentPtr == ';')
            {
                currentPtr++;
                return;
            }

            currentPtr++;
        }
    }

    static bool SqlBatchContainsAttachOrDetachSql(const std::string& sqlUtf8)
    {
        const char* currentPtr = sqlUtf8.c_str();
        while (currentPtr != nullptr && *currentPtr != '\0')
        {
            while (*currentPtr != '\0' && (IsSqlWhitespace(*currentPtr) || *currentPtr == ';'))
            {
                currentPtr++;
            }

            if (SkipSqlComment(currentPtr))
            {
                continue;
            }

            if (*currentPtr == '\0')
            {
                break;
            }

            const char* keywordPtr = currentPtr;
            const std::string keyword = ReadSqlKeywordLowerAtCurrent(keywordPtr);
            if (keyword == "attach" || keyword == "detach")
            {
                return true;
            }

            SkipToNextSqlStatement(currentPtr);
        }

        return false;
    }

    static bool ValidateSqlText(const std::string& sqlUtf8, GB_SqliteError& outError)
    {
        if (ContainsNullCharacter(sqlUtf8))
        {
            outError = MakeUserError("SQLite SQL text contains null character.", sqlUtf8);
            return false;
        }

        outError.Clear();
        return true;
    }

    static bool ValidateSqlIdentifier(const std::string& identifierUtf8, const std::string& identifierDescriptionUtf8, GB_SqliteError& outError, const std::string& sqlUtf8 = std::string())
    {
        if (identifierUtf8.empty())
        {
            outError = MakeUserError("SQLite " + identifierDescriptionUtf8 + " is empty.", sqlUtf8);
            return false;
        }

        if (ContainsNullCharacter(identifierUtf8))
        {
            outError = MakeUserError("SQLite " + identifierDescriptionUtf8 + " contains null character.", sqlUtf8);
            return false;
        }

        outError.Clear();
        return true;
    }

    static bool BuildQualifiedTableName(const std::string& schemaNameUtf8, const std::string& tableNameUtf8, std::string& outQualifiedNameUtf8, GB_SqliteError& outError)
    {
        const std::string normalizedSchemaNameUtf8 = NormalizeSchemaName(schemaNameUtf8);
        if (!ValidateSqlIdentifier(normalizedSchemaNameUtf8, "schema name", outError) || !ValidateSqlIdentifier(tableNameUtf8, "table name", outError))
        {
            return false;
        }

        outQualifiedNameUtf8 = QuoteSqlIdentifierUnchecked(normalizedSchemaNameUtf8) + "." + QuoteSqlIdentifierUnchecked(tableNameUtf8);
        outError.Clear();
        return true;
    }

    static bool BuildQualifiedSchemaObjectName(const std::string& schemaNameUtf8, const std::string& objectNameUtf8, std::string& outQualifiedNameUtf8, GB_SqliteError& outError)
    {
        const std::string normalizedSchemaNameUtf8 = NormalizeSchemaName(schemaNameUtf8);
        if (!ValidateSqlIdentifier(normalizedSchemaNameUtf8, "schema name", outError) || !ValidateSqlIdentifier(objectNameUtf8, "schema object name", outError))
        {
            return false;
        }

        outQualifiedNameUtf8 = QuoteSqlIdentifierUnchecked(normalizedSchemaNameUtf8) + "." + QuoteSqlIdentifierUnchecked(objectNameUtf8);
        outError.Clear();
        return true;
    }

    static bool VariantToInt(const GB_Variant& value, int& outValue)
    {
        bool ok = false;
        outValue = value.ToInt(&ok);
        return ok;
    }

    static bool VariantToInt64(const GB_Variant& value, long long& outValue)
    {
        bool ok = false;
        outValue = value.ToInt64(&ok);
        return ok;
    }

    static bool VariantToString(const GB_Variant& value, std::string& outValue)
    {
        if (value.IsEmpty() || value.Type() == GB_VariantType::Empty)
        {
            outValue.clear();
            return false;
        }

        bool ok = false;
        outValue = value.ToString(&ok);
        return ok;
    }


    static int PrepareSqlStatement(sqlite3* database, const std::string& sqlUtf8, bool persistent, sqlite3_stmt** outStatement, const char** outTail)
    {
        const int sqlByteCount = static_cast<int>(sqlUtf8.size() + 1);
#if defined(SQLITE_VERSION_NUMBER) && SQLITE_VERSION_NUMBER >= 3020000 && defined(SQLITE_PREPARE_PERSISTENT)
        const unsigned int prepareFlags = persistent ? SQLITE_PREPARE_PERSISTENT : 0;
        return sqlite3_prepare_v3(database, sqlUtf8.c_str(), sqlByteCount, prepareFlags, outStatement, outTail);
#else
        (void)persistent;
        return sqlite3_prepare_v2(database, sqlUtf8.c_str(), sqlByteCount, outStatement, outTail);
#endif
    }

    struct GB_SqliteStatementCacheEntry
    {
        std::string sqlUtf8;
        sqlite3_stmt* statement = nullptr;
    };

    class GB_SqliteStatementCache
    {
    public:
        GB_SqliteStatementCache()
        {
        }

        ~GB_SqliteStatementCache()
        {
            Clear();
        }

        GB_SqliteStatementCache(const GB_SqliteStatementCache& other) = delete;
        GB_SqliteStatementCache& operator=(const GB_SqliteStatementCache& other) = delete;

        void SetCapacity(std::size_t capacity)
        {
            capacity_ = capacity;
            TrimToCapacity();
        }

        sqlite3_stmt* Find(const std::string& sqlUtf8)
        {
            auto iter = map_.find(sqlUtf8);
            if (iter == map_.end())
            {
                misses_++;
                return nullptr;
            }

            entries_.splice(entries_.begin(), entries_, iter->second);
            hits_++;
            return iter->second->statement;
        }

        bool Put(const std::string& sqlUtf8, sqlite3_stmt* statement)
        {
            if (capacity_ == 0 || statement == nullptr)
            {
                return false;
            }

            auto iter = map_.find(sqlUtf8);
            if (iter != map_.end())
            {
                if (iter->second->statement != nullptr && iter->second->statement != statement)
                {
                    sqlite3_finalize(iter->second->statement);
                }
                iter->second->statement = statement;
                entries_.splice(entries_.begin(), entries_, iter->second);
                return true;
            }

            try
            {
                GB_SqliteStatementCacheEntry entry;
                entry.sqlUtf8 = sqlUtf8;
                entry.statement = statement;
                entries_.push_front(std::move(entry));

                const auto insertResult = map_.insert(std::make_pair(entries_.front().sqlUtf8, entries_.begin()));
                if (!insertResult.second)
                {
                    entries_.pop_front();
                    return false;
                }

                TrimToCapacity();
                return true;
            }
            catch (...)
            {
                if (!entries_.empty() && entries_.front().statement == statement)
                {
                    entries_.front().statement = nullptr;
                    entries_.pop_front();
                }
                return false;
            }
        }

        void Remove(const std::string& sqlUtf8)
        {
            auto iter = map_.find(sqlUtf8);
            if (iter == map_.end())
            {
                return;
            }

            if (iter->second->statement != nullptr)
            {
                sqlite3_finalize(iter->second->statement);
            }
            entries_.erase(iter->second);
            map_.erase(iter);
        }

        void Clear()
        {
            for (auto iter = entries_.begin(); iter != entries_.end(); iter++)
            {
                if (iter->statement != nullptr)
                {
                    sqlite3_finalize(iter->statement);
                    iter->statement = nullptr;
                }
            }
            entries_.clear();
            map_.clear();
        }

        std::size_t Size() const
        {
            return entries_.size();
        }

        std::uint64_t Hits() const
        {
            return hits_;
        }

        std::uint64_t Misses() const
        {
            return misses_;
        }

        std::uint64_t Evictions() const
        {
            return evictions_;
        }

    private:
        void TrimToCapacity()
        {
            while (entries_.size() > capacity_)
            {
                auto iter = entries_.end();
                iter--;
                if (iter->statement != nullptr)
                {
                    sqlite3_finalize(iter->statement);
                }
                map_.erase(iter->sqlUtf8);
                entries_.erase(iter);
                evictions_++;
            }
        }

    private:
        std::size_t capacity_ = 0;
        std::list<GB_SqliteStatementCacheEntry> entries_;
        std::unordered_map<std::string, std::list<GB_SqliteStatementCacheEntry>::iterator> map_;
        std::uint64_t hits_ = 0;
        std::uint64_t misses_ = 0;
        std::uint64_t evictions_ = 0;
    };

    struct GB_SqliteConnection
    {
        sqlite3* database = nullptr;
        GB_SqliteStatementCache statementCache;
        bool readOnly = false;

        ~GB_SqliteConnection()
        {
            Close();
        }

        GB_SqliteConnection(const GB_SqliteConnection& other) = delete;
        GB_SqliteConnection& operator=(const GB_SqliteConnection& other) = delete;

        GB_SqliteConnection()
        {
        }

        void Close()
        {
            statementCache.Clear();
            if (database != nullptr)
            {
                sqlite3_stmt* remainingStatement = nullptr;
                while ((remainingStatement = sqlite3_next_stmt(database, nullptr)) != nullptr)
                {
                    sqlite3_finalize(remainingStatement);
                }
                sqlite3_close(database);
                database = nullptr;
            }
        }
    };

    class GB_SqliteStatementHandle
    {
    public:
        GB_SqliteStatementHandle()
        {
        }

        ~GB_SqliteStatementHandle()
        {
            (void)ResetAndClear();
        }

        GB_SqliteStatementHandle(const GB_SqliteStatementHandle& other) = delete;
        GB_SqliteStatementHandle& operator=(const GB_SqliteStatementHandle& other) = delete;

        sqlite3_stmt* Get() const
        {
            return statement_;
        }

        bool IsValid() const
        {
            return statement_ != nullptr;
        }

        bool IsCached() const
        {
            return cached_;
        }

        void Attach(sqlite3_stmt* statement, bool cached)
        {
            (void)ResetAndClear();
            statement_ = statement;
            cached_ = cached;
        }

        int ResetAndClear()
        {
            if (statement_ == nullptr)
            {
                return SQLITE_OK;
            }

            const int resetCode = sqlite3_reset(statement_);
            sqlite3_clear_bindings(statement_);

            int finalizeCode = SQLITE_OK;
            if (!cached_)
            {
                finalizeCode = sqlite3_finalize(statement_);
            }

            statement_ = nullptr;
            cached_ = false;

            return resetCode != SQLITE_OK ? resetCode : finalizeCode;
        }

    private:
        sqlite3_stmt* statement_ = nullptr;
        bool cached_ = false;
    };

    class GB_SqliteOptionalWriteLockGuard
    {
    public:
        GB_SqliteOptionalWriteLockGuard(GB_ReadWriteLock& lock, bool shouldLock) : lock_(shouldLock ? &lock : nullptr)
        {
            if (lock_ != nullptr)
            {
                lock_->Lock();
            }
        }

        ~GB_SqliteOptionalWriteLockGuard()
        {
            if (lock_ != nullptr)
            {
                lock_->Unlock();
            }
        }

        GB_SqliteOptionalWriteLockGuard(const GB_SqliteOptionalWriteLockGuard& other) = delete;
        GB_SqliteOptionalWriteLockGuard& operator=(const GB_SqliteOptionalWriteLockGuard& other) = delete;

    private:
        GB_ReadWriteLock* lock_ = nullptr;
    };

    static bool ResetStatementHandle(GB_SqliteConnection& connection, const std::string& sqlUtf8, GB_SqliteStatementHandle& statementHandle, GB_SqliteError& outError)
    {
        const bool wasCached = statementHandle.IsCached();
        const int resetCode = statementHandle.ResetAndClear();
        if (resetCode != SQLITE_OK)
        {
            if (wasCached && resetCode == SQLITE_SCHEMA)
            {
                connection.statementCache.Remove(sqlUtf8);
            }
            outError = MakeError(connection.database, resetCode, sqlUtf8);
            return false;
        }

        outError.Clear();
        return true;
    }

    static void DropCachedStatementAfterError(GB_SqliteConnection& connection, const std::string& sqlUtf8, GB_SqliteStatementHandle& statementHandle, int code)
    {
        if (code != SQLITE_SCHEMA)
        {
            return;
        }

        if (statementHandle.IsCached())
        {
            (void)statementHandle.ResetAndClear();
            connection.statementCache.Remove(sqlUtf8);
        }
    }

    static bool DirectExec(sqlite3* database, const std::string& sqlUtf8, GB_SqliteError& outError)
    {
        if (database == nullptr)
        {
            outError = MakeUserError("SQLite connection is not open.", sqlUtf8);
            return false;
        }

        if (!ValidateSqlText(sqlUtf8, outError))
        {
            return false;
        }

        char* errorMessage = nullptr;
        const int code = sqlite3_exec(database, sqlUtf8.c_str(), nullptr, nullptr, &errorMessage);
        if (code == SQLITE_OK)
        {
            if (errorMessage != nullptr)
            {
                sqlite3_free(errorMessage);
            }
            outError.Clear();
            return true;
        }

        outError = MakeError(database, code, sqlUtf8, SafeString(errorMessage));
        if (errorMessage != nullptr)
        {
            sqlite3_free(errorMessage);
        }
        return false;
    }

    static bool ExecuteWalPragma(sqlite3* database, GB_SqliteError& outError)
    {
        const std::string sqlUtf8 = "PRAGMA journal_mode=WAL";
        if (database == nullptr)
        {
            outError = MakeUserError("SQLite connection is not open.", sqlUtf8);
            return false;
        }

        sqlite3_stmt* statement = nullptr;
        const int prepareCode = PrepareSqlStatement(database, sqlUtf8, false, &statement, nullptr);
        if (prepareCode != SQLITE_OK)
        {
            outError = MakeError(database, prepareCode, sqlUtf8);
            if (statement != nullptr)
            {
                sqlite3_finalize(statement);
            }
            return false;
        }

        if (statement == nullptr)
        {
            outError = MakeUserError("SQLite WAL pragma produced an empty statement.", sqlUtf8);
            return false;
        }

        const int stepCode = sqlite3_step(statement);
        std::string journalModeUtf8;
        if (stepCode == SQLITE_ROW)
        {
            const unsigned char* textPtr = sqlite3_column_text(statement, 0);
            const int byteCount = sqlite3_column_bytes(statement, 0);
            if (textPtr != nullptr && byteCount > 0)
            {
                journalModeUtf8.assign(reinterpret_cast<const char*>(textPtr), static_cast<std::size_t>(byteCount));
            }
        }
        else if (stepCode != SQLITE_DONE)
        {
            outError = MakeError(database, stepCode, sqlUtf8);
            sqlite3_finalize(statement);
            return false;
        }

        const int finalizeCode = sqlite3_finalize(statement);
        if (finalizeCode != SQLITE_OK)
        {
            outError = MakeError(database, finalizeCode, sqlUtf8);
            return false;
        }

        if (ToLowerAsciiString(journalModeUtf8) != "wal")
        {
            outError = MakeUserError("Failed to switch SQLite journal_mode to WAL. Current journal_mode is: " + journalModeUtf8, sqlUtf8);
            return false;
        }

        outError.Clear();
        return true;
    }

    static bool CanCacheStatement(const GB_SqliteOptions& options, const std::string& sqlUtf8)
    {
        if (!options.enableStatementCache || options.maxCachedStatementsPerConnection == 0)
        {
            return false;
        }

        return options.maxCachedStatementSqlByteCount == 0 || sqlUtf8.size() <= options.maxCachedStatementSqlByteCount;
    }

    static bool PrepareStatement(GB_SqliteConnection& connection, const GB_SqliteOptions& options, const std::string& sqlUtf8, GB_SqliteStatementHandle& outStatement, GB_SqliteError& outError)
    {
        if (connection.database == nullptr)
        {
            outError = MakeUserError("SQLite connection is not open.", sqlUtf8);
            return false;
        }

        if (!ValidateSqlText(sqlUtf8, outError))
        {
            return false;
        }

        if (sqlUtf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() - 1))
        {
            outError = MakeUserError("SQLite SQL text is too large.", sqlUtf8);
            return false;
        }

        if (IsConnectionStateSql(sqlUtf8))
        {
            outError = MakeUserError("SQLite transaction SQL should use GB_SqliteTransaction. ATTACH/DETACH should use AttachDatabase() or DetachDatabase().", sqlUtf8);
            return false;
        }

        sqlite3_stmt* statement = nullptr;
        bool cached = false;
        const bool cacheAllowed = CanCacheStatement(options, sqlUtf8);
        if (cacheAllowed)
        {
            statement = connection.statementCache.Find(sqlUtf8);
            if (statement != nullptr)
            {
                cached = true;
            }
        }

        if (statement == nullptr)
        {
            const char* tailPtr = nullptr;
            const bool persistent = cacheAllowed;
            const int code = PrepareSqlStatement(connection.database, sqlUtf8, persistent, &statement, &tailPtr);
            if (code != SQLITE_OK)
            {
                outError = MakeError(connection.database, code, sqlUtf8);
                if (statement != nullptr)
                {
                    sqlite3_finalize(statement);
                }
                return false;
            }

            if (statement == nullptr)
            {
                outError = MakeUserError("SQLite SQL statement is empty.", sqlUtf8);
                return false;
            }

            if (HasTrailingSqlStatement(tailPtr))
            {
                sqlite3_finalize(statement);
                outError = MakeUserError("SQLite SQL contains more than one statement. Use ExecuteBatch() for SQL batches.", sqlUtf8);
                return false;
            }

            if (cacheAllowed)
            {
                cached = connection.statementCache.Put(sqlUtf8, statement);
            }
        }

        outStatement.Attach(statement, cached);
        outError.Clear();
        return true;
    }

    static bool BindNull(sqlite3_stmt* statement, int index, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        const int code = sqlite3_bind_null(statement, index);
        if (code == SQLITE_OK)
        {
            return true;
        }

        outError = MakeError(sqlite3_db_handle(statement), code, sqlUtf8);
        return false;
    }

    static bool BindText(sqlite3_stmt* statement, int index, const std::string& valueUtf8, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        if (valueUtf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            outError = MakeUserError("SQLite text parameter is too large.", sqlUtf8);
            return false;
        }

        const char* textPtr = valueUtf8.empty() ? "" : valueUtf8.data();
        const int code = sqlite3_bind_text(statement, index, textPtr, static_cast<int>(valueUtf8.size()), SQLITE_TRANSIENT);
        if (code == SQLITE_OK)
        {
            return true;
        }

        outError = MakeError(sqlite3_db_handle(statement), code, sqlUtf8);
        return false;
    }

    static bool BindBlob(sqlite3_stmt* statement, int index, const GB_ByteBuffer& value, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            outError = MakeUserError("SQLite blob parameter is too large.", sqlUtf8);
            return false;
        }

        const void* dataPtr = value.empty() ? static_cast<const void*>("") : static_cast<const void*>(value.data());
        const int code = sqlite3_bind_blob(statement, index, dataPtr, static_cast<int>(value.size()), SQLITE_TRANSIENT);
        if (code == SQLITE_OK)
        {
            return true;
        }

        outError = MakeError(sqlite3_db_handle(statement), code, sqlUtf8);
        return false;
    }

    static bool BindVariant(sqlite3_stmt* statement, int index, const GB_Variant& value, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        if (value.IsEmpty() || value.Type() == GB_VariantType::Empty)
        {
            return BindNull(statement, index, outError, sqlUtf8);
        }

        int code = SQLITE_OK;
        bool ok = false;
        switch (value.Type())
        {
        case GB_VariantType::Bool:
        {
            const bool boolValue = value.ToBool(&ok);
            if (!ok)
            {
                outError = MakeUserError("Failed to convert GB_Variant to bool.", sqlUtf8);
                return false;
            }
            code = sqlite3_bind_int64(statement, index, boolValue ? 1 : 0);
            break;
        }
        case GB_VariantType::Int32:
        case GB_VariantType::Int64:
        {
            const long long intValue = value.ToInt64(&ok);
            if (!ok)
            {
                outError = MakeUserError("Failed to convert GB_Variant to signed integer.", sqlUtf8);
                return false;
            }
            code = sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(intValue));
            break;
        }
        case GB_VariantType::UInt32:
        case GB_VariantType::UInt64:
        {
            const unsigned long long uintValue = value.ToUInt64(&ok);
            if (!ok)
            {
                outError = MakeUserError("Failed to convert GB_Variant to unsigned integer.", sqlUtf8);
                return false;
            }

            if (uintValue <= static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            {
                code = sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(uintValue));
            }
            else
            {
                std::ostringstream stream;
                stream << uintValue;
                return BindText(statement, index, stream.str(), outError, sqlUtf8);
            }
            break;
        }
        case GB_VariantType::Float:
        case GB_VariantType::Double:
        {
            const double doubleValue = value.ToDouble(&ok);
            if (!ok)
            {
                outError = MakeUserError("Failed to convert GB_Variant to double.", sqlUtf8);
                return false;
            }
            code = sqlite3_bind_double(statement, index, doubleValue);
            break;
        }
        case GB_VariantType::String:
        {
            const std::string* textValue = value.AnyCast<std::string>();
            if (textValue != nullptr)
            {
                return BindText(statement, index, *textValue, outError, sqlUtf8);
            }

            const std::string textUtf8 = value.ToString(&ok);
            if (!ok)
            {
                outError = MakeUserError("Failed to convert GB_Variant to UTF-8 string.", sqlUtf8);
                return false;
            }
            return BindText(statement, index, textUtf8, outError, sqlUtf8);
        }
        case GB_VariantType::Binary:
        {
            const GB_ByteBuffer* bytesValue = value.AnyCast<GB_ByteBuffer>();
            if (bytesValue != nullptr)
            {
                return BindBlob(statement, index, *bytesValue, outError, sqlUtf8);
            }

            const GB_ByteBuffer bytes = value.ToBinary(&ok);
            if (!ok)
            {
                outError = MakeUserError("Failed to convert GB_Variant to binary buffer.", sqlUtf8);
                return false;
            }
            return BindBlob(statement, index, bytes, outError, sqlUtf8);
        }
        case GB_VariantType::Custom:
        {
            GB_ByteBuffer bytes;
            if (!value.Serialize(bytes))
            {
                outError = MakeUserError("Failed to serialize custom GB_Variant parameter.", sqlUtf8);
                return false;
            }
            return BindBlob(statement, index, bytes, outError, sqlUtf8);
        }
        default:
            return BindNull(statement, index, outError, sqlUtf8);
        }

        if (code == SQLITE_OK)
        {
            return true;
        }

        outError = MakeError(sqlite3_db_handle(statement), code, sqlUtf8);
        return false;
    }

    static bool CheckPositionalParameterCount(sqlite3_stmt* statement, std::size_t suppliedParameterCount, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        if (suppliedParameterCount > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            outError = MakeUserError("SQLite positional parameter count is too large.", sqlUtf8);
            return false;
        }

        const int parameterCount = sqlite3_bind_parameter_count(statement);
        if (parameterCount != static_cast<int>(suppliedParameterCount))
        {
            outError = MakeUserError("SQLite positional parameter count mismatch.", sqlUtf8);
            return false;
        }

        outError.Clear();
        return true;
    }

    static bool BindParametersUnchecked(sqlite3_stmt* statement, const GB_SqliteParameterList& parameters, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        const int parameterCount = sqlite3_bind_parameter_count(statement);
        for (int index = 1; index <= parameterCount; index++)
        {
            if (!BindVariant(statement, index, parameters[static_cast<std::size_t>(index - 1)], outError, sqlUtf8))
            {
                return false;
            }
        }

        outError.Clear();
        return true;
    }

    static bool BindParameters(sqlite3_stmt* statement, const GB_SqliteParameterList& parameters, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        if (!CheckPositionalParameterCount(statement, parameters.size(), outError, sqlUtf8))
        {
            return false;
        }

        return BindParametersUnchecked(statement, parameters, outError, sqlUtf8);
    }

    static bool BindNoParameters(sqlite3_stmt* statement, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        return CheckPositionalParameterCount(statement, 0, outError, sqlUtf8);
    }

    static const GB_Variant* FindNamedParameter(const GB_SqliteNamedParameters& parameters, const std::string& sqliteNameUtf8)
    {
        GB_SqliteNamedParameters::const_iterator iter = parameters.find(sqliteNameUtf8);
        if (iter != parameters.end())
        {
            return &iter->second;
        }

        const std::string strippedNameUtf8 = StripParameterPrefix(sqliteNameUtf8);
        iter = parameters.find(strippedNameUtf8);
        if (iter != parameters.end())
        {
            return &iter->second;
        }

        return nullptr;
    }

    static bool IsSameNamedParameter(const std::string& userNameUtf8, const std::string& sqliteNameUtf8)
    {
        if (HasParameterPrefix(userNameUtf8))
        {
            return userNameUtf8 == sqliteNameUtf8;
        }

        return userNameUtf8 == StripParameterPrefix(sqliteNameUtf8);
    }

    struct GB_SqliteNamedParameterBinding
    {
        int index = 0;
        std::string sqliteNameUtf8;
    };

    static bool BuildNamedParameterBindings(sqlite3_stmt* statement, std::vector<GB_SqliteNamedParameterBinding>& outBindings, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        outBindings.clear();
        const int parameterCount = sqlite3_bind_parameter_count(statement);
        try
        {
            outBindings.reserve(static_cast<std::size_t>(std::max(parameterCount, 0)));
        }
        catch (...)
        {
            outError = MakeBadAllocError("build named parameter binding plan", sqlUtf8);
            return false;
        }

        for (int index = 1; index <= parameterCount; index++)
        {
            const char* namePtr = sqlite3_bind_parameter_name(statement, index);
            if (namePtr == nullptr || std::strlen(namePtr) == 0)
            {
                outError = MakeUserError("SQLite statement contains unnamed parameter, but named parameters were supplied.", sqlUtf8);
                return false;
            }

            GB_SqliteNamedParameterBinding binding;
            binding.index = index;
            binding.sqliteNameUtf8 = namePtr;
            outBindings.push_back(std::move(binding));
        }

        outError.Clear();
        return true;
    }

    static bool IsNamedParameterUsed(const std::vector<GB_SqliteNamedParameterBinding>& bindings, const std::string& userNameUtf8)
    {
        for (std::size_t index = 0; index < bindings.size(); index++)
        {
            if (IsSameNamedParameter(userNameUtf8, bindings[index].sqliteNameUtf8))
            {
                return true;
            }
        }

        return false;
    }

    static bool CheckAmbiguousUnprefixedNamedParameters(const std::vector<GB_SqliteNamedParameterBinding>& bindings, const GB_SqliteNamedParameters& parameters, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        for (GB_SqliteNamedParameters::const_iterator iter = parameters.begin(); iter != parameters.end(); iter++)
        {
            if (HasParameterPrefix(iter->first))
            {
                continue;
            }

            int matchedSqlParameterCount = 0;
            std::set<std::string> matchedSqlNames;
            for (std::size_t index = 0; index < bindings.size(); index++)
            {
                if (StripParameterPrefix(bindings[index].sqliteNameUtf8) == iter->first)
                {
                    matchedSqlNames.insert(bindings[index].sqliteNameUtf8);
                }
            }

            matchedSqlParameterCount = static_cast<int>(matchedSqlNames.size());
            if (matchedSqlParameterCount > 1)
            {
                outError = MakeUserError("SQLite unprefixed named parameter is ambiguous, please use exact parameter prefix: " + iter->first, sqlUtf8);
                return false;
            }
        }

        outError.Clear();
        return true;
    }

    static bool CheckUnusedNamedParameters(const std::vector<GB_SqliteNamedParameterBinding>& bindings, const GB_SqliteNamedParameters& parameters, bool rejectUnusedNamedParameters, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        if (!CheckAmbiguousUnprefixedNamedParameters(bindings, parameters, outError, sqlUtf8))
        {
            return false;
        }

        if (!rejectUnusedNamedParameters)
        {
            outError.Clear();
            return true;
        }

        for (GB_SqliteNamedParameters::const_iterator iter = parameters.begin(); iter != parameters.end(); iter++)
        {
            if (!IsNamedParameterUsed(bindings, iter->first))
            {
                outError = MakeUserError("SQLite named parameter was supplied but not used by SQL: " + iter->first, sqlUtf8);
                return false;
            }
        }

        outError.Clear();
        return true;
    }

    static bool BindNamedParametersUnchecked(sqlite3_stmt* statement, const std::vector<GB_SqliteNamedParameterBinding>& bindings, const GB_SqliteNamedParameters& parameters, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        for (std::size_t index = 0; index < bindings.size(); index++)
        {
            const GB_SqliteNamedParameterBinding& binding = bindings[index];
            const GB_Variant* value = FindNamedParameter(parameters, binding.sqliteNameUtf8);
            if (value == nullptr)
            {
                outError = MakeUserError("SQLite named parameter not found: " + binding.sqliteNameUtf8, sqlUtf8);
                return false;
            }

            if (!BindVariant(statement, binding.index, *value, outError, sqlUtf8))
            {
                return false;
            }
        }

        outError.Clear();
        return true;
    }

    static bool BindNamedParameters(sqlite3_stmt* statement, const GB_SqliteNamedParameters& parameters, bool rejectUnusedNamedParameters, GB_SqliteError& outError, const std::string& sqlUtf8)
    {
        std::vector<GB_SqliteNamedParameterBinding> bindings;
        if (!BuildNamedParameterBindings(statement, bindings, outError, sqlUtf8))
        {
            return false;
        }

        if (!CheckUnusedNamedParameters(bindings, parameters, rejectUnusedNamedParameters, outError, sqlUtf8))
        {
            return false;
        }

        return BindNamedParametersUnchecked(statement, bindings, parameters, outError, sqlUtf8);
    }

    static GB_Variant GetColumnValue(sqlite3_stmt* statement, int columnIndex)
    {
        const int sqliteValueType = sqlite3_column_type(statement, columnIndex);
        switch (sqliteValueType)
        {
        case SQLITE_INTEGER:
        {
            const sqlite3_int64 value = sqlite3_column_int64(statement, columnIndex);
            return GB_Variant(static_cast<long long>(value));
        }
        case SQLITE_FLOAT:
        {
            const double value = sqlite3_column_double(statement, columnIndex);
            return GB_Variant(value);
        }
        case SQLITE_TEXT:
        {
            const unsigned char* textPtr = sqlite3_column_text(statement, columnIndex);
            const int byteCount = sqlite3_column_bytes(statement, columnIndex);
            if (textPtr == nullptr || byteCount <= 0)
            {
                return GB_Variant(std::string());
            }

            return GB_Variant(std::string(reinterpret_cast<const char*>(textPtr), static_cast<std::size_t>(byteCount)));
        }
        case SQLITE_BLOB:
        {
            const void* blobPtr = sqlite3_column_blob(statement, columnIndex);
            const int byteCount = sqlite3_column_bytes(statement, columnIndex);
            if (blobPtr == nullptr || byteCount <= 0)
            {
                return GB_Variant(GB_ByteBuffer());
            }

            const unsigned char* beginPtr = static_cast<const unsigned char*>(blobPtr);
            const unsigned char* endPtr = beginPtr + byteCount;
            GB_ByteBuffer buffer;
            buffer.assign(beginPtr, endPtr);
            return GB_Variant(std::move(buffer));
        }
        case SQLITE_NULL:
        default:
            return GB_Variant();
        }
    }

    static std::vector<GB_SqliteColumnInfo> ReadColumnInfos(sqlite3_stmt* statement)
    {
        std::vector<GB_SqliteColumnInfo> columns;
        const int columnCount = sqlite3_column_count(statement);
        columns.reserve(static_cast<std::size_t>(std::max(columnCount, 0)));
        for (int columnIndex = 0; columnIndex < columnCount; columnIndex++)
        {
            GB_SqliteColumnInfo column;
            column.nameUtf8 = SafeString(sqlite3_column_name(statement, columnIndex));
#ifndef SQLITE_OMIT_DECLTYPE
            column.declaredTypeUtf8 = SafeString(sqlite3_column_decltype(statement, columnIndex));
#endif
#ifdef SQLITE_ENABLE_COLUMN_METADATA
            column.databaseNameUtf8 = SafeString(sqlite3_column_database_name(statement, columnIndex));
            column.tableNameUtf8 = SafeString(sqlite3_column_table_name(statement, columnIndex));
            column.originNameUtf8 = SafeString(sqlite3_column_origin_name(statement, columnIndex));
#endif
            columns.push_back(std::move(column));
        }

        return columns;
    }

    static void FillCurrentRow(sqlite3_stmt* statement, int columnCount, std::vector<GB_Variant>& outRow)
    {
        outRow.clear();
        outRow.reserve(static_cast<std::size_t>(std::max(columnCount, 0)));
        for (int columnIndex = 0; columnIndex < columnCount; columnIndex++)
        {
            outRow.push_back(GetColumnValue(statement, columnIndex));
        }
    }
}

struct GB_Sqlite::Impl
{
    GB_ReadWriteLock lifecycleLock;
    GB_ReadWriteLock schemaLock;
    bool isOpen = false;
    std::string databasePathUtf8;
    GB_SqliteOptions options;
    bool useSingleConnectionForReads = false;

    std::unique_ptr<GB_SqliteConnection> writeConnection;
    std::vector<std::unique_ptr<GB_SqliteConnection>> readConnections;
    std::vector<std::size_t> availableReadConnectionIndexes;
    bool pendingReadStatementCacheClear = false;
    int pendingReadConnectionBusyTimeoutMs = -1;

    mutable std::mutex readPoolMutex;
    mutable std::condition_variable readPoolCondition;
    mutable std::mutex writeMutex;
    mutable std::mutex errorMutex;
    mutable GB_SqliteError lastError;

    void SetLastError(const GB_SqliteError& error) const
    {
        std::lock_guard<std::mutex> lockGuard(errorMutex);
        lastError = error;
    }

    GB_SqliteError GetLastError() const
    {
        std::lock_guard<std::mutex> lockGuard(errorMutex);
        return lastError;
    }

    void ClearLastError() const
    {
        std::lock_guard<std::mutex> lockGuard(errorMutex);
        lastError.Clear();
    }

    bool CanWrite() const
    {
        if (!isOpen || !writeConnection || writeConnection->database == nullptr)
        {
            return false;
        }

        if (IsReadonlyOpenMode(options.openMode) || writeConnection->readOnly)
        {
            return false;
        }

        return sqlite3_db_readonly(writeConnection->database, "main") == 0;
    }

    bool OpenConnection(const std::string& databasePathUtf8Value, bool readOnly, std::unique_ptr<GB_SqliteConnection>& outConnection, GB_SqliteError& outError)
    {
        std::unique_ptr<GB_SqliteConnection> connection(new GB_SqliteConnection());
        connection->readOnly = readOnly;
        connection->statementCache.SetCapacity(options.enableStatementCache ? options.maxCachedStatementsPerConnection : 0);

        sqlite3* database = nullptr;
        const int flags = BuildOpenFlags(options.openMode, readOnly, options);
        const int code = sqlite3_open_v2(databasePathUtf8Value.c_str(), &database, flags, nullptr);
        if (code != SQLITE_OK)
        {
            outError = MakeError(database, code, std::string(), "Failed to open SQLite database.");
            if (database != nullptr)
            {
                sqlite3_close(database);
            }
            return false;
        }

        connection->database = database;
        sqlite3_extended_result_codes(connection->database, 1);

        const bool actualReadOnly = sqlite3_db_readonly(connection->database, "main") != 0;
        connection->readOnly = readOnly || IsReadonlyOpenMode(options.openMode) || actualReadOnly;
        if (!readOnly && !IsReadonlyOpenMode(options.openMode) && actualReadOnly)
        {
            outError = MakeUserError("SQLite database was opened as readonly by SQLite although a writable connection was requested.");
            return false;
        }

        if (options.busyTimeoutMs > 0)
        {
            const int busyCode = sqlite3_busy_timeout(connection->database, options.busyTimeoutMs);
            if (busyCode != SQLITE_OK)
            {
                outError = MakeError(connection->database, busyCode, std::string());
                return false;
            }
        }

        if (!ApplyConnectionPragmas(*connection, databasePathUtf8Value, outError))
        {
            return false;
        }

        outConnection = std::move(connection);
        outError.Clear();
        return true;
    }

    bool ApplyConnectionPragmas(GB_SqliteConnection& connection, const std::string& databasePathUtf8Value, GB_SqliteError& outError)
    {
        if (connection.database == nullptr)
        {
            outError = MakeUserError("SQLite connection is not open.");
            return false;
        }

        if (options.enableForeignKeys)
        {
            if (!DirectExec(connection.database, "PRAGMA foreign_keys=ON", outError))
            {
                return false;
            }
        }

        if (options.cacheSizeKb > 0)
        {
            const std::string sqlUtf8 = "PRAGMA cache_size=-" + ToString(options.cacheSizeKb);
            if (!DirectExec(connection.database, sqlUtf8, outError))
            {
                return false;
            }
        }

        if (options.memoryMapSizeBytes > 0)
        {
            const std::string sqlUtf8 = "PRAGMA mmap_size=" + ToString(options.memoryMapSizeBytes);
            if (!DirectExec(connection.database, sqlUtf8, outError))
            {
                return false;
            }
        }

        if (!connection.readOnly && options.enableWal && !IsReadonlyOpenMode(options.openMode) && CanUseWalForDatabasePath(databasePathUtf8Value))
        {
            if (!ExecuteWalPragma(connection.database, outError))
            {
                return false;
            }
        }

        {
            const std::string sqlUtf8 = std::string("PRAGMA synchronous=") + ToSynchronousPragmaValue(options.synchronousMode);
            if (!DirectExec(connection.database, sqlUtf8, outError))
            {
                return false;
            }
        }

        if (options.enableTempStoreMemory)
        {
            if (!DirectExec(connection.database, "PRAGMA temp_store=MEMORY", outError))
            {
                return false;
            }
        }

        if (!connection.readOnly && options.walAutoCheckpointPages > 0 && options.enableWal && CanUseWalForDatabasePath(databasePathUtf8Value))
        {
            const std::string sqlUtf8 = "PRAGMA wal_autocheckpoint=" + ToString(options.walAutoCheckpointPages);
            if (!DirectExec(connection.database, sqlUtf8, outError))
            {
                return false;
            }
        }

        if ((connection.readOnly || IsReadonlyOpenMode(options.openMode)) && options.enableQueryOnlyForReadConnections)
        {
            if (!DirectExec(connection.database, "PRAGMA query_only=ON", outError))
            {
                return false;
            }
        }

        outError.Clear();
        return true;
    }

    void ClearAvailableReadConnectionStatementCachesUnlocked()
    {
        for (std::size_t index = 0; index < availableReadConnectionIndexes.size(); index++)
        {
            const std::size_t connectionIndex = availableReadConnectionIndexes[index];
            if (connectionIndex < readConnections.size() && readConnections[connectionIndex])
            {
                readConnections[connectionIndex]->statementCache.Clear();
            }
        }

        pendingReadStatementCacheClear = availableReadConnectionIndexes.size() < readConnections.size();
    }

    void ClearAllStatementCachesWithWriteLockHeld()
    {
        if (writeConnection)
        {
            writeConnection->statementCache.Clear();
        }

        std::lock_guard<std::mutex> readLock(readPoolMutex);
        ClearAvailableReadConnectionStatementCachesUnlocked();
    }

    void CloseUnlocked()
    {
        {
            std::lock_guard<std::mutex> lockGuard(readPoolMutex);
            pendingReadStatementCacheClear = false;
            pendingReadConnectionBusyTimeoutMs = -1;
            availableReadConnectionIndexes.clear();
            readConnections.clear();
        }

        {
            std::lock_guard<std::mutex> lockGuard(writeMutex);
            writeConnection.reset();
        }

        isOpen = false;
        useSingleConnectionForReads = false;
        databasePathUtf8.clear();
        readPoolCondition.notify_all();
    }

    bool PrepareOpenedDatabase(const std::string& databasePathUtf8Value, const GB_SqliteOptions& openOptions, GB_SqliteError& outError)
    {
        options = openOptions;
        options.readConnectionCount = NormalizeReadConnectionCount(options.readConnectionCount);
        if (options.busyTimeoutMs < 0)
        {
            options.busyTimeoutMs = 0;
        }

        if (options.maxCachedStatementsPerConnection > 1024)
        {
            options.maxCachedStatementsPerConnection = 1024;
        }

        if (options.memoryMapSizeBytes < 0)
        {
            options.memoryMapSizeBytes = 0;
        }

        if (!OpenConnection(databasePathUtf8Value, false, writeConnection, outError))
        {
            return false;
        }

        readConnections.clear();
        availableReadConnectionIndexes.clear();
        pendingReadStatementCacheClear = false;
        pendingReadConnectionBusyTimeoutMs = -1;
        useSingleConnectionForReads = ShouldUseSingleConnectionForReads(databasePathUtf8Value);

        if (!useSingleConnectionForReads)
        {
            readConnections.reserve(options.readConnectionCount);
            availableReadConnectionIndexes.reserve(options.readConnectionCount);

            for (std::size_t index = 0; index < options.readConnectionCount; index++)
            {
                std::unique_ptr<GB_SqliteConnection> readConnection;
                if (!OpenConnection(databasePathUtf8Value, true, readConnection, outError))
                {
                    return false;
                }

                readConnections.push_back(std::move(readConnection));
                availableReadConnectionIndexes.push_back(index);
            }
        }

        isOpen = true;
        databasePathUtf8 = databasePathUtf8Value;
        outError.Clear();
        return true;
    }

    struct ReadConnectionLease
    {
        Impl* owner = nullptr;
        GB_SqliteConnection* connection = nullptr;
        std::size_t index = 0;

        ~ReadConnectionLease()
        {
            Release();
        }

        ReadConnectionLease() = default;
        ReadConnectionLease(const ReadConnectionLease& other) = delete;
        ReadConnectionLease& operator=(const ReadConnectionLease& other) = delete;

        void Release()
        {
            if (owner == nullptr || connection == nullptr)
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lockGuard(owner->readPoolMutex);
                if (owner->pendingReadStatementCacheClear)
                {
                    connection->statementCache.Clear();
                }

                if (owner->pendingReadConnectionBusyTimeoutMs >= 0 && connection->database != nullptr)
                {
                    (void)sqlite3_busy_timeout(connection->database, owner->pendingReadConnectionBusyTimeoutMs);
                }

                owner->availableReadConnectionIndexes.push_back(index);
                if (owner->availableReadConnectionIndexes.size() == owner->readConnections.size())
                {
                    owner->pendingReadStatementCacheClear = false;
                    owner->pendingReadConnectionBusyTimeoutMs = -1;
                }
            }
            owner->readPoolCondition.notify_one();
            owner = nullptr;
            connection = nullptr;
            index = 0;
        }
    };

    bool AcquireReadConnection(ReadConnectionLease& outLease, GB_SqliteError& outError)
    {
        if (!isOpen || readConnections.empty())
        {
            outError = MakeUserError("SQLite database is not open.");
            return false;
        }

        std::unique_lock<std::mutex> lockGuard(readPoolMutex);
        readPoolCondition.wait(lockGuard, [this]() { return !availableReadConnectionIndexes.empty(); });
        const std::size_t index = availableReadConnectionIndexes.back();
        availableReadConnectionIndexes.pop_back();
        lockGuard.unlock();

        outLease.owner = this;
        outLease.index = index;
        outLease.connection = readConnections[index].get();
        outError.Clear();
        return true;
    }

    bool ExecuteOnConnection(GB_SqliteConnection& connection, const std::string& sqlUtf8, const GB_SqliteParameterList* parameters, const GB_SqliteNamedParameters* namedParameters, GB_SqliteError& outError)
    {
        GB_SqliteStatementHandle statementHandle;
        if (!PrepareStatement(connection, options, sqlUtf8, statementHandle, outError))
        {
            return false;
        }

        sqlite3_stmt* statement = statementHandle.Get();
        if (parameters != nullptr)
        {
            if (!BindParameters(statement, *parameters, outError, sqlUtf8))
            {
                return false;
            }
        }
        else if (namedParameters != nullptr)
        {
            if (!BindNamedParameters(statement, *namedParameters, options.rejectUnusedNamedParameters, outError, sqlUtf8))
            {
                return false;
            }
        }
        else
        {
            if (!BindNoParameters(statement, outError, sqlUtf8))
            {
                return false;
            }
        }

        while (true)
        {
            const int stepCode = sqlite3_step(statement);
            if (stepCode == SQLITE_DONE)
            {
                return ResetStatementHandle(connection, sqlUtf8, statementHandle, outError);
            }

            if (stepCode == SQLITE_ROW)
            {
                continue;
            }

            DropCachedStatementAfterError(connection, sqlUtf8, statementHandle, stepCode);
            outError = MakeError(connection.database, stepCode, sqlUtf8);
            return false;
        }
    }

    bool ExecuteManyOnConnection(GB_SqliteConnection& connection, const std::string& sqlUtf8, const std::vector<GB_SqliteParameterList>* parameterRows, const std::vector<GB_SqliteNamedParameters>* namedParameterRows, bool useTransaction, GB_SqliteError& outError)
    {
        if ((parameterRows == nullptr || parameterRows->empty()) && (namedParameterRows == nullptr || namedParameterRows->empty()))
        {
            outError.Clear();
            return true;
        }

        GB_SqliteStatementHandle statementHandle;
        if (!PrepareStatement(connection, options, sqlUtf8, statementHandle, outError))
        {
            return false;
        }

        sqlite3_stmt* statement = statementHandle.Get();
        if (sqlite3_stmt_readonly(statement) != 0)
        {
            outError = MakeUserError("GB_Sqlite::ExecuteMany() only accepts write SQL. Use Query() for readonly SQL.", sqlUtf8);
            return false;
        }

        std::vector<GB_SqliteNamedParameterBinding> namedBindings;
        if (namedParameterRows != nullptr)
        {
            if (!BuildNamedParameterBindings(statement, namedBindings, outError, sqlUtf8))
            {
                return false;
            }
        }

        bool transactionStarted = false;
        const auto rollbackOnFailure = [&connection, &outError, &sqlUtf8, &statementHandle, &transactionStarted]()
            {
                GB_SqliteError resetError;
                (void)ResetStatementHandle(connection, sqlUtf8, statementHandle, resetError);

                if (transactionStarted)
                {
                    GB_SqliteError rollbackError;
                    (void)DirectExec(connection.database, "ROLLBACK", rollbackError);
                    transactionStarted = false;
                }

                if (outError.IsOk())
                {
                    outError = MakeUserError("SQLite ExecuteMany failed.", sqlUtf8);
                }
                return false;
            };

        try
        {
            if (useTransaction && sqlite3_get_autocommit(connection.database) != 0)
            {
                if (!DirectExec(connection.database, ToTransactionBeginSql(GB_SqliteTransactionMode::Immediate), outError))
                {
                    return false;
                }
                transactionStarted = true;
            }

            const std::size_t rowCount = parameterRows != nullptr ? parameterRows->size() : namedParameterRows->size();
            for (std::size_t rowIndex = 0; rowIndex < rowCount; rowIndex++)
            {
                if (parameterRows != nullptr)
                {
                    const GB_SqliteParameterList& parameters = (*parameterRows)[rowIndex];
                    if (!CheckPositionalParameterCount(statement, parameters.size(), outError, sqlUtf8) || !BindParametersUnchecked(statement, parameters, outError, sqlUtf8))
                    {
                        return rollbackOnFailure();
                    }
                }
                else
                {
                    const GB_SqliteNamedParameters& parameters = (*namedParameterRows)[rowIndex];
                    if (!CheckUnusedNamedParameters(namedBindings, parameters, options.rejectUnusedNamedParameters, outError, sqlUtf8) || !BindNamedParametersUnchecked(statement, namedBindings, parameters, outError, sqlUtf8))
                    {
                        return rollbackOnFailure();
                    }
                }

                while (true)
                {
                    const int stepCode = sqlite3_step(statement);
                    if (stepCode == SQLITE_DONE)
                    {
                        break;
                    }

                    if (stepCode == SQLITE_ROW)
                    {
                        continue;
                    }

                    DropCachedStatementAfterError(connection, sqlUtf8, statementHandle, stepCode);
                    outError = MakeError(connection.database, stepCode, sqlUtf8);
                    return rollbackOnFailure();
                }

                const int resetCode = sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                if (resetCode != SQLITE_OK)
                {
                    outError = MakeError(connection.database, resetCode, sqlUtf8);
                    return rollbackOnFailure();
                }
            }

            if (transactionStarted)
            {
                if (!DirectExec(connection.database, "COMMIT", outError))
                {
                    return rollbackOnFailure();
                }
                transactionStarted = false;
            }

            outError.Clear();
            return true;
        }
        catch (const std::bad_alloc&)
        {
            outError = MakeBadAllocError("execute SQLite batch statement", sqlUtf8);
            return rollbackOnFailure();
        }
        catch (...)
        {
            outError = MakeExceptionError("execute SQLite batch statement", sqlUtf8);
            return rollbackOnFailure();
        }
    }

    bool QueryOnConnection(GB_SqliteConnection& connection, const std::string& sqlUtf8, const GB_SqliteParameterList* parameters, const GB_SqliteNamedParameters* namedParameters, GB_SqliteResult& outResult, std::size_t maxRowCount, bool requireReadOnly)
    {
        outResult.Clear();
        GB_SqliteError error;
        GB_SqliteStatementHandle statementHandle;
        if (!PrepareStatement(connection, options, sqlUtf8, statementHandle, error))
        {
            outResult.error = error;
            return false;
        }

        sqlite3_stmt* statement = statementHandle.Get();
        if (requireReadOnly && sqlite3_stmt_readonly(statement) == 0)
        {
            outResult.error = MakeUserError("GB_Sqlite::Query() only accepts readonly SQL. Use Execute() for write SQL.", sqlUtf8);
            return false;
        }

        if (parameters != nullptr)
        {
            if (!BindParameters(statement, *parameters, error, sqlUtf8))
            {
                outResult.error = error;
                return false;
            }
        }
        else if (namedParameters != nullptr)
        {
            if (!BindNamedParameters(statement, *namedParameters, options.rejectUnusedNamedParameters, error, sqlUtf8))
            {
                outResult.error = error;
                return false;
            }
        }
        else
        {
            if (!BindNoParameters(statement, error, sqlUtf8))
            {
                outResult.error = error;
                return false;
            }
        }

        try
        {
            outResult.columns = ReadColumnInfos(statement);
            const std::size_t reserveCount = GetResultReserveCount(maxRowCount);
            if (reserveCount > 0)
            {
                outResult.rows.reserve(reserveCount);
            }
        }
        catch (...)
        {
            GB_SqliteError ignoredResetError;
            (void)ResetStatementHandle(connection, sqlUtf8, statementHandle, ignoredResetError);
            outResult.Clear();
            outResult.error = MakeUserError("Failed to allocate SQLite query result storage.", sqlUtf8);
            return false;
        }

        const int columnCount = sqlite3_column_count(statement);

        while (true)
        {
            const int stepCode = sqlite3_step(statement);
            if (stepCode == SQLITE_DONE)
            {
                GB_SqliteError resetError;
                if (!ResetStatementHandle(connection, sqlUtf8, statementHandle, resetError))
                {
                    outResult.error = resetError;
                    return false;
                }

                outResult.error.Clear();
                return true;
            }

            if (stepCode == SQLITE_ROW)
            {
                if (maxRowCount == 0 || outResult.rows.size() < maxRowCount)
                {
                    try
                    {
                        outResult.rows.push_back(std::vector<GB_Variant>());
                        FillCurrentRow(statement, columnCount, outResult.rows.back());
                    }
                    catch (...)
                    {
                        GB_SqliteError ignoredResetError;
                        (void)ResetStatementHandle(connection, sqlUtf8, statementHandle, ignoredResetError);
                        outResult.Clear();
                        outResult.error = MakeUserError("Failed to read SQLite query result row.", sqlUtf8);
                        return false;
                    }
                    continue;
                }

                GB_SqliteError resetError;
                if (!ResetStatementHandle(connection, sqlUtf8, statementHandle, resetError))
                {
                    outResult.error = resetError;
                    return false;
                }

                outResult.error.Clear();
                return true;
            }

            DropCachedStatementAfterError(connection, sqlUtf8, statementHandle, stepCode);
            outResult.error = MakeError(connection.database, stepCode, sqlUtf8);
            return false;
        }
    }

    bool QueryEachOnConnection(GB_SqliteConnection& connection, const std::string& sqlUtf8, const GB_SqliteParameterList* parameters, const GB_SqliteNamedParameters* namedParameters, const GB_SqliteRowCallback& rowCallback, GB_SqliteError& outError, bool requireReadOnly)
    {
        if (!rowCallback)
        {
            outError = MakeUserError("SQLite row callback is empty.", sqlUtf8);
            return false;
        }

        GB_SqliteStatementHandle statementHandle;
        if (!PrepareStatement(connection, options, sqlUtf8, statementHandle, outError))
        {
            return false;
        }

        sqlite3_stmt* statement = statementHandle.Get();
        if (requireReadOnly && sqlite3_stmt_readonly(statement) == 0)
        {
            outError = MakeUserError("GB_Sqlite::QueryEach() only accepts readonly SQL. Use Execute() for write SQL.", sqlUtf8);
            return false;
        }

        if (parameters != nullptr)
        {
            if (!BindParameters(statement, *parameters, outError, sqlUtf8))
            {
                return false;
            }
        }
        else if (namedParameters != nullptr)
        {
            if (!BindNamedParameters(statement, *namedParameters, options.rejectUnusedNamedParameters, outError, sqlUtf8))
            {
                return false;
            }
        }
        else
        {
            if (!BindNoParameters(statement, outError, sqlUtf8))
            {
                return false;
            }
        }

        std::vector<GB_SqliteColumnInfo> columns;
        try
        {
            columns = ReadColumnInfos(statement);
        }
        catch (...)
        {
            GB_SqliteError ignoredResetError;
            (void)ResetStatementHandle(connection, sqlUtf8, statementHandle, ignoredResetError);
            outError = MakeUserError("Failed to allocate SQLite query column metadata.", sqlUtf8);
            return false;
        }

        const int columnCount = sqlite3_column_count(statement);
        std::vector<GB_Variant> values;

        while (true)
        {
            const int stepCode = sqlite3_step(statement);
            if (stepCode == SQLITE_DONE)
            {
                return ResetStatementHandle(connection, sqlUtf8, statementHandle, outError);
            }

            if (stepCode == SQLITE_ROW)
            {
                try
                {
                    FillCurrentRow(statement, columnCount, values);
                }
                catch (...)
                {
                    GB_SqliteError ignoredResetError;
                    (void)ResetStatementHandle(connection, sqlUtf8, statementHandle, ignoredResetError);
                    outError = MakeUserError("Failed to read SQLite query callback row.", sqlUtf8);
                    return false;
                }

                bool shouldContinue = false;
                try
                {
                    shouldContinue = rowCallback(columns, values);
                }
                catch (...)
                {
                    GB_SqliteError ignoredResetError;
                    (void)ResetStatementHandle(connection, sqlUtf8, statementHandle, ignoredResetError);
                    outError = MakeUserError("SQLite row callback threw an exception.", sqlUtf8);
                    return false;
                }

                if (!shouldContinue)
                {
                    return ResetStatementHandle(connection, sqlUtf8, statementHandle, outError);
                }
                continue;
            }

            DropCachedStatementAfterError(connection, sqlUtf8, statementHandle, stepCode);
            outError = MakeError(connection.database, stepCode, sqlUtf8);
            return false;
        }
    }


    bool ExecuteScalarOnConnection(GB_SqliteConnection& connection, const std::string& sqlUtf8, const GB_SqliteParameterList* parameters, const GB_SqliteNamedParameters* namedParameters, GB_Variant& outValue, GB_SqliteError& outError, bool requireReadOnly)
    {
        outValue.Reset();

        GB_SqliteStatementHandle statementHandle;
        if (!PrepareStatement(connection, options, sqlUtf8, statementHandle, outError))
        {
            return false;
        }

        sqlite3_stmt* statement = statementHandle.Get();
        if (requireReadOnly && sqlite3_stmt_readonly(statement) == 0)
        {
            outError = MakeUserError("GB_Sqlite::ExecuteScalar() only accepts readonly SQL. Use Execute() for write SQL.", sqlUtf8);
            return false;
        }

        if (parameters != nullptr)
        {
            if (!BindParameters(statement, *parameters, outError, sqlUtf8))
            {
                return false;
            }
        }
        else if (namedParameters != nullptr)
        {
            if (!BindNamedParameters(statement, *namedParameters, options.rejectUnusedNamedParameters, outError, sqlUtf8))
            {
                return false;
            }
        }
        else
        {
            if (!BindNoParameters(statement, outError, sqlUtf8))
            {
                return false;
            }
        }

        const int stepCode = sqlite3_step(statement);
        if (stepCode == SQLITE_DONE)
        {
            return ResetStatementHandle(connection, sqlUtf8, statementHandle, outError);
        }

        if (stepCode == SQLITE_ROW)
        {
            try
            {
                if (sqlite3_column_count(statement) > 0)
                {
                    outValue = GetColumnValue(statement, 0);
                }
            }
            catch (...)
            {
                GB_SqliteError ignoredResetError;
                (void)ResetStatementHandle(connection, sqlUtf8, statementHandle, ignoredResetError);
                outError = MakeUserError("Failed to read SQLite scalar result.", sqlUtf8);
                outValue.Reset();
                return false;
            }

            if (!ResetStatementHandle(connection, sqlUtf8, statementHandle, outError))
            {
                outValue.Reset();
                return false;
            }

            outError.Clear();
            return true;
        }

        DropCachedStatementAfterError(connection, sqlUtf8, statementHandle, stepCode);
        outError = MakeError(connection.database, stepCode, sqlUtf8);
        outValue.Reset();
        return false;
    }
};

struct GB_SqliteTransaction::Impl
{
    GB_Sqlite::Impl* owner = nullptr;
    std::unique_ptr<GB_ReadLockGuard> lifecycleGuard;
    std::unique_ptr<GB_WriteLockGuard> schemaGuard;
    std::unique_lock<std::mutex> writeLock;
    bool active = false;
    bool schemaChanged = false;
    GB_SqliteError lastError;

    Impl()
    {
    }

    ~Impl()
    {
        RollbackIfActive();
    }

    Impl(const Impl& other) = delete;
    Impl& operator=(const Impl& other) = delete;

    void SetError(const GB_SqliteError& error)
    {
        lastError = error;
        if (owner != nullptr)
        {
            owner->SetLastError(error);
        }
    }

    void ClearError()
    {
        lastError.Clear();
        if (owner != nullptr)
        {
            owner->ClearLastError();
        }
    }

    bool Begin(GB_Sqlite& databaseValue, GB_SqliteTransactionMode transactionMode)
    {
        if (!databaseValue.impl_)
        {
            lastError = MakeUserError("SQLite database object is empty.");
            return false;
        }

        owner = databaseValue.impl_.get();
        lifecycleGuard.reset(new GB_ReadLockGuard(owner->lifecycleLock));
        if (!owner->isOpen || !owner->writeConnection || owner->writeConnection->database == nullptr)
        {
            SetError(MakeUserError("SQLite database is not open."));
            lifecycleGuard.reset();
            owner = nullptr;
            return false;
        }

        schemaGuard.reset(new GB_WriteLockGuard(owner->schemaLock));
        writeLock = std::unique_lock<std::mutex>(owner->writeMutex);
        if (!owner->CanWrite())
        {
            SetError(MakeUserError("SQLite database is readonly."));
            writeLock.unlock();
            schemaGuard.reset();
            lifecycleGuard.reset();
            owner = nullptr;
            return false;
        }

        GB_SqliteError error;
        sqlite3* sqliteDatabase = owner->writeConnection->database;
        if (!DirectExec(sqliteDatabase, ToTransactionBeginSql(transactionMode), error))
        {
            SetError(error);
            writeLock.unlock();
            schemaGuard.reset();
            lifecycleGuard.reset();
            owner = nullptr;
            return false;
        }

        if (sqlite3_get_autocommit(sqliteDatabase) != 0)
        {
            SetError(MakeUserError("SQLite transaction was not started successfully."));
            writeLock.unlock();
            schemaGuard.reset();
            lifecycleGuard.reset();
            owner = nullptr;
            return false;
        }

        active = true;
        ClearError();
        return true;
    }

    bool Commit()
    {
        if (!active || owner == nullptr || !owner->writeConnection || owner->writeConnection->database == nullptr)
        {
            SetError(MakeUserError("SQLite transaction is not active."));
            return false;
        }

        GB_SqliteError error;
        sqlite3* sqliteDatabase = owner->writeConnection->database;
        if (!DirectExec(sqliteDatabase, "COMMIT", error))
        {
            SetError(error);
            return false;
        }

        if (sqlite3_get_autocommit(sqliteDatabase) == 0)
        {
            SetError(MakeUserError("SQLite transaction commit did not return to autocommit mode."));
            return false;
        }

        active = false;
        if (schemaChanged && owner != nullptr)
        {
            owner->ClearAllStatementCachesWithWriteLockHeld();
            schemaChanged = false;
        }
        ClearError();
        writeLock.unlock();
        schemaGuard.reset();
        lifecycleGuard.reset();
        owner = nullptr;
        return true;
    }

    bool Rollback()
    {
        if (!active)
        {
            ClearError();
            return true;
        }

        if (owner == nullptr || !owner->writeConnection || owner->writeConnection->database == nullptr)
        {
            SetError(MakeUserError("SQLite transaction is not active."));
            active = false;
            if (writeLock.owns_lock())
            {
                writeLock.unlock();
            }
            schemaGuard.reset();
            lifecycleGuard.reset();
            owner = nullptr;
            return false;
        }

        GB_SqliteError error;
        sqlite3* sqliteDatabase = owner->writeConnection->database;
        if (!DirectExec(sqliteDatabase, "ROLLBACK", error))
        {
            SetError(error);
            return false;
        }

        active = false;
        if (schemaChanged && owner != nullptr)
        {
            owner->ClearAllStatementCachesWithWriteLockHeld();
            schemaChanged = false;
        }
        ClearError();
        writeLock.unlock();
        schemaGuard.reset();
        lifecycleGuard.reset();
        owner = nullptr;
        return true;
    }

    void RollbackIfActive()
    {
        if (active && owner != nullptr && owner->writeConnection && owner->writeConnection->database != nullptr)
        {
            GB_SqliteError ignoredError;
            (void)DirectExec(owner->writeConnection->database, "ROLLBACK", ignoredError);
            active = false;
            if (schemaChanged)
            {
                owner->ClearAllStatementCachesWithWriteLockHeld();
                schemaChanged = false;
            }
        }

        if (writeLock.owns_lock())
        {
            writeLock.unlock();
        }
        schemaGuard.reset();
        lifecycleGuard.reset();
        owner = nullptr;
    }
};

bool GB_SqliteError::IsOk() const
{
    return code == GB_SqliteOk && extendedCode == GB_SqliteOk && messageUtf8.empty();
}

void GB_SqliteError::Clear()
{
    code = 0;
    extendedCode = 0;
    messageUtf8.clear();
    sqlUtf8.clear();
}

bool GB_SqliteResult::IsOk() const
{
    return error.IsOk();
}

bool GB_SqliteResult::IsEmpty() const
{
    return rows.empty();
}

std::size_t GB_SqliteResult::RowCount() const
{
    return rows.size();
}

std::size_t GB_SqliteResult::ColumnCount() const
{
    return columns.size();
}

int GB_SqliteResult::GetColumnIndex(const std::string& columnNameUtf8) const
{
    for (std::size_t index = 0; index < columns.size(); index++)
    {
        if (columns[index].nameUtf8 == columnNameUtf8)
        {
            return static_cast<int>(index);
        }
    }

    return -1;
}

bool GB_SqliteResult::TryGetValue(std::size_t rowIndex, std::size_t columnIndex, GB_Variant& outValue) const
{
    if (rowIndex >= rows.size() || columnIndex >= rows[rowIndex].size())
    {
        return false;
    }

    outValue = rows[rowIndex][columnIndex];
    return true;
}

bool GB_SqliteResult::TryGetValue(std::size_t rowIndex, const std::string& columnNameUtf8, GB_Variant& outValue) const
{
    const int columnIndex = GetColumnIndex(columnNameUtf8);
    if (columnIndex < 0)
    {
        return false;
    }

    return TryGetValue(rowIndex, static_cast<std::size_t>(columnIndex), outValue);
}

GB_Variant GB_SqliteResult::GetValue(std::size_t rowIndex, std::size_t columnIndex) const
{
    GB_Variant value;
    (void)TryGetValue(rowIndex, columnIndex, value);
    return value;
}

GB_Variant GB_SqliteResult::GetValue(std::size_t rowIndex, const std::string& columnNameUtf8) const
{
    GB_Variant value;
    (void)TryGetValue(rowIndex, columnNameUtf8, value);
    return value;
}

void GB_SqliteResult::Clear()
{
    columns.clear();
    rows.clear();
    error.Clear();
}

bool GB_SqliteCheckpointResult::IsComplete() const
{
    return logFrameCount >= 0 && checkpointedFrameCount >= 0 && checkpointedFrameCount >= logFrameCount;
}

GB_Sqlite::GB_Sqlite() : impl_(new Impl())
{
}

GB_Sqlite::GB_Sqlite(const std::string& databasePathUtf8, const GB_SqliteOptions& options) : impl_(new Impl())
{
    (void)Open(databasePathUtf8, options);
}

GB_Sqlite::~GB_Sqlite()
{
    Close();
}

GB_Sqlite::GB_Sqlite(GB_Sqlite&& other) noexcept : impl_(std::move(other.impl_))
{
    if (!impl_)
    {
        impl_.reset(new Impl());
    }
}

GB_Sqlite& GB_Sqlite::operator=(GB_Sqlite&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Close();
    impl_ = std::move(other.impl_);
    if (!impl_)
    {
        impl_.reset(new Impl());
    }
    return *this;
}

bool GB_Sqlite::Open(const std::string& databasePathUtf8, const GB_SqliteOptions& options)
{
    if (!impl_)
    {
        impl_.reset(new Impl());
    }

    GB_WriteLockGuard lifecycleGuard(impl_->lifecycleLock);
    impl_->CloseUnlocked();

    if (databasePathUtf8.empty())
    {
        const GB_SqliteError error = MakeUserError("SQLite database path is empty.");
        impl_->SetLastError(error);
        return false;
    }

    if (sqlite3_threadsafe() == 0)
    {
        const GB_SqliteError error = MakeUserError("SQLite library was compiled with SQLITE_THREADSAFE=0. GB_Sqlite requires a thread-safe SQLite build.");
        impl_->SetLastError(error);
        return false;
    }

    GB_SqliteError error;
    try
    {
        if (!impl_->PrepareOpenedDatabase(databasePathUtf8, options, error))
        {
            impl_->CloseUnlocked();
            impl_->SetLastError(error);
            return false;
        }
    }
    catch (const std::bad_alloc&)
    {
        impl_->CloseUnlocked();
        impl_->SetLastError(MakeBadAllocError("open SQLite database"));
        return false;
    }
    catch (...)
    {
        impl_->CloseUnlocked();
        impl_->SetLastError(MakeExceptionError("open SQLite database"));
        return false;
    }

    impl_->ClearLastError();
    return true;
}

void GB_Sqlite::Close()
{
    if (!impl_)
    {
        return;
    }

    GB_WriteLockGuard lifecycleGuard(impl_->lifecycleLock);
    impl_->CloseUnlocked();
    impl_->ClearLastError();
}

bool GB_Sqlite::IsOpen() const
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    return impl_->isOpen;
}

bool GB_Sqlite::IsReadOnly() const
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection || impl_->writeConnection->database == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    return IsReadonlyOpenMode(impl_->options.openMode) || impl_->writeConnection->readOnly || sqlite3_db_readonly(impl_->writeConnection->database, "main") != 0;
}

std::string GB_Sqlite::GetDatabasePathUtf8() const
{
    if (!impl_)
    {
        return std::string();
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    return impl_->databasePathUtf8;
}

GB_SqliteOptions GB_Sqlite::GetOptions() const
{
    if (!impl_)
    {
        return GB_SqliteOptions();
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    return impl_->options;
}

GB_SqliteError GB_Sqlite::GetLastError() const
{
    if (!impl_)
    {
        return MakeUserError("GB_Sqlite object is empty.");
    }

    return impl_->GetLastError();
}

void GB_Sqlite::ClearLastError()
{
    if (!impl_)
    {
        return;
    }

    impl_->ClearLastError();
}

bool GB_Sqlite::Execute(const std::string& sqlUtf8)
{
    return Execute(sqlUtf8, GB_SqliteParameterList());
}

bool GB_Sqlite::Execute(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters)
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open.", sqlUtf8));
        return false;
    }

    const bool isSchemaChangingSql = IsProbablySchemaChangingSql(sqlUtf8);
    GB_SqliteOptionalWriteLockGuard schemaGuard(impl_->schemaLock, isSchemaChangingSql);

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    if (!impl_->CanWrite())
    {
        impl_->SetLastError(MakeUserError("SQLite database is readonly.", sqlUtf8));
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->ExecuteOnConnection(*impl_->writeConnection, sqlUtf8, &parameters, nullptr, error);
    if (ok && isSchemaChangingSql)
    {
        impl_->ClearAllStatementCachesWithWriteLockHeld();
    }
    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_Sqlite::ExecuteNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters)
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open.", sqlUtf8));
        return false;
    }

    const bool isSchemaChangingSql = IsProbablySchemaChangingSql(sqlUtf8);
    GB_SqliteOptionalWriteLockGuard schemaGuard(impl_->schemaLock, isSchemaChangingSql);

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    if (!impl_->CanWrite())
    {
        impl_->SetLastError(MakeUserError("SQLite database is readonly.", sqlUtf8));
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->ExecuteOnConnection(*impl_->writeConnection, sqlUtf8, nullptr, &parameters, error);
    if (ok && isSchemaChangingSql)
    {
        impl_->ClearAllStatementCachesWithWriteLockHeld();
    }
    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_Sqlite::ExecuteMany(const std::string& sqlUtf8, const std::vector<GB_SqliteParameterList>& parameterRows, bool useTransaction)
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open.", sqlUtf8));
        return false;
    }

    const bool isSchemaChangingSql = IsProbablySchemaChangingSql(sqlUtf8);
    GB_SqliteOptionalWriteLockGuard schemaGuard(impl_->schemaLock, isSchemaChangingSql);

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    if (!impl_->CanWrite())
    {
        impl_->SetLastError(MakeUserError("SQLite database is readonly.", sqlUtf8));
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->ExecuteManyOnConnection(*impl_->writeConnection, sqlUtf8, &parameterRows, nullptr, useTransaction, error);
    if (ok && isSchemaChangingSql)
    {
        impl_->ClearAllStatementCachesWithWriteLockHeld();
    }
    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_Sqlite::ExecuteManyNamed(const std::string& sqlUtf8, const std::vector<GB_SqliteNamedParameters>& parameterRows, bool useTransaction)
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open.", sqlUtf8));
        return false;
    }

    const bool isSchemaChangingSql = IsProbablySchemaChangingSql(sqlUtf8);
    GB_SqliteOptionalWriteLockGuard schemaGuard(impl_->schemaLock, isSchemaChangingSql);

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    if (!impl_->CanWrite())
    {
        impl_->SetLastError(MakeUserError("SQLite database is readonly.", sqlUtf8));
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->ExecuteManyOnConnection(*impl_->writeConnection, sqlUtf8, nullptr, &parameterRows, useTransaction, error);
    if (ok && isSchemaChangingSql)
    {
        impl_->ClearAllStatementCachesWithWriteLockHeld();
    }
    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_Sqlite::ExecuteBatch(const std::string& sqlBatchUtf8)
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open.", sqlBatchUtf8));
        return false;
    }

    if (SqlBatchContainsAttachOrDetachSql(sqlBatchUtf8))
    {
        impl_->SetLastError(MakeUserError("SQLite ATTACH/DETACH should use AttachDatabase() or DetachDatabase() so all internal connections stay consistent.", sqlBatchUtf8));
        return false;
    }

    GB_WriteLockGuard schemaGuard(impl_->schemaLock);
    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    if (!impl_->CanWrite())
    {
        impl_->SetLastError(MakeUserError("SQLite database is readonly.", sqlBatchUtf8));
        return false;
    }

    GB_SqliteError error;
    const bool ok = DirectExec(impl_->writeConnection->database, sqlBatchUtf8, error);
    impl_->ClearAllStatementCachesWithWriteLockHeld();
    if (!ok)
    {
        if (sqlite3_get_autocommit(impl_->writeConnection->database) == 0)
        {
            GB_SqliteError rollbackError;
            (void)DirectExec(impl_->writeConnection->database, "ROLLBACK", rollbackError);
        }
        impl_->SetLastError(error);
        return false;
    }

    if (sqlite3_get_autocommit(impl_->writeConnection->database) == 0)
    {
        GB_SqliteError rollbackError;
        (void)DirectExec(impl_->writeConnection->database, "ROLLBACK", rollbackError);
        impl_->SetLastError(MakeUserError("SQLite SQL batch left an open transaction. Use GB_SqliteTransaction or commit/rollback inside the batch.", sqlBatchUtf8));
        return false;
    }

    impl_->SetLastError(MakeOkError());
    return true;
}

bool GB_Sqlite::Query(const std::string& sqlUtf8, GB_SqliteResult& outResult, std::size_t maxRowCount) const
{
    return Query(sqlUtf8, GB_SqliteParameterList(), outResult, maxRowCount);
}

bool GB_Sqlite::Query(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount) const
{
    outResult.Clear();
    if (!impl_)
    {
        outResult.error = MakeUserError("GB_Sqlite object is empty.", sqlUtf8);
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    GB_ReadLockGuard schemaGuard(impl_->schemaLock);
    GB_SqliteError error;
    if (impl_->useSingleConnectionForReads)
    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        const bool ok = impl_->QueryOnConnection(*impl_->writeConnection, sqlUtf8, &parameters, nullptr, outResult, maxRowCount, true);
        impl_->SetLastError(ok ? MakeOkError() : outResult.error);
        return ok;
    }

    Impl::ReadConnectionLease readConnectionLease;
    if (!impl_->AcquireReadConnection(readConnectionLease, error))
    {
        outResult.error = error;
        impl_->SetLastError(error);
        return false;
    }

    const bool ok = impl_->QueryOnConnection(*readConnectionLease.connection, sqlUtf8, &parameters, nullptr, outResult, maxRowCount, true);
    impl_->SetLastError(ok ? MakeOkError() : outResult.error);
    return ok;
}

bool GB_Sqlite::QueryNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount) const
{
    outResult.Clear();
    if (!impl_)
    {
        outResult.error = MakeUserError("GB_Sqlite object is empty.", sqlUtf8);
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    GB_ReadLockGuard schemaGuard(impl_->schemaLock);
    GB_SqliteError error;
    if (impl_->useSingleConnectionForReads)
    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        const bool ok = impl_->QueryOnConnection(*impl_->writeConnection, sqlUtf8, nullptr, &parameters, outResult, maxRowCount, true);
        impl_->SetLastError(ok ? MakeOkError() : outResult.error);
        return ok;
    }

    Impl::ReadConnectionLease readConnectionLease;
    if (!impl_->AcquireReadConnection(readConnectionLease, error))
    {
        outResult.error = error;
        impl_->SetLastError(error);
        return false;
    }

    const bool ok = impl_->QueryOnConnection(*readConnectionLease.connection, sqlUtf8, nullptr, &parameters, outResult, maxRowCount, true);
    impl_->SetLastError(ok ? MakeOkError() : outResult.error);
    return ok;
}

bool GB_Sqlite::QueryEach(const std::string& sqlUtf8, const GB_SqliteRowCallback& rowCallback) const
{
    return QueryEach(sqlUtf8, GB_SqliteParameterList(), rowCallback);
}

bool GB_Sqlite::QueryEach(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, const GB_SqliteRowCallback& rowCallback) const
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    GB_ReadLockGuard schemaGuard(impl_->schemaLock);
    GB_SqliteError error;
    if (impl_->useSingleConnectionForReads)
    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        const bool ok = impl_->QueryEachOnConnection(*impl_->writeConnection, sqlUtf8, &parameters, nullptr, rowCallback, error, true);
        impl_->SetLastError(ok ? MakeOkError() : error);
        return ok;
    }

    Impl::ReadConnectionLease readConnectionLease;
    if (!impl_->AcquireReadConnection(readConnectionLease, error))
    {
        impl_->SetLastError(error);
        return false;
    }

    const bool ok = impl_->QueryEachOnConnection(*readConnectionLease.connection, sqlUtf8, &parameters, nullptr, rowCallback, error, true);
    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_Sqlite::QueryEachNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, const GB_SqliteRowCallback& rowCallback) const
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    GB_ReadLockGuard schemaGuard(impl_->schemaLock);
    GB_SqliteError error;
    if (impl_->useSingleConnectionForReads)
    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        const bool ok = impl_->QueryEachOnConnection(*impl_->writeConnection, sqlUtf8, nullptr, &parameters, rowCallback, error, true);
        impl_->SetLastError(ok ? MakeOkError() : error);
        return ok;
    }

    Impl::ReadConnectionLease readConnectionLease;
    if (!impl_->AcquireReadConnection(readConnectionLease, error))
    {
        impl_->SetLastError(error);
        return false;
    }

    const bool ok = impl_->QueryEachOnConnection(*readConnectionLease.connection, sqlUtf8, nullptr, &parameters, rowCallback, error, true);
    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_Sqlite::ExecuteScalar(const std::string& sqlUtf8, GB_Variant& outValue) const
{
    return ExecuteScalar(sqlUtf8, GB_SqliteParameterList(), outValue);
}

bool GB_Sqlite::ExecuteScalar(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_Variant& outValue) const
{
    outValue.Reset();
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    GB_ReadLockGuard schemaGuard(impl_->schemaLock);
    GB_SqliteError error;
    bool ok = false;
    if (impl_->useSingleConnectionForReads)
    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        ok = impl_->ExecuteScalarOnConnection(*impl_->writeConnection, sqlUtf8, &parameters, nullptr, outValue, error, true);
    }
    else
    {
        Impl::ReadConnectionLease readConnectionLease;
        if (!impl_->AcquireReadConnection(readConnectionLease, error))
        {
            impl_->SetLastError(error);
            return false;
        }

        ok = impl_->ExecuteScalarOnConnection(*readConnectionLease.connection, sqlUtf8, &parameters, nullptr, outValue, error, true);
    }

    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_Sqlite::ExecuteScalarNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_Variant& outValue) const
{
    outValue.Reset();
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    GB_ReadLockGuard schemaGuard(impl_->schemaLock);
    GB_SqliteError error;
    bool ok = false;
    if (impl_->useSingleConnectionForReads)
    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        ok = impl_->ExecuteScalarOnConnection(*impl_->writeConnection, sqlUtf8, nullptr, &parameters, outValue, error, true);
    }
    else
    {
        Impl::ReadConnectionLease readConnectionLease;
        if (!impl_->AcquireReadConnection(readConnectionLease, error))
        {
            impl_->SetLastError(error);
            return false;
        }

        ok = impl_->ExecuteScalarOnConnection(*readConnectionLease.connection, sqlUtf8, nullptr, &parameters, outValue, error, true);
    }

    impl_->SetLastError(ok ? MakeOkError() : error);
    return ok;
}


bool GB_Sqlite::GetTableNames(std::vector<std::string>& outTableNames, bool includeSystemTables, const std::string& schemaNameUtf8) const
{
    outTableNames.clear();

    GB_SqliteError error;
    std::string schemaObjectNameUtf8;
    if (!BuildQualifiedSchemaObjectName(schemaNameUtf8, "sqlite_schema", schemaObjectNameUtf8, error))
    {
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    const std::string sqlUtf8 = "SELECT name FROM " + schemaObjectNameUtf8 + " WHERE type = 'table' AND (:includeSystemTables != 0 OR name NOT LIKE 'sqlite\\_%' ESCAPE '\\') ORDER BY name";
    GB_SqliteNamedParameters parameters;
    parameters["includeSystemTables"] = GB_Variant(includeSystemTables ? 1 : 0);

    GB_SqliteResult result;
    if (!QueryNamed(sqlUtf8, parameters, result))
    {
        return false;
    }

    try
    {
        outTableNames.reserve(result.RowCount());
        for (std::size_t rowIndex = 0; rowIndex < result.RowCount(); rowIndex++)
        {
            std::string tableNameUtf8;
            if (!VariantToString(result.GetValue(rowIndex, "name"), tableNameUtf8))
            {
                error = MakeUserError("Failed to read SQLite table name.", sqlUtf8);
                impl_->SetLastError(error);
                outTableNames.clear();
                return false;
            }

            outTableNames.push_back(tableNameUtf8);
        }
    }
    catch (const std::bad_alloc&)
    {
        error = MakeBadAllocError("read SQLite table names", sqlUtf8);
        impl_->SetLastError(error);
        outTableNames.clear();
        return false;
    }
    catch (...)
    {
        error = MakeExceptionError("read SQLite table names", sqlUtf8);
        impl_->SetLastError(error);
        outTableNames.clear();
        return false;
    }

    if (impl_)
    {
        impl_->ClearLastError();
    }
    return true;
}

bool GB_Sqlite::TableExists(const std::string& tableNameUtf8, bool& outExists, bool includeSystemTables, const std::string& schemaNameUtf8) const
{
    outExists = false;

    GB_SqliteError error;
    if (!ValidateSqlIdentifier(tableNameUtf8, "table name", error))
    {
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    std::string schemaObjectNameUtf8;
    if (!BuildQualifiedSchemaObjectName(schemaNameUtf8, "sqlite_schema", schemaObjectNameUtf8, error))
    {
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    const std::string sqlUtf8 = "SELECT 1 FROM " + schemaObjectNameUtf8 + " WHERE type = 'table' AND name = :tableName AND (:includeSystemTables != 0 OR name NOT LIKE 'sqlite\\_%' ESCAPE '\\') LIMIT 1";
    GB_SqliteNamedParameters parameters;
    parameters["tableName"] = GB_Variant(tableNameUtf8);
    parameters["includeSystemTables"] = GB_Variant(includeSystemTables ? 1 : 0);

    GB_Variant value;
    if (!ExecuteScalarNamed(sqlUtf8, parameters, value))
    {
        return false;
    }

    outExists = !value.IsEmpty() && value.Type() != GB_VariantType::Empty;
    if (impl_)
    {
        impl_->ClearLastError();
    }
    return true;
}

bool GB_Sqlite::GetTableFieldInfos(const std::string& tableNameUtf8, std::vector<GB_SqliteTableFieldInfo>& outFieldInfos, bool includeHiddenFields, const std::string& schemaNameUtf8) const
{
    outFieldInfos.clear();

    GB_SqliteError error;
    if (!ValidateSqlIdentifier(tableNameUtf8, "table name", error))
    {
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    const std::string normalizedSchemaNameUtf8 = NormalizeSchemaName(schemaNameUtf8);
    if (!ValidateSqlIdentifier(normalizedSchemaNameUtf8, "schema name", error))
    {
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    bool tableExists = false;
    if (!TableExists(tableNameUtf8, tableExists, true, normalizedSchemaNameUtf8))
    {
        return false;
    }

    if (!tableExists)
    {
        error = MakeUserError("SQLite table does not exist: " + tableNameUtf8);
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    const std::string sqlUtf8 = "SELECT cid, name, type, \"notnull\", dflt_value, pk, hidden FROM pragma_table_xinfo(:tableName, :schemaName) WHERE (:includeHiddenFields != 0 OR hidden = 0) ORDER BY cid";
    GB_SqliteNamedParameters parameters;
    parameters["tableName"] = GB_Variant(tableNameUtf8);
    parameters["schemaName"] = GB_Variant(normalizedSchemaNameUtf8);
    parameters["includeHiddenFields"] = GB_Variant(includeHiddenFields ? 1 : 0);

    GB_SqliteResult result;
    if (!QueryNamed(sqlUtf8, parameters, result))
    {
        return false;
    }

    try
    {
        outFieldInfos.reserve(result.RowCount());
        for (std::size_t rowIndex = 0; rowIndex < result.RowCount(); rowIndex++)
        {
            GB_SqliteTableFieldInfo fieldInfo;
            if (!VariantToInt(result.GetValue(rowIndex, "cid"), fieldInfo.cid))
            {
                error = MakeUserError("Failed to read SQLite table field cid.", sqlUtf8);
                impl_->SetLastError(error);
                outFieldInfos.clear();
                return false;
            }

            if (!VariantToString(result.GetValue(rowIndex, "name"), fieldInfo.nameUtf8))
            {
                error = MakeUserError("Failed to read SQLite table field name.", sqlUtf8);
                impl_->SetLastError(error);
                outFieldInfos.clear();
                return false;
            }

            (void)VariantToString(result.GetValue(rowIndex, "type"), fieldInfo.typeUtf8);

            int notNullValue = 0;
            if (!VariantToInt(result.GetValue(rowIndex, "notnull"), notNullValue))
            {
                error = MakeUserError("Failed to read SQLite table field notnull flag.", sqlUtf8);
                impl_->SetLastError(error);
                outFieldInfos.clear();
                return false;
            }
            fieldInfo.notNull = notNullValue != 0;

            const GB_Variant defaultValue = result.GetValue(rowIndex, "dflt_value");
            fieldInfo.hasDefaultValue = !defaultValue.IsEmpty() && defaultValue.Type() != GB_VariantType::Empty;
            if (fieldInfo.hasDefaultValue)
            {
                (void)VariantToString(defaultValue, fieldInfo.defaultValueUtf8);
            }

            if (!VariantToInt(result.GetValue(rowIndex, "pk"), fieldInfo.primaryKeyIndex))
            {
                error = MakeUserError("Failed to read SQLite table field primary key index.", sqlUtf8);
                impl_->SetLastError(error);
                outFieldInfos.clear();
                return false;
            }

            if (!VariantToInt(result.GetValue(rowIndex, "hidden"), fieldInfo.hidden))
            {
                error = MakeUserError("Failed to read SQLite table field hidden flag.", sqlUtf8);
                impl_->SetLastError(error);
                outFieldInfos.clear();
                return false;
            }

            outFieldInfos.push_back(std::move(fieldInfo));
        }
    }
    catch (const std::bad_alloc&)
    {
        error = MakeBadAllocError("read SQLite table field infos", sqlUtf8);
        impl_->SetLastError(error);
        outFieldInfos.clear();
        return false;
    }
    catch (...)
    {
        error = MakeExceptionError("read SQLite table field infos", sqlUtf8);
        impl_->SetLastError(error);
        outFieldInfos.clear();
        return false;
    }

    if (impl_)
    {
        impl_->ClearLastError();
    }
    return true;
}

bool GB_Sqlite::GetTableData(const std::string& tableNameUtf8, GB_SqliteResult& outResult, std::size_t maxRowCount, const std::string& schemaNameUtf8) const
{
    outResult.Clear();

    GB_SqliteError error;
    std::string qualifiedTableNameUtf8;
    if (!BuildQualifiedTableName(schemaNameUtf8, tableNameUtf8, qualifiedTableNameUtf8, error))
    {
        outResult.error = error;
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    std::string sqlUtf8 = "SELECT * FROM " + qualifiedTableNameUtf8;
    if (maxRowCount > 0 && maxRowCount <= static_cast<std::size_t>(std::numeric_limits<long long>::max()))
    {
        sqlUtf8 += " LIMIT " + ToString(static_cast<long long>(maxRowCount));
    }

    return Query(sqlUtf8, outResult, maxRowCount);
}

bool GB_Sqlite::GetTableRowCount(const std::string& tableNameUtf8, long long& outRowCount, const std::string& schemaNameUtf8) const
{
    outRowCount = 0;

    GB_SqliteError error;
    std::string qualifiedTableNameUtf8;
    if (!BuildQualifiedTableName(schemaNameUtf8, tableNameUtf8, qualifiedTableNameUtf8, error))
    {
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    const std::string sqlUtf8 = "SELECT COUNT(*) FROM " + qualifiedTableNameUtf8;
    GB_Variant value;
    if (!ExecuteScalar(sqlUtf8, value))
    {
        return false;
    }

    if (!VariantToInt64(value, outRowCount))
    {
        error = MakeUserError("Failed to read SQLite table row count.", sqlUtf8);
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        outRowCount = 0;
        return false;
    }

    if (impl_)
    {
        impl_->ClearLastError();
    }
    return true;
}

bool GB_Sqlite::TableRowExists(const std::string& tableNameUtf8, const GB_SqliteNamedParameters& equalFieldValues, bool& outExists, const std::string& schemaNameUtf8) const
{
    outExists = false;

    GB_SqliteError error;
    std::string qualifiedTableNameUtf8;
    if (!BuildQualifiedTableName(schemaNameUtf8, tableNameUtf8, qualifiedTableNameUtf8, error))
    {
        if (impl_)
        {
            impl_->SetLastError(error);
        }
        return false;
    }

    std::string sqlUtf8 = "SELECT 1 FROM " + qualifiedTableNameUtf8;
    GB_SqliteNamedParameters parameters;

    if (!equalFieldValues.empty())
    {
        sqlUtf8 += " WHERE ";
        std::size_t conditionIndex = 0;
        for (GB_SqliteNamedParameters::const_iterator iter = equalFieldValues.begin(); iter != equalFieldValues.end(); iter++)
        {
            if (!ValidateSqlIdentifier(iter->first, "field name", error))
            {
                if (impl_)
                {
                    impl_->SetLastError(error);
                }
                return false;
            }

            std::ostringstream parameterNameStream;
            parameterNameStream << "gbRowValue" << conditionIndex;
            const std::string parameterNameUtf8 = parameterNameStream.str();

            if (conditionIndex > 0)
            {
                sqlUtf8 += " AND ";
            }

            sqlUtf8 += QuoteSqlIdentifierUnchecked(iter->first) + " IS :" + parameterNameUtf8;
            parameters[parameterNameUtf8] = iter->second;
            conditionIndex++;
        }
    }

    sqlUtf8 += " LIMIT 1";

    GB_Variant value;
    if (!ExecuteScalarNamed(sqlUtf8, parameters, value))
    {
        return false;
    }

    outExists = !value.IsEmpty() && value.Type() != GB_VariantType::Empty;
    if (impl_)
    {
        impl_->ClearLastError();
    }
    return true;
}

bool GB_Sqlite::AttachDatabase(const std::string& databasePathUtf8, const std::string& schemaNameUtf8)
{
    if (!impl_)
    {
        return false;
    }

    GB_SqliteError error;
    const std::string normalizedSchemaNameUtf8 = NormalizeSchemaName(schemaNameUtf8);
    if (databasePathUtf8.empty())
    {
        impl_->SetLastError(MakeUserError("SQLite attached database path is empty."));
        return false;
    }

    if (ContainsNullCharacter(databasePathUtf8))
    {
        impl_->SetLastError(MakeUserError("SQLite attached database path contains null character."));
        return false;
    }

    if (!ValidateSqlIdentifier(normalizedSchemaNameUtf8, "schema name", error))
    {
        impl_->SetLastError(error);
        return false;
    }

    if (IsMainOrTempSchemaName(normalizedSchemaNameUtf8))
    {
        impl_->SetLastError(MakeUserError("SQLite attached schema name cannot be main or temp."));
        return false;
    }

    const std::string sqlUtf8 = "ATTACH DATABASE " + QuoteSqlStringLiteralUnchecked(databasePathUtf8) + " AS " + QuoteSqlIdentifierUnchecked(normalizedSchemaNameUtf8);

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open.", sqlUtf8));
        return false;
    }

    GB_WriteLockGuard schemaGuard(impl_->schemaLock);
    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    std::lock_guard<std::mutex> readLock(impl_->readPoolMutex);

    const auto clearStatementCaches = [this]()
        {
            if (impl_->writeConnection)
            {
                impl_->writeConnection->statementCache.Clear();
            }

            for (std::size_t index = 0; index < impl_->readConnections.size(); index++)
            {
                if (impl_->readConnections[index])
                {
                    impl_->readConnections[index]->statementCache.Clear();
                }
            }

            impl_->pendingReadStatementCacheClear = false;
        };

    clearStatementCaches();

    std::vector<GB_SqliteConnection*> attachedConnections;
    attachedConnections.reserve(impl_->readConnections.size() + 1);

    if (!DirectExec(impl_->writeConnection->database, sqlUtf8, error))
    {
        impl_->SetLastError(error);
        return false;
    }
    attachedConnections.push_back(impl_->writeConnection.get());

    for (std::size_t index = 0; index < impl_->readConnections.size(); index++)
    {
        if (!impl_->readConnections[index] || impl_->readConnections[index]->database == nullptr)
        {
            continue;
        }

        if (!DirectExec(impl_->readConnections[index]->database, sqlUtf8, error))
        {
            const std::string detachSqlUtf8 = "DETACH DATABASE " + QuoteSqlIdentifierUnchecked(normalizedSchemaNameUtf8);
            for (std::size_t attachedIndex = 0; attachedIndex < attachedConnections.size(); attachedIndex++)
            {
                GB_SqliteError ignoredError;
                (void)DirectExec(attachedConnections[attachedIndex]->database, detachSqlUtf8, ignoredError);
            }

            clearStatementCaches();
            impl_->SetLastError(error);
            return false;
        }

        attachedConnections.push_back(impl_->readConnections[index].get());
    }

    clearStatementCaches();
    impl_->ClearLastError();
    return true;
}

bool GB_Sqlite::DetachDatabase(const std::string& schemaNameUtf8)
{
    if (!impl_)
    {
        return false;
    }

    GB_SqliteError error;
    const std::string normalizedSchemaNameUtf8 = NormalizeSchemaName(schemaNameUtf8);
    if (!ValidateSqlIdentifier(normalizedSchemaNameUtf8, "schema name", error))
    {
        impl_->SetLastError(error);
        return false;
    }

    if (IsMainOrTempSchemaName(normalizedSchemaNameUtf8))
    {
        impl_->SetLastError(MakeUserError("SQLite detached schema name cannot be main or temp."));
        return false;
    }

    const std::string sqlUtf8 = "DETACH DATABASE " + QuoteSqlIdentifierUnchecked(normalizedSchemaNameUtf8);

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open.", sqlUtf8));
        return false;
    }

    GB_WriteLockGuard schemaGuard(impl_->schemaLock);
    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    std::lock_guard<std::mutex> readLock(impl_->readPoolMutex);

    const auto clearStatementCaches = [this]()
        {
            if (impl_->writeConnection)
            {
                impl_->writeConnection->statementCache.Clear();
            }

            for (std::size_t index = 0; index < impl_->readConnections.size(); index++)
            {
                if (impl_->readConnections[index])
                {
                    impl_->readConnections[index]->statementCache.Clear();
                }
            }

            impl_->pendingReadStatementCacheClear = false;
        };

    clearStatementCaches();

    if (!DirectExec(impl_->writeConnection->database, sqlUtf8, error))
    {
        impl_->SetLastError(error);
        return false;
    }

    for (std::size_t index = 0; index < impl_->readConnections.size(); index++)
    {
        if (!impl_->readConnections[index] || impl_->readConnections[index]->database == nullptr)
        {
            continue;
        }

        if (!DirectExec(impl_->readConnections[index]->database, sqlUtf8, error))
        {
            clearStatementCaches();
            impl_->SetLastError(error);
            return false;
        }
    }

    clearStatementCaches();
    impl_->ClearLastError();
    return true;
}

GB_SqliteTransaction GB_Sqlite::BeginTransaction(GB_SqliteTransactionMode transactionMode)
{
    return GB_SqliteTransaction(*this, transactionMode);
}

bool GB_Sqlite::ExecuteInTransaction(const std::function<bool(GB_SqliteTransaction& transaction)>& transactionFunc, GB_SqliteTransactionMode transactionMode)
{
    if (!transactionFunc)
    {
        if (impl_)
        {
            impl_->SetLastError(MakeUserError("SQLite transaction callback is empty."));
        }
        return false;
    }

    GB_SqliteTransaction transaction = BeginTransaction(transactionMode);
    if (!transaction.IsActive())
    {
        return false;
    }

    bool ok = false;
    try
    {
        ok = transactionFunc(transaction);
    }
    catch (...)
    {
        (void)transaction.Rollback();
        if (impl_)
        {
            impl_->SetLastError(MakeUserError("SQLite transaction callback threw an exception."));
        }
        return false;
    }

    if (!ok)
    {
        const GB_SqliteError callbackError = transaction.GetLastError();
        (void)transaction.Rollback();
        if (impl_)
        {
            impl_->SetLastError(callbackError.IsOk() ? MakeUserError("SQLite transaction callback returned false.") : callbackError);
        }
        return false;
    }

    return transaction.Commit();
}

long long GB_Sqlite::GetLastInsertRowId() const
{
    if (!impl_)
    {
        return 0;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        return 0;
    }

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    return static_cast<long long>(sqlite3_last_insert_rowid(impl_->writeConnection->database));
}

int GB_Sqlite::GetChanges() const
{
    if (!impl_)
    {
        return 0;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        return 0;
    }

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    return sqlite3_changes(impl_->writeConnection->database);
}

int GB_Sqlite::GetTotalChanges() const
{
    if (!impl_)
    {
        return 0;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        return 0;
    }

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    return sqlite3_total_changes(impl_->writeConnection->database);
}

bool GB_Sqlite::SetBusyTimeout(int busyTimeoutMs)
{
    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open."));
        return false;
    }

    if (busyTimeoutMs < 0)
    {
        busyTimeoutMs = 0;
    }

    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        const int code = sqlite3_busy_timeout(impl_->writeConnection->database, busyTimeoutMs);
        if (code != SQLITE_OK)
        {
            impl_->SetLastError(MakeError(impl_->writeConnection->database, code, std::string()));
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> readLock(impl_->readPoolMutex);
        for (std::size_t index = 0; index < impl_->availableReadConnectionIndexes.size(); index++)
        {
            const std::size_t connectionIndex = impl_->availableReadConnectionIndexes[index];
            if (connectionIndex >= impl_->readConnections.size() || !impl_->readConnections[connectionIndex] || impl_->readConnections[connectionIndex]->database == nullptr)
            {
                continue;
            }

            const int code = sqlite3_busy_timeout(impl_->readConnections[connectionIndex]->database, busyTimeoutMs);
            if (code != SQLITE_OK)
            {
                impl_->SetLastError(MakeError(impl_->readConnections[connectionIndex]->database, code, std::string()));
                return false;
            }
        }

        impl_->pendingReadConnectionBusyTimeoutMs = impl_->availableReadConnectionIndexes.size() == impl_->readConnections.size() ? -1 : busyTimeoutMs;
    }

    impl_->options.busyTimeoutMs = busyTimeoutMs;
    impl_->ClearLastError();
    return true;
}

bool GB_Sqlite::CheckpointWal(bool truncate)
{
    return CheckpointWal(nullptr, truncate);
}

bool GB_Sqlite::CheckpointWal(GB_SqliteCheckpointResult* outCheckpointResult, bool truncate)
{
    if (outCheckpointResult != nullptr)
    {
        *outCheckpointResult = GB_SqliteCheckpointResult();
    }

    if (!impl_)
    {
        return false;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen || !impl_->writeConnection)
    {
        impl_->SetLastError(MakeUserError("SQLite database is not open."));
        return false;
    }

    std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
    if (!impl_->CanWrite())
    {
        impl_->SetLastError(MakeUserError("SQLite database is readonly."));
        return false;
    }

    int logFrameCount = -1;
    int checkpointedFrameCount = -1;
    const int mode = truncate ? SQLITE_CHECKPOINT_TRUNCATE : SQLITE_CHECKPOINT_PASSIVE;
    const int code = sqlite3_wal_checkpoint_v2(impl_->writeConnection->database, nullptr, mode, &logFrameCount, &checkpointedFrameCount);
    if (outCheckpointResult != nullptr)
    {
        outCheckpointResult->logFrameCount = logFrameCount;
        outCheckpointResult->checkpointedFrameCount = checkpointedFrameCount;
    }

    if (code != SQLITE_OK)
    {
        impl_->SetLastError(MakeError(impl_->writeConnection->database, code, "wal_checkpoint"));
        return false;
    }

    impl_->ClearLastError();
    return true;
}

void GB_Sqlite::ClearStatementCache()
{
    if (!impl_)
    {
        return;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen)
    {
        return;
    }

    {
        GB_WriteLockGuard schemaGuard(impl_->schemaLock);
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        impl_->ClearAllStatementCachesWithWriteLockHeld();
    }
}

GB_SqliteStatementCacheStats GB_Sqlite::GetStatementCacheStats() const
{
    GB_SqliteStatementCacheStats stats;
    if (!impl_)
    {
        return stats;
    }

    GB_ReadLockGuard lifecycleGuard(impl_->lifecycleLock);
    if (!impl_->isOpen)
    {
        return stats;
    }

    {
        std::lock_guard<std::mutex> writeLock(impl_->writeMutex);
        if (impl_->writeConnection)
        {
            stats.connectionCount++;
            stats.cachedStatementCount += impl_->writeConnection->statementCache.Size();
            stats.hits += impl_->writeConnection->statementCache.Hits();
            stats.misses += impl_->writeConnection->statementCache.Misses();
            stats.evictions += impl_->writeConnection->statementCache.Evictions();
        }
    }

    {
        std::lock_guard<std::mutex> readLock(impl_->readPoolMutex);
        stats.connectionCount += impl_->readConnections.size();
        for (std::size_t index = 0; index < impl_->availableReadConnectionIndexes.size(); index++)
        {
            const std::size_t connectionIndex = impl_->availableReadConnectionIndexes[index];
            if (connectionIndex >= impl_->readConnections.size() || !impl_->readConnections[connectionIndex])
            {
                continue;
            }

            stats.cachedStatementCount += impl_->readConnections[connectionIndex]->statementCache.Size();
            stats.hits += impl_->readConnections[connectionIndex]->statementCache.Hits();
            stats.misses += impl_->readConnections[connectionIndex]->statementCache.Misses();
            stats.evictions += impl_->readConnections[connectionIndex]->statementCache.Evictions();
        }
    }

    return stats;
}

GB_SqliteTransaction::GB_SqliteTransaction() : impl_(new Impl())
{
}

GB_SqliteTransaction::GB_SqliteTransaction(GB_Sqlite& database, GB_SqliteTransactionMode transactionMode) : impl_(new Impl())
{
    (void)impl_->Begin(database, transactionMode);
}

GB_SqliteTransaction::~GB_SqliteTransaction()
{
}

GB_SqliteTransaction::GB_SqliteTransaction(GB_SqliteTransaction&& other) noexcept : impl_(std::move(other.impl_))
{
    if (!impl_)
    {
        impl_.reset(new Impl());
    }
}

GB_SqliteTransaction& GB_SqliteTransaction::operator=(GB_SqliteTransaction&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    impl_.reset();
    impl_ = std::move(other.impl_);
    if (!impl_)
    {
        impl_.reset(new Impl());
    }
    return *this;
}

bool GB_SqliteTransaction::IsActive() const
{
    return impl_ && impl_->active;
}

GB_SqliteError GB_SqliteTransaction::GetLastError() const
{
    if (!impl_)
    {
        return MakeUserError("GB_SqliteTransaction object is empty.");
    }

    return impl_->lastError;
}

bool GB_SqliteTransaction::Execute(const std::string& sqlUtf8)
{
    return Execute(sqlUtf8, GB_SqliteParameterList());
}

bool GB_SqliteTransaction::Execute(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters)
{
    if (!impl_ || !impl_->active || impl_->owner == nullptr || !impl_->owner->writeConnection)
    {
        if (impl_)
        {
            impl_->SetError(MakeUserError("SQLite transaction is not active.", sqlUtf8));
        }
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->owner->ExecuteOnConnection(*impl_->owner->writeConnection, sqlUtf8, &parameters, nullptr, error);
    if (ok && IsProbablySchemaChangingSql(sqlUtf8))
    {
        impl_->schemaChanged = true;
    }
    impl_->SetError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_SqliteTransaction::ExecuteNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters)
{
    if (!impl_ || !impl_->active || impl_->owner == nullptr || !impl_->owner->writeConnection)
    {
        if (impl_)
        {
            impl_->SetError(MakeUserError("SQLite transaction is not active.", sqlUtf8));
        }
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->owner->ExecuteOnConnection(*impl_->owner->writeConnection, sqlUtf8, nullptr, &parameters, error);
    if (ok && IsProbablySchemaChangingSql(sqlUtf8))
    {
        impl_->schemaChanged = true;
    }
    impl_->SetError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_SqliteTransaction::Query(const std::string& sqlUtf8, GB_SqliteResult& outResult, std::size_t maxRowCount)
{
    return Query(sqlUtf8, GB_SqliteParameterList(), outResult, maxRowCount);
}

bool GB_SqliteTransaction::Query(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount)
{
    outResult.Clear();
    if (!impl_ || !impl_->active || impl_->owner == nullptr || !impl_->owner->writeConnection)
    {
        outResult.error = MakeUserError("SQLite transaction is not active.", sqlUtf8);
        if (impl_)
        {
            impl_->SetError(outResult.error);
        }
        return false;
    }

    const bool ok = impl_->owner->QueryOnConnection(*impl_->owner->writeConnection, sqlUtf8, &parameters, nullptr, outResult, maxRowCount, false);
    if (ok && IsProbablySchemaChangingSql(sqlUtf8))
    {
        impl_->schemaChanged = true;
    }
    impl_->SetError(ok ? MakeOkError() : outResult.error);
    return ok;
}

bool GB_SqliteTransaction::QueryNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount)
{
    outResult.Clear();
    if (!impl_ || !impl_->active || impl_->owner == nullptr || !impl_->owner->writeConnection)
    {
        outResult.error = MakeUserError("SQLite transaction is not active.", sqlUtf8);
        if (impl_)
        {
            impl_->SetError(outResult.error);
        }
        return false;
    }

    const bool ok = impl_->owner->QueryOnConnection(*impl_->owner->writeConnection, sqlUtf8, nullptr, &parameters, outResult, maxRowCount, false);
    if (ok && IsProbablySchemaChangingSql(sqlUtf8))
    {
        impl_->schemaChanged = true;
    }
    impl_->SetError(ok ? MakeOkError() : outResult.error);
    return ok;
}

bool GB_SqliteTransaction::ExecuteScalar(const std::string& sqlUtf8, GB_Variant& outValue)
{
    return ExecuteScalar(sqlUtf8, GB_SqliteParameterList(), outValue);
}

bool GB_SqliteTransaction::ExecuteScalar(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_Variant& outValue)
{
    outValue.Reset();
    if (!impl_ || !impl_->active || impl_->owner == nullptr || !impl_->owner->writeConnection)
    {
        if (impl_)
        {
            impl_->SetError(MakeUserError("SQLite transaction is not active.", sqlUtf8));
        }
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->owner->ExecuteScalarOnConnection(*impl_->owner->writeConnection, sqlUtf8, &parameters, nullptr, outValue, error, false);
    if (ok && IsProbablySchemaChangingSql(sqlUtf8))
    {
        impl_->schemaChanged = true;
    }
    impl_->SetError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_SqliteTransaction::ExecuteScalarNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_Variant& outValue)
{
    outValue.Reset();
    if (!impl_ || !impl_->active || impl_->owner == nullptr || !impl_->owner->writeConnection)
    {
        if (impl_)
        {
            impl_->SetError(MakeUserError("SQLite transaction is not active.", sqlUtf8));
        }
        return false;
    }

    GB_SqliteError error;
    const bool ok = impl_->owner->ExecuteScalarOnConnection(*impl_->owner->writeConnection, sqlUtf8, nullptr, &parameters, outValue, error, false);
    if (ok && IsProbablySchemaChangingSql(sqlUtf8))
    {
        impl_->schemaChanged = true;
    }
    impl_->SetError(ok ? MakeOkError() : error);
    return ok;
}

bool GB_SqliteTransaction::Commit()
{
    if (!impl_)
    {
        return false;
    }

    return impl_->Commit();
}

bool GB_SqliteTransaction::Rollback()
{
    if (!impl_)
    {
        return false;
    }

    return impl_->Rollback();
}
