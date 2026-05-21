#ifndef GLOBALBASE_SQLITE_H_H
#define GLOBALBASE_SQLITE_H_H

#include "GB_BaseTypes.h"
#include "GB_Variant.h"
#include "GlobalBasePort.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief SQLite 数据库打开模式。
 *
 * @details
 * 本枚举用于描述 GB_Sqlite::Open() 或 GB_Sqlite 构造函数打开数据库文件时的权限语义。
 * 其含义最终会映射到 sqlite3_open_v2() 的打开标志。
 */
enum class GB_SqliteOpenMode
{
    /**
     * @brief 只读打开。
     *
     * @remarks
     * 适合纯查询场景。只读模式下，本模块不会主动切换 WAL 日志模式，也不会执行需要写权限的 PRAGMA。
     */
    ReadOnly = 0,

    /**
     * @brief 读写打开，但不自动创建数据库文件。
     *
     * @remarks
     * 若目标数据库文件不存在，Open() 会失败。
     */
    ReadWrite,

    /**
     * @brief 读写打开，并在数据库文件不存在时自动创建。
     *
     * @remarks
     * 这是 GB_SqliteOptions 的默认打开模式，适合大多数本地工程数据库场景。
     */
    ReadWriteCreate
};

/**
 * @brief SQLite synchronous 刷盘策略。
 *
 * @details
 * synchronous 会影响 SQLite 在事务提交、WAL checkpoint、回滚日志同步等环节的磁盘同步强度。
 * 不同策略在写入性能和断电一致性之间取舍不同。
 *
 * @remarks
 * - Normal 通常适合 WAL 模式下的工程应用，能在可靠性和性能之间取得较好平衡。
 * - Full / Extra 更偏重断电一致性，代价是写入性能下降。
 * - Off 性能最高，但异常断电时风险较高，底层基础库默认不使用。
 */
enum class GB_SqliteSynchronousMode
{
    /**
     * @brief 关闭大部分同步刷盘。
     *
     * @warning
     * 该模式写入性能较高，但系统崩溃或断电时更容易造成数据库损坏或已提交数据丢失。
     */
    Off = 0,

    /**
     * @brief 普通同步策略。
     *
     * @remarks
     * WAL 模式下常用的折中方案。对桌面端缓存、索引、工程辅助数据库等场景通常较合适。
     */
    Normal,

    /**
     * @brief 完整同步策略。
     *
     * @remarks
     * 比 Normal 更保守，写入性能低一些，但断电一致性更强。
     */
    Full,

    /**
     * @brief 额外同步策略。
     *
     * @remarks
     * SQLite 提供的更严格策略之一。仅在对异常断电一致性要求很高且能接受性能成本时使用。
     */
    Extra
};

/**
 * @brief SQLite 事务开启模式。
 *
 * @details
 * 本枚举对应 BEGIN DEFERRED / BEGIN IMMEDIATE / BEGIN EXCLUSIVE 三种事务启动语义。
 */
enum class GB_SqliteTransactionMode
{
    /**
     * @brief 延迟事务。
     *
     * @remarks
     * 事务开始时不立即获取写锁，直到第一次读写语句执行时再按需获取锁。
     */
    Deferred = 0,

    /**
     * @brief 立即事务。
     *
     * @remarks
     * 事务开始时立即尝试获取写锁。适合明确要写入的事务，能更早暴露写锁竞争。
     */
    Immediate,

    /**
     * @brief 排他事务。
     *
     * @remarks
     * 会尝试以更强的排他方式占用数据库。普通业务写入通常不需要该模式。
     */
    Exclusive
};

/**
 * @brief SQLite 单元格运行时值类型。
 *
 * @details
 * 用于描述 sqlite3_column_type() 返回的运行时存储类型，而不是字段声明类型。
 */
enum class GB_SqliteValueType
{
    /** @brief SQL NULL。 */
    Null = 0,

    /** @brief SQLite INTEGER，读取时映射为 long long。 */
    Integer,

    /** @brief SQLite REAL，读取时映射为 double。 */
    Float,

    /** @brief SQLite TEXT，读取时映射为 UTF-8 std::string。 */
    Text,

    /** @brief SQLite BLOB，读取时映射为 GB_ByteBuffer。 */
    Blob
};

/**
 * @brief SQLite 错误信息。
 *
 * @details
 * 用于保存最近一次失败操作的 SQLite 原始错误码、扩展错误码、错误消息以及相关 SQL。
 * 若 code 为 0，通常表示当前对象没有记录错误。
 */
struct GLOBALBASE_PORT GB_SqliteError
{
    /** @brief SQLite 基础错误码，例如 SQLITE_OK、SQLITE_BUSY、SQLITE_CONSTRAINT 等。 */
    int code = 0;

    /** @brief SQLite 扩展错误码。若底层未提供扩展错误码，通常与 code 相同或为 0。 */
    int extendedCode = 0;

    /** @brief UTF-8 错误描述。通常来自 sqlite3_errmsg() 或模块内部校验逻辑。 */
    std::string messageUtf8;

    /** @brief 触发错误的 SQL 文本。对于非 SQL 操作或未知 SQL，该字段可能为空。 */
    std::string sqlUtf8;

    /**
     * @brief 判断当前错误对象是否表示成功状态。
     *
     * @return true 没有错误；
     * @return false 当前对象保存了错误信息。
     */
    bool IsOk() const;

    /**
     * @brief 清空错误码、扩展错误码、错误消息和 SQL 文本。
     */
    void Clear();
};

/**
 * @brief SQLite 数据库配置。
 *
 * @details
 * GB_SqliteOptions 在 Open() 时被读取，并影响所有内部 SQLite 连接的创建、PRAGMA 初始化、语句缓存、连接池规模等行为。
 * Open() 成功后再修改外部 options 对象不会影响已打开的数据库；需要重新配置时，应 Close() 后重新 Open()。
 */
struct GLOBALBASE_PORT GB_SqliteOptions
{
    /** @brief 数据库打开模式。默认读写并自动创建文件。 */
    GB_SqliteOpenMode openMode = GB_SqliteOpenMode::ReadWriteCreate;

    /**
     * @brief 是否启用 WAL 日志模式。
     *
     * @remarks
     * 只读打开时不会主动切换 WAL。内存数据库不支持真正意义上的 WAL，本模块会自动跳过。
     */
    bool enableWal = true;

    /** @brief 是否启用 foreign_keys 约束。建议保持 true，避免外键约束静默失效。 */
    bool enableForeignKeys = true;

    /**
     * @brief 是否让只读连接设置 query_only，防止误写。
     *
     * @remarks
     * 内部读连接在可写数据库上可能使用 READWRITE 方式打开，以避免 WAL 辅助文件尚未创建时只读连接打开失败。
     * query_only 可进一步限制这些连接不执行写操作。
     */
    bool enableQueryOnlyForReadConnections = true;

    /**
     * @brief 忙等待超时，单位毫秒。
     *
     * @remarks
     * 遇到锁冲突时，SQLite 会在该时间内重试；0 表示不等待并立即返回 SQLITE_BUSY 或相关错误。
     */
    int busyTimeoutMs = 5000;

    /** @brief synchronous 刷盘策略。默认 Normal。 */
    GB_SqliteSynchronousMode synchronousMode = GB_SqliteSynchronousMode::Normal;

    /**
     * @brief 读连接数量。
     *
     * @remarks
     * 0 表示由内部根据硬件线程数选择，当前限制在合理范围内。内存数据库会退化为单连接读写，以避免多个 :memory: 连接互不可见。
     */
    std::size_t readConnectionCount = 0;

    /** @brief 是否启用预编译语句缓存。 */
    bool enableStatementCache = true;

    /**
     * @brief 每条 SQLite 连接最多缓存多少条预编译语句。
     *
     * @remarks
     * 0 表示不缓存。缓存适合重复执行的短 SQL，可减少 sqlite3_prepare_v2() 成本。
     */
    std::size_t maxCachedStatementsPerConnection = 64;

    /**
     * @brief WAL 自动 checkpoint 页数。
     *
     * @remarks
     * 0 表示不主动设置，使用 SQLite 默认值。仅在 WAL 可用且数据库可写时生效。
     */
    int walAutoCheckpointPages = 1000;

    /**
     * @brief SQLite cache_size，单位 KB。
     *
     * @remarks
     * 0 表示不主动设置。该参数主要影响每个连接的页面缓存规模。
     */
    int cacheSizeKb = 0;

    /**
     * @brief 是否允许 SQLite 按 URI 语义解析 file: 开头的数据库路径。
     *
     * @remarks
     * 需要使用 file:xxx?mode=ro、file::memory:?cache=shared 等 URI 形式时应保持 true。
     */
    bool enableUri = true;

    /**
     * @brief 是否使用 SQLite 连接级 FULLMUTEX。
     *
     * @remarks
     * false 时使用 NOMUTEX。本类仍会通过连接池租借、写连接互斥等机制保证同一 SQLite 连接不会被多个线程同时使用。
     * 若上层存在非预期跨线程共享底层连接的风险，可设为 true 换取更保守的 SQLite 内部互斥保护。
     */
    bool useFullMutex = false;

    /** @brief 是否把 SQLite 临时表、临时索引等中间数据优先放在内存中。 */
    bool enableTempStoreMemory = false;

    /**
     * @brief 是否拒绝命名参数列表中未被 SQL 实际使用的参数。
     *
     * @remarks
     * 建议保持 true，以便尽早发现参数名拼写错误或 SQL 与参数对象不匹配的问题。
     */
    bool rejectUnusedNamedParameters = true;

    /**
     * @brief 允许被语句缓存接管的最大 SQL 字节数。
     *
     * @remarks
     * 0 表示不按 SQL 长度限制缓存。该限制可避免超长一次性 SQL 长期占用 LRU 缓存空间。
     */
    std::size_t maxCachedStatementSqlByteCount = 64 * 1024;

    /**
     * @brief SQLite mmap_size，单位字节。
     *
     * @remarks
     * 0 表示不主动设置。该值会在连接初始化阶段设置，适合对大量只读扫描或索引读取进行性能调优。
     */
    long long memoryMapSizeBytes = 0;
};

/**
 * @brief 查询结果列信息。
 *
 * @details
 * 该结构保存结果集某一列的名称、声明类型以及来源数据库、表和原始列名。
 * 其中部分来源字段依赖 SQLite 编译选项 SQLITE_ENABLE_COLUMN_METADATA；若底层 SQLite 未启用相关能力，字段可能为空。
 */
struct GLOBALBASE_PORT GB_SqliteColumnInfo
{
    /** @brief 结果列名称，通常来自 SELECT 列别名或表达式名称。 */
    std::string nameUtf8;

    /** @brief 字段声明类型，例如 INTEGER、TEXT、REAL。表达式列可能为空。 */
    std::string declaredTypeUtf8;

    /** @brief 来源数据库名称，例如 main、temp。不可取得时为空。 */
    std::string databaseNameUtf8;

    /** @brief 来源表名称。表达式列或不可取得时为空。 */
    std::string tableNameUtf8;

    /** @brief 来源字段原始名称。表达式列或不可取得时为空。 */
    std::string originNameUtf8;
};

/**
 * @brief SQLite 表字段信息。
 *
 * @details
 * 该结构用于描述数据库表中真实字段的定义信息，通常由 GetTableFieldInfos() 返回。
 * 与 GB_SqliteColumnInfo 不同，GB_SqliteColumnInfo 描述的是查询结果列；本结构描述的是表结构中的字段。
 */
struct GLOBALBASE_PORT GB_SqliteTableFieldInfo
{
    /** @brief 字段在当前表结构查询结果中的序号。 */
    int cid = -1;

    /** @brief 字段名称，UTF-8 编码。 */
    std::string nameUtf8;

    /** @brief 字段声明类型，例如 INTEGER、TEXT、REAL、BLOB。未声明时为空字符串。 */
    std::string typeUtf8;

    /** @brief 字段是否声明为 NOT NULL。 */
    bool notNull = false;

    /** @brief 字段是否存在默认值声明。 */
    bool hasDefaultValue = false;

    /** @brief 字段默认值表达式文本，UTF-8 编码。仅当 hasDefaultValue 为 true 时有效。 */
    std::string defaultValueUtf8;

    /** @brief 主键字段序号。0 表示不是主键字段；大于 0 表示在复合主键中的 1 基序号。 */
    int primaryKeyIndex = 0;

    /**
     * @brief SQLite table_xinfo hidden 标记。
     *
     * @remarks
     * 0 表示普通字段；1 通常表示虚表隐藏字段；2 或 3 表示生成字段。includeHiddenFields 为 false 时只返回 hidden == 0 的普通字段。
     */
    int hidden = 0;
};

/**
 * @brief SQLite 查询结果。
 *
 * @details
 * Query() 会将结果集完整读入 GB_SqliteResult；QueryEach() 则逐行回调，不强制把所有行保存在内存中。
 * 对于可能返回大量记录的查询，优先考虑 QueryEach() 或使用 maxRowCount 限制返回行数。
 *
 * @remarks
 * - 单元格值统一使用 GB_Variant 承载。
 * - SQL NULL 会被转换为空 GB_Variant。
 * - SQLite INTEGER / REAL / TEXT / BLOB 分别转换为 long long / double / std::string / GB_ByteBuffer。
 */

struct GLOBALBASE_PORT GB_SqliteResult
{
    /** @brief 查询结果列元信息，顺序与 SELECT 输出列顺序一致。 */
    std::vector<GB_SqliteColumnInfo> columns;

    /** @brief 查询结果行。rows[rowIndex][columnIndex] 为对应单元格值。 */
    std::vector<std::vector<GB_Variant>> rows;

    /** @brief 查询错误信息。成功时应为空错误。 */
    GB_SqliteError error;

    /**
     * @brief 判断结果对象是否处于成功状态。
     *
     * @return true 没有错误；
     * @return false 查询失败或结果对象保存了错误信息。
     */
    bool IsOk() const;

    /**
     * @brief 判断结果集中是否没有任何数据行。
     *
     * @return true 没有数据行；
     * @return false 至少存在一行数据。
     */
    bool IsEmpty() const;

    /** @brief 获取结果行数。 */
    std::size_t RowCount() const;

    /** @brief 获取结果列数。 */
    std::size_t ColumnCount() const;

    /**
     * @brief 根据列名获取列索引。
     *
     * @param columnNameUtf8 列名或别名，UTF-8 编码。
     * @return int 找到时返回从 0 开始的列索引；找不到时返回 -1。
     */
    int GetColumnIndex(const std::string& columnNameUtf8) const;

    /**
     * @brief 尝试根据行列索引读取单元格值。
     *
     * @param rowIndex 行索引，从 0 开始。
     * @param columnIndex 列索引，从 0 开始。
     * @param outValue 输出单元格值。
     * @return true 读取成功；
     * @return false 行列索引越界。
     */
    bool TryGetValue(std::size_t rowIndex, std::size_t columnIndex, GB_Variant& outValue) const;

    /**
     * @brief 尝试根据行索引和列名读取单元格值。
     *
     * @param rowIndex 行索引，从 0 开始。
     * @param columnNameUtf8 列名或别名，UTF-8 编码。
     * @param outValue 输出单元格值。
     * @return true 读取成功；
     * @return false 行索引越界或列名不存在。
     */
    bool TryGetValue(std::size_t rowIndex, const std::string& columnNameUtf8, GB_Variant& outValue) const;

    /**
     * @brief 根据行列索引读取单元格值。
     *
     * @param rowIndex 行索引，从 0 开始。
     * @param columnIndex 列索引，从 0 开始。
     * @return GB_Variant 读取成功时返回对应值；索引越界时返回空 GB_Variant。
     */
    GB_Variant GetValue(std::size_t rowIndex, std::size_t columnIndex) const;

    /**
     * @brief 根据行索引和列名读取单元格值。
     *
     * @param rowIndex 行索引，从 0 开始。
     * @param columnNameUtf8 列名或别名，UTF-8 编码。
     * @return GB_Variant 读取成功时返回对应值；索引越界或列名不存在时返回空 GB_Variant。
     */
    GB_Variant GetValue(std::size_t rowIndex, const std::string& columnNameUtf8) const;

    /** @brief 清空列信息、数据行和错误信息。 */
    void Clear();
};

/**
 * @brief SQLite WAL checkpoint 结果。
 *
 * @details
 * 由 CheckpointWal() 返回，用于判断 WAL 中有多少帧需要 checkpoint，以及实际完成了多少帧。
 */
struct GLOBALBASE_PORT GB_SqliteCheckpointResult
{
    /** @brief checkpoint 前 WAL 日志中的总帧数。-1 表示当前数据库不处于 WAL 模式或无法取得该值。 */
    int logFrameCount = -1;

    /** @brief 已 checkpoint 的总帧数。-1 表示当前数据库不处于 WAL 模式或无法取得该值。 */
    int checkpointedFrameCount = -1;

    /**
     * @brief 判断 checkpoint 是否完整完成。
     *
     * @return true logFrameCount 有效且 checkpointedFrameCount 不小于 logFrameCount；
     * @return false WAL 状态未知或仍有未完成 checkpoint 的帧。
     */
    bool IsComplete() const;
};

/**
 * @brief SQLite 语句缓存统计。
 *
 * @details
 * 统计信息用于观察预编译语句缓存是否有效，例如命中率是否足够高、是否频繁淘汰等。
 */
struct GLOBALBASE_PORT GB_SqliteStatementCacheStats
{
    /** @brief 参与统计的连接数量。 */
    std::size_t connectionCount = 0;

    /** @brief 当前缓存中的预编译语句总数。 */
    std::size_t cachedStatementCount = 0;

    /** @brief 语句缓存命中次数。 */
    std::uint64_t hits = 0;

    /** @brief 语句缓存未命中次数。 */
    std::uint64_t misses = 0;

    /** @brief 因 LRU 容量限制等原因被淘汰的缓存语句数量。 */
    std::uint64_t evictions = 0;
};

/**
 * @brief SQLite 位置参数列表。
 *
 * @details
 * 用于绑定 ? 或 ?NNN 参数。列表下标从 0 开始，对应 SQLite 参数索引从 1 开始。
 */
using GB_SqliteParameterList = std::vector<GB_Variant>;

/**
 * @brief SQLite 命名参数表。
 *
 * @details
 * 支持 SQLite 原生 :name、@name、$name、?NNN 形式。调用侧 key 可以带前缀，也可以省略前缀。
 * 若同一 SQL 中存在 :id 与 @id 这类去前缀后同名的参数，建议使用带前缀的精确 key。
 */
using GB_SqliteNamedParameters = std::map<std::string, GB_Variant>;

/**
 * @brief 逐行查询回调函数。
 *
 * @param columns 结果列信息。
 * @param values 当前行的单元格值。
 * @return true 继续读取下一行；
 * @return false 立即停止遍历，QueryEach() 会按成功停止处理。
 */
using GB_SqliteRowCallback = std::function<bool(const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values)>;

class GLOBALBASE_PORT GB_SqliteTransaction;

/**
 * @brief 通用 SQLite 读写器。
 *
 * @details
 * GB_Sqlite 是面向工程基础库的 SQLite C API 封装。它负责管理数据库连接、预编译语句、语句缓存、读连接池、写连接串行化、事务 RAII 和错误信息。
 * 默认结构为“一个写连接 + 多个读连接”。在 WAL 模式下，多个只读查询可并发租借不同读连接执行，写操作仍通过写连接串行化执行。
 *
 * 设计目标：
 * - RAII 管理 SQLite 数据库连接、预编译语句与事务生命周期。
 * - 默认使用“一个写连接 + 多个读连接”的连接池结构；配合 WAL 模式，提升多读一写场景下的并发度。
 * - 所有公开接口内部做同步保护，允许多个线程并发访问同一个 GB_Sqlite 对象。
 * - SQL 文本、数据库路径、TEXT 字段统一按 UTF-8 处理。
 * - 参数绑定统一使用 GB_Variant；二进制字段使用 GB_ByteBuffer。
 * - 命名参数支持 SQLite 原生 :name / @name / $name / ?NNN 形式；调用侧参数名可带前缀，也可省略前缀。
 *
 * 重要说明：
 * - SQLite 同一时刻仍只允许一个写事务真正写入；本类会串行化写操作。
 * - Execute() / Query() 等预编译接口只接受单条 SQL；多条 SQL 请使用 ExecuteBatch()。
 * - 事务内需要使用 GB_SqliteTransaction::Execute() / Query() 等接口，以确保读写都落在同一个写连接上。
 * - 显式事务会独占本类的 schema 保护锁，保证事务期间不会与读连接上的缓存语句发生 schema 变更竞争。
 * - 大量同构 INSERT / UPDATE / DELETE 建议使用 ExecuteMany()，内部复用同一条预编译语句并可自动包事务。
 * - Query() 只接受只读 SQL；写 SQL 请使用 Execute() 或事务接口。
 * - BEGIN / COMMIT / ROLLBACK / SAVEPOINT / RELEASE 等事务控制语句请使用事务接口，不建议直接通过 Execute() / Query() 发送。
 * - Query() 的只读判定用于拦截直接写库语句；若自定义 SQL 函数或虚表有副作用，调用者仍需自行约束。
 * - 若同一条 SQL 同时出现 :id / @id 等去前缀后同名的命名参数，调用侧应使用带前缀的精确参数名，避免歧义。
 * - QueryEach() 回调执行期间仍持有当前读连接，不建议在回调中递归调用同一个 GB_Sqlite 对象的写接口或可能修改 schema 的接口。
 */
class GLOBALBASE_PORT GB_Sqlite
{
public:
    /**
     * @brief 构造一个尚未打开的 SQLite 数据库对象。
     *
     * @remarks
     * 构造后需要显式调用 Open()。
     */
    GB_Sqlite();

    /**
     * @brief 构造并打开 SQLite 数据库。
     *
     * @param databasePathUtf8 数据库路径，UTF-8 编码。支持普通文件路径，也支持启用 URI 后的 file: URI。
     * @param options 数据库配置。
     *
     * @remarks
     * 若打开失败，可通过 IsOpen() 判断，并通过 GetLastError() 获取错误信息。
     */
    explicit GB_Sqlite(const std::string& databasePathUtf8, const GB_SqliteOptions& options = GB_SqliteOptions());

    /**
     * @brief 析构并自动关闭数据库连接。
     *
     * @remarks
     * 析构时会清理语句缓存、关闭所有内部连接。建议外部确保没有其他线程正在调用该对象。
     */
    ~GB_Sqlite();

    GB_Sqlite(const GB_Sqlite& other) = delete;
    GB_Sqlite& operator=(const GB_Sqlite& other) = delete;

    /**
     * @brief 移动构造。
     *
     * @param other 被移动的数据库对象。移动后 other 处于可析构、可重新 Open() 的状态。
     */
    GB_Sqlite(GB_Sqlite&& other) noexcept;

    /**
     * @brief 移动赋值。
     *
     * @param other 被移动的数据库对象。
     * @return GB_Sqlite& 当前对象引用。
     */
    GB_Sqlite& operator=(GB_Sqlite&& other) noexcept;

    /**
     * @brief 打开 SQLite 数据库。
     *
     * @param databasePathUtf8 数据库路径，UTF-8 编码。
     * @param options 数据库配置。
     * @return true 打开成功；
     * @return false 打开失败，可通过 GetLastError() 获取错误信息。
     *
     * @remarks
     * 若当前对象已经打开，Open() 会先 Close() 再重新打开。
     */
    bool Open(const std::string& databasePathUtf8, const GB_SqliteOptions& options = GB_SqliteOptions());

    /**
     * @brief 关闭数据库并释放所有内部资源。
     *
     * @remarks
     * 会清空语句缓存、关闭读连接池和写连接。Close() 后可再次 Open()。
     */
    void Close();

    /** @brief 判断数据库当前是否已成功打开。 */
    bool IsOpen() const;

    /** @brief 判断数据库是否以只读模式打开。未打开时返回 false。 */
    bool IsReadOnly() const;

    /** @brief 获取当前数据库路径，UTF-8 编码。未打开时返回空字符串。 */
    std::string GetDatabasePathUtf8() const;

    /** @brief 获取当前数据库配置快照。 */
    GB_SqliteOptions GetOptions() const;

    /** @brief 获取最近一次失败操作的错误信息。 */
    GB_SqliteError GetLastError() const;

    /** @brief 清空最近一次错误信息。 */
    void ClearLastError();


    /**
     * @brief 获取当前数据库某个 schema 中的所有表名。
     *
     * @param outTableNames 输出表名列表。函数开始时会清空旧内容。
     * @param includeSystemTables 是否包含 sqlite_ 开头的 SQLite 内部表。
     * @param schemaNameUtf8 schema 名称，默认 main；也可传入 temp 或已 ATTACH 的 schema 名。
     * @return true 查询成功；
     * @return false 查询失败，可通过 GetLastError() 获取错误信息。
     */
    bool GetTableNames(std::vector<std::string>& outTableNames, bool includeSystemTables = false, const std::string& schemaNameUtf8 = "main") const;

    /**
     * @brief 判断指定表是否存在。
     *
     * @param tableNameUtf8 表名，UTF-8 编码，不需要包含 schema 前缀。
     * @param outExists 输出是否存在。函数开始时会先置为 false。
     * @param includeSystemTables 是否允许匹配 sqlite_ 开头的 SQLite 内部表。
     * @param schemaNameUtf8 schema 名称，默认 main。
     * @return true 查询成功；
     * @return false 查询失败。
     */
    bool TableExists(const std::string& tableNameUtf8, bool& outExists, bool includeSystemTables = false, const std::string& schemaNameUtf8 = "main") const;

    /**
     * @brief 获取指定表的字段定义信息。
     *
     * @param tableNameUtf8 表名，UTF-8 编码，不需要包含 schema 前缀。
     * @param outFieldInfos 输出字段信息。函数开始时会清空旧内容。
     * @param includeHiddenFields 是否包含虚表隐藏字段、生成字段等 table_xinfo 扩展字段。
     * @param schemaNameUtf8 schema 名称，默认 main。
     * @return true 查询成功；
     * @return false 查询失败。
     *
     * @remarks
     * 本接口优先使用 SQLite 的 pragma_table_xinfo 表值函数，可取得比 table_info 更完整的字段信息。
     */
    bool GetTableFieldInfos(const std::string& tableNameUtf8, std::vector<GB_SqliteTableFieldInfo>& outFieldInfos, bool includeHiddenFields = false, const std::string& schemaNameUtf8 = "main") const;

    /**
     * @brief 获取指定表的当前数据内容。
     *
     * @param tableNameUtf8 表名，UTF-8 编码，不需要包含 schema 前缀。
     * @param outResult 输出查询结果。字段信息和单元格数据按 SELECT * 的列顺序返回。
     * @param maxRowCount 最大读取行数；0 表示不限制。
     * @param schemaNameUtf8 schema 名称，默认 main。
     * @return true 查询成功；
     * @return false 查询失败。
     *
     * @remarks
     * 该接口会完整读取结果到内存。大表建议设置 maxRowCount，或直接使用 QueryEach() 自行流式遍历。
     */
    bool GetTableData(const std::string& tableNameUtf8, GB_SqliteResult& outResult, std::size_t maxRowCount = 0, const std::string& schemaNameUtf8 = "main") const;

    /**
     * @brief 获取指定表的当前行数。
     *
     * @param tableNameUtf8 表名，UTF-8 编码，不需要包含 schema 前缀。
     * @param outRowCount 输出行数。函数开始时会先置为 0。
     * @param schemaNameUtf8 schema 名称，默认 main。
     * @return true 查询成功；
     * @return false 查询失败。
     */
    bool GetTableRowCount(const std::string& tableNameUtf8, long long& outRowCount, const std::string& schemaNameUtf8 = "main") const;

    /**
     * @brief 判断指定表中是否存在一行满足字段等值条件的数据。
     *
     * @param tableNameUtf8 表名，UTF-8 编码，不需要包含 schema 前缀。
     * @param equalFieldValues 字段等值条件。key 为字段名，value 为期望值；value 为空 GB_Variant 时按 SQL NULL 判断。
     * @param outExists 输出是否存在。函数开始时会先置为 false。
     * @param schemaNameUtf8 schema 名称，默认 main。
     * @return true 查询成功；
     * @return false 查询失败。
     *
     * @remarks
     * equalFieldValues 为空时，本接口退化为判断表中是否至少存在一行数据。
     */
    bool TableRowExists(const std::string& tableNameUtf8, const GB_SqliteNamedParameters& equalFieldValues, bool& outExists, const std::string& schemaNameUtf8 = "main") const;

    /**
     * @brief 将另一个 SQLite 数据库附加到当前连接池的所有内部连接。
     *
     * @param databasePathUtf8 被附加数据库路径，UTF-8 编码；启用 URI 时可传入 file: URI。
     * @param schemaNameUtf8 附加后的 schema 名称，不能为 main 或 temp。
     * @return true 附加成功；
     * @return false 附加失败。
     *
     * @remarks
     * SQLite 的 ATTACH 是连接级状态。为了避免“写连接已 ATTACH、读连接未 ATTACH”的不一致，本接口会在 schema 写锁下同步更新写连接和所有读连接。
     */
    bool AttachDatabase(const std::string& databasePathUtf8, const std::string& schemaNameUtf8);

    /**
     * @brief 从当前连接池的所有内部连接中分离一个已附加数据库。
     *
     * @param schemaNameUtf8 需要分离的 schema 名称，不能为 main 或 temp。
     * @return true 分离成功；
     * @return false 分离失败。
     */
    bool DetachDatabase(const std::string& schemaNameUtf8);


    /**
     * @brief 执行一条不带参数的非查询 SQL。
     *
     * @param sqlUtf8 SQL 文本，UTF-8 编码。应为单条 SQL。
     * @return true 执行成功；
     * @return false 执行失败。
     *
     * @remarks
     * 适合 CREATE TABLE、INSERT、UPDATE、DELETE、PRAGMA 等无需返回结果集的语句。
     */
    bool Execute(const std::string& sqlUtf8);

    /**
     * @brief 执行一条使用位置参数的非查询 SQL。
     *
     * @param sqlUtf8 SQL 文本，UTF-8 编码。应为单条 SQL。
     * @param parameters 位置参数列表，对应 ? 或 ?NNN。
     * @return true 执行成功；
     * @return false 执行失败。
     */
    bool Execute(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters);

    /**
     * @brief 执行一条使用命名参数的非查询 SQL。
     *
     * @param sqlUtf8 SQL 文本，UTF-8 编码。应为单条 SQL。
     * @param parameters 命名参数表。key 可写为 "id"、":id"、"@id"、"$id" 或 "?1" 等形式。
     * @return true 执行成功；
     * @return false 执行失败。
     */
    bool ExecuteNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters);

    /**
     * @brief 批量执行同一条使用位置参数的 SQL。
     *
     * @param sqlUtf8 SQL 文本，UTF-8 编码。通常为 INSERT / UPDATE / DELETE。
     * @param parameterRows 多行参数，每个元素对应一次执行。
     * @param useTransaction 是否由内部自动包裹事务。
     * @return true 全部执行成功；
     * @return false 任一行执行失败。
     *
     * @remarks
     * 本接口会复用同一条预编译语句，适合大量同构写入；通常比循环调用 Execute() 更高效。
     */
    bool ExecuteMany(const std::string& sqlUtf8, const std::vector<GB_SqliteParameterList>& parameterRows, bool useTransaction = true);

    /**
     * @brief 批量执行同一条使用命名参数的 SQL。
     *
     * @param sqlUtf8 SQL 文本，UTF-8 编码。通常为 INSERT / UPDATE / DELETE。
     * @param parameterRows 多行命名参数，每个元素对应一次执行。
     * @param useTransaction 是否由内部自动包裹事务。
     * @return true 全部执行成功；
     * @return false 任一行执行失败。
     */
    bool ExecuteManyNamed(const std::string& sqlUtf8, const std::vector<GB_SqliteNamedParameters>& parameterRows, bool useTransaction = true);

    /**
     * @brief 执行 SQL 批处理文本。
     *
     * @param sqlBatchUtf8 SQL 批处理文本，允许包含多条 SQL。
     * @return true 执行成功；
     * @return false 执行失败。
     *
     * @remarks
     * 适合一次性建表、建索引、初始化脚本等场景。需要参数绑定时不应使用本接口。
     */
    bool ExecuteBatch(const std::string& sqlBatchUtf8);

    /**
     * @brief 执行不带参数的只读查询，并将结果完整读入 outResult。
     *
     * @param sqlUtf8 SQL 查询文本，UTF-8 编码。应为只读单条 SQL。
     * @param outResult 输出查询结果。函数开始时会清空旧结果。
     * @param maxRowCount 最大读取行数；0 表示不限制。
     * @return true 查询成功；
     * @return false 查询失败。
     */
    bool Query(const std::string& sqlUtf8, GB_SqliteResult& outResult, std::size_t maxRowCount = 0) const;

    /**
     * @brief 执行使用位置参数的只读查询，并将结果完整读入 outResult。
     */
    bool Query(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount = 0) const;

    /**
     * @brief 执行使用命名参数的只读查询，并将结果完整读入 outResult。
     */
    bool QueryNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount = 0) const;

    /**
     * @brief 执行不带参数的只读查询，并逐行回调。
     *
     * @param sqlUtf8 SQL 查询文本，UTF-8 编码。应为只读单条 SQL。
     * @param rowCallback 行回调。返回 false 可提前停止遍历。
     * @return true 查询执行成功；
     * @return false 查询执行失败或回调为空。
     *
     * @remarks
     * 适合大结果集遍历，可避免一次性占用大量内存。
     */
    bool QueryEach(const std::string& sqlUtf8, const GB_SqliteRowCallback& rowCallback) const;

    /** @brief 执行使用位置参数的只读查询，并逐行回调。 */
    bool QueryEach(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, const GB_SqliteRowCallback& rowCallback) const;

    /** @brief 执行使用命名参数的只读查询，并逐行回调。 */
    bool QueryEachNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, const GB_SqliteRowCallback& rowCallback) const;

    /**
     * @brief 执行不带参数的只读查询，并读取第一行第一列。
     *
     * @param sqlUtf8 SQL 查询文本，UTF-8 编码。
     * @param outValue 输出第一行第一列的值。无结果时通常为空 GB_Variant。
     * @return true 查询成功；
     * @return false 查询失败。
     */
    bool ExecuteScalar(const std::string& sqlUtf8, GB_Variant& outValue) const;

    /** @brief 执行使用位置参数的只读查询，并读取第一行第一列。 */
    bool ExecuteScalar(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_Variant& outValue) const;

    /** @brief 执行使用命名参数的只读查询，并读取第一行第一列。 */
    bool ExecuteScalarNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_Variant& outValue) const;

    /**
     * @brief 开启显式事务。
     *
     * @param transactionMode 事务开启模式。默认 Immediate。
     * @return GB_SqliteTransaction 事务对象。需要通过 IsActive() 判断是否开启成功。
     *
     * @remarks
     * 事务对象析构时若仍 active，会自动 Rollback()。事务内请使用返回对象的 Execute() / Query() 接口。
     */
    GB_SqliteTransaction BeginTransaction(GB_SqliteTransactionMode transactionMode = GB_SqliteTransactionMode::Immediate);

    /**
     * @brief 在事务中执行一个函数对象。
     *
     * @param transactionFunc 事务回调。返回 true 时提交，返回 false 时回滚。
     * @param transactionMode 事务开启模式。默认 Immediate。
     * @return true 事务函数返回 true 且 Commit() 成功；
     * @return false 开启事务失败、事务函数返回 false、执行异常或提交失败。
     *
     * @remarks
     * 该接口适合把多条相关 SQL 组织成一个原子操作。若回调抛出异常，内部会尽量回滚事务，然后返回 false。
     */
    bool ExecuteInTransaction(const std::function<bool(GB_SqliteTransaction& transaction)>& transactionFunc, GB_SqliteTransactionMode transactionMode = GB_SqliteTransactionMode::Immediate);

    /** @brief 获取写连接上最近一次成功 INSERT 的 rowid。 */
    long long GetLastInsertRowId() const;

    /** @brief 获取写连接上最近一次 INSERT / UPDATE / DELETE 影响的行数。 */
    int GetChanges() const;

    /** @brief 获取写连接自打开以来累计 INSERT / UPDATE / DELETE 影响的行数。 */
    int GetTotalChanges() const;

    /**
     * @brief 修改 busy timeout。
     *
     * @param busyTimeoutMs 忙等待超时，单位毫秒。0 表示不等待。
     * @return true 设置成功；
     * @return false 数据库未打开或设置失败。
     *
     * @remarks
     * 会尽量同步更新写连接和当前空闲读连接；正在使用中的读连接会在归还或后续初始化路径中保持一致。
     */
    bool SetBusyTimeout(int busyTimeoutMs);

    /**
     * @brief 对 WAL 数据库执行 checkpoint。
     *
     * @param truncate 是否使用 TRUNCATE checkpoint；false 时使用普通 checkpoint。
     * @return true 执行成功；
     * @return false 执行失败。
     */
    bool CheckpointWal(bool truncate = false);

    /**
     * @brief 对 WAL 数据库执行 checkpoint，并返回 checkpoint 统计结果。
     *
     * @param outCheckpointResult 输出 checkpoint 结果；可为 nullptr。
     * @param truncate 是否使用 TRUNCATE checkpoint；false 时使用普通 checkpoint。
     * @return true 执行成功；
     * @return false 执行失败。
     */
    bool CheckpointWal(GB_SqliteCheckpointResult* outCheckpointResult, bool truncate = false);

    /**
     * @brief 清空所有当前可清理的预编译语句缓存。
     *
     * @remarks
     * 在执行大量 schema 变更、希望释放内存或准备关闭数据库前可以调用。正在执行中的语句不会被强制破坏。
     */
    void ClearStatementCache();

    /**
     * @brief 获取语句缓存统计快照。
     *
     * @return GB_SqliteStatementCacheStats 当前统计信息。
     *
     * @remarks
     * - 为避免在 QueryEach() 回调等场景中等待自身释放读连接，本接口不会阻塞等待正在使用的读连接。
     * - 正在使用中的读连接会计入 connectionCount，但其缓存命中统计可能不会计入当前快照。
     */
    GB_SqliteStatementCacheStats GetStatementCacheStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class GB_SqliteTransaction;
};

/**
 * @brief SQLite 事务 RAII 封装。
 *
 * @details
 * GB_SqliteTransaction 只能通过 GB_Sqlite::BeginTransaction() 或 GB_Sqlite::ExecuteInTransaction() 创建。
 * 事务对象内部持有写连接租约，事务内的 Execute() / Query() / ExecuteScalar() 均在同一连接上执行。
 *
 * @remarks
 * 析构时若事务仍处于 active 状态，会自动 Rollback()，避免异常路径或提前返回导致事务悬挂。
 */
class GLOBALBASE_PORT GB_SqliteTransaction
{
public:
    /** @brief 构造一个无效事务对象。 */
    GB_SqliteTransaction();

    /** @brief 析构事务对象。若事务仍 active，会自动 Rollback()。 */
    ~GB_SqliteTransaction();

    GB_SqliteTransaction(const GB_SqliteTransaction& other) = delete;
    GB_SqliteTransaction& operator=(const GB_SqliteTransaction& other) = delete;

    /** @brief 移动构造事务对象。 */
    GB_SqliteTransaction(GB_SqliteTransaction&& other) noexcept;

    /** @brief 移动赋值事务对象。当前 active 事务会先按析构语义处理。 */
    GB_SqliteTransaction& operator=(GB_SqliteTransaction&& other) noexcept;

    /** @brief 判断事务是否处于 active 状态。 */
    bool IsActive() const;

    /** @brief 获取事务最近一次失败操作的错误信息。 */
    GB_SqliteError GetLastError() const;

    /** @brief 在当前事务中执行一条不带参数的非查询 SQL。 */
    bool Execute(const std::string& sqlUtf8);

    /** @brief 在当前事务中执行一条使用位置参数的非查询 SQL。 */
    bool Execute(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters);

    /** @brief 在当前事务中执行一条使用命名参数的非查询 SQL。 */
    bool ExecuteNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters);

    /** @brief 在当前事务中执行不带参数的查询。事务内查询会使用事务持有的写连接。 */
    bool Query(const std::string& sqlUtf8, GB_SqliteResult& outResult, std::size_t maxRowCount = 0);

    /** @brief 在当前事务中执行使用位置参数的查询。 */
    bool Query(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount = 0);

    /** @brief 在当前事务中执行使用命名参数的查询。 */
    bool QueryNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_SqliteResult& outResult, std::size_t maxRowCount = 0);

    /** @brief 在当前事务中执行不带参数的查询，并读取第一行第一列。 */
    bool ExecuteScalar(const std::string& sqlUtf8, GB_Variant& outValue);

    /** @brief 在当前事务中执行使用位置参数的查询，并读取第一行第一列。 */
    bool ExecuteScalar(const std::string& sqlUtf8, const GB_SqliteParameterList& parameters, GB_Variant& outValue);

    /** @brief 在当前事务中执行使用命名参数的查询，并读取第一行第一列。 */
    bool ExecuteScalarNamed(const std::string& sqlUtf8, const GB_SqliteNamedParameters& parameters, GB_Variant& outValue);

    /**
     * @brief 提交事务。
     *
     * @return true 提交成功；
     * @return false 提交失败或事务无效。失败后事务通常仍会尝试保持 active，调用者可根据错误决定 Rollback()。
     */
    bool Commit();

    /**
     * @brief 回滚事务。
     *
     * @return true 回滚成功或事务已经不可用；
     * @return false 回滚失败。
     */
    bool Rollback();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    GB_SqliteTransaction(GB_Sqlite& database, GB_SqliteTransactionMode transactionMode);

    friend class GB_Sqlite;
};

#if 0
// ============================================================================
// GB_Sqlite 使用示例
// ============================================================================

// 示例 1：打开数据库、建表、插入和查询。
static void GB_SqliteExample_BasicUsage()
{
    GB_SqliteOptions options;
    options.openMode = GB_SqliteOpenMode::ReadWriteCreate;
    options.enableWal = true;
    options.busyTimeoutMs = 5000;
    options.maxCachedStatementsPerConnection = 128;

    GB_Sqlite database;
    if (!database.Open("example.db", options))
    {
        const GB_SqliteError error = database.GetLastError();
        // 这里可以记录 error.code、error.extendedCode、error.messageUtf8、error.sqlUtf8。
        return;
    }

    if (!database.Execute("CREATE TABLE IF NOT EXISTS person(id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)"))
    {
        return;
    }

    GB_SqliteParameterList insertParameters;
    insertParameters.push_back(GB_Variant(GB_STR("张三")));
    insertParameters.push_back(GB_Variant(30));
    database.Execute("INSERT INTO person(name, age) VALUES(?, ?)", insertParameters);

    GB_SqliteResult result;
    if (database.Query("SELECT id, name, age FROM person ORDER BY id", result))
    {
        for (std::size_t rowIndex = 0; rowIndex < result.RowCount(); rowIndex++)
        {
            const GB_Variant id = result.GetValue(rowIndex, "id");
            const GB_Variant name = result.GetValue(rowIndex, "name");
            const GB_Variant age = result.GetValue(rowIndex, "age");
            // 在这里使用 id、name、age。
        }
    }
}

// 示例 2：使用命名参数，避免参数顺序错误。
static void GB_SqliteExample_NamedParameters(GB_Sqlite& database)
{
    GB_SqliteNamedParameters parameters;
    parameters["name"] = GB_Variant(GB_STR("李四"));
    parameters["age"] = GB_Variant(28);

    database.ExecuteNamed("INSERT INTO person(name, age) VALUES(:name, :age)", parameters);

    GB_SqliteNamedParameters queryParameters;
    queryParameters[":minAge"] = GB_Variant(18);

    GB_SqliteResult result;
    database.QueryNamed("SELECT id, name, age FROM person WHERE age >= :minAge ORDER BY age", queryParameters, result);
}

// 示例 3：批量插入。大量同构写入优先使用 ExecuteMany()，不要循环调用 Execute()。
static void GB_SqliteExample_ExecuteMany(GB_Sqlite& database)
{
    std::vector<GB_SqliteParameterList> rows;

    GB_SqliteParameterList firstRow;
    firstRow.push_back(GB_Variant(GB_STR("武汉")));
    firstRow.push_back(GB_Variant(1200));
    rows.push_back(firstRow);

    GB_SqliteParameterList secondRow;
    secondRow.push_back(GB_Variant(GB_STR("南京")));
    secondRow.push_back(GB_Variant(900));
    rows.push_back(secondRow);

    database.Execute("CREATE TABLE IF NOT EXISTS city(name TEXT NOT NULL, score INTEGER NOT NULL)");
    database.ExecuteMany("INSERT INTO city(name, score) VALUES(?, ?)", rows, true);
}

// 示例 4：显式事务。事务内必须使用 transaction 对象执行 SQL。
static bool GB_SqliteExample_Transaction(GB_Sqlite& database)
{
    GB_SqliteTransaction transaction = database.BeginTransaction(GB_SqliteTransactionMode::Immediate);
    if (!transaction.IsActive())
    {
        return false;
    }

    if (!transaction.Execute("UPDATE account SET balance = balance - 100 WHERE id = 1"))
    {
        transaction.Rollback();
        return false;
    }

    if (!transaction.Execute("UPDATE account SET balance = balance + 100 WHERE id = 2"))
    {
        transaction.Rollback();
        return false;
    }

    return transaction.Commit();
}

// 示例 5：使用 ExecuteInTransaction() 组织原子操作。
static bool GB_SqliteExample_ExecuteInTransaction(GB_Sqlite& database)
{
    return database.ExecuteInTransaction([](GB_SqliteTransaction& transaction) -> bool
        {
            if (!transaction.Execute("INSERT INTO log(message) VALUES('begin')"))
            {
                return false;
            }

            if (!transaction.Execute("INSERT INTO log(message) VALUES('end')"))
            {
                return false;
            }

            return true;
        });
}

// 示例 6：大结果集遍历。QueryEach() 避免一次性把所有行读入内存。
static void GB_SqliteExample_QueryEach(GB_Sqlite& database)
{
    database.QueryEach("SELECT id, name FROM person ORDER BY id", [](const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values) -> bool
        {
            (void)columns;
            (void)values;

            // 返回 true 继续遍历下一行；返回 false 可提前停止。
            return true;
        });
}

// 示例 7：读取单值和检查缓存统计。
static void GB_SqliteExample_ScalarAndStats(GB_Sqlite& database)
{
    GB_Variant count;
    if (database.ExecuteScalar("SELECT COUNT(*) FROM person", count))
    {
        // 在这里使用 count。
    }

    const GB_SqliteStatementCacheStats stats = database.GetStatementCacheStats();
    // 可根据 stats.hits、stats.misses、stats.evictions 判断语句缓存配置是否合适。
}

// 示例 8：WAL checkpoint。适合在批量写入后或程序空闲时主动执行。
static void GB_SqliteExample_CheckpointWal(GB_Sqlite& database)
{
    GB_SqliteCheckpointResult checkpointResult;
    if (database.CheckpointWal(&checkpointResult, false))
    {
        const bool complete = checkpointResult.IsComplete();
        (void)complete;
    }
}
#endif

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
