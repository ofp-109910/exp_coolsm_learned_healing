#ifndef TITAN_COOLSM_ENGINE_H
#define TITAN_COOLSM_ENGINE_H

#include "rocksdb/db.h"
#include "rocksdb/listener.h"
#include <memory>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

#include "pgm/pgm_index.hpp"


// 全局作用域声明，供测试程序使用
#include <unordered_map>
#include <string>

// 前向声明（因为 TMR_LearnedIndex 定义在 rocksdb::coolsm 中）
namespace rocksdb {
namespace coolsm {
    struct TMR_LearnedIndex;
}
}

extern std::unordered_map<std::string, rocksdb::coolsm::TMR_LearnedIndex> global_tmr_indexes;
extern std::mutex index_map_mutex;





namespace rocksdb {
namespace coolsm {

    // 在 coolsm_engine.h 或 coolsm_engine.cc 顶部定义
struct TMR_LearnedIndex {
    pgm::PGMIndex<uint64_t, 64> model_v1;
    pgm::PGMIndex<uint64_t, 64> model_v2;
    pgm::PGMIndex<uint64_t, 64> model_v3;
    // 新增：文件的用户 Key 范围（数值化后的最小/最大 Key）
    uint64_t smallest_key;
    uint64_t largest_key;
};


// ==========================================
// 1. Reader 模块
// ==========================================
class CooLSMReader {
public:
    explicit CooLSMReader(DB* db) : db_(db) {}
    Status Get(const ReadOptions& options, const Slice& key, std::string* value);
    // 新增声明
    Status PointLookup(const ReadOptions& options, const Slice& key, std::string* value);
private:
    DB* db_;
};

// ==========================================
// 2. Ingestor 模块（已修正大小写与方法名）
// ==========================================
class CooLSMIngestor {
public:
    CooLSMIngestor(DB* db) : db_(db) {}
    Status Put(const WriteOptions& options, const Slice& key, const Slice& value);
private:
    DB* db_;
};

// ==========================================
// 3. Compactor 模块
// ==========================================
class CooLSMCompactor : public EventListener {
public:
    CooLSMCompactor() = default;
    void OnTableFileCreated(const TableFileCreationInfo& info) override;
    void TriggerManualReorganization(DB* db);
};

// ==========================================
// 4. CooLSM 顶层引擎
// ==========================================
class CooLSMEngine {
public:
    CooLSMEngine();
    ~CooLSMEngine();

    Status Open(const Options& options, const std::string& dbname);
    Status Put(const WriteOptions& options, const Slice& key, const Slice& value);
    Status Get(const ReadOptions& options, const Slice& key, std::string* value);

private:
    DB* db_;
    Options options_;
    std::shared_ptr<CooLSMCompactor> compactor_listener_;
    
    std::unique_ptr<CooLSMIngestor> ingestor_;
    std::unique_ptr<CooLSMReader> reader_;
};



} // namespace coolsm
} // namespace rocksdb

#endif // TITAN_COOLSM_ENGINE_H