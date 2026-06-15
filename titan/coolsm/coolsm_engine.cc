#include "coolsm_engine.h"
#include <iostream>

#include "rocksdb/sst_file_reader.h"
#include "pgm/pgm_index.hpp"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <sstream>

// 全局定义（匹配头文件中的 extern 声明）
std::unordered_map<std::string, rocksdb::coolsm::TMR_LearnedIndex> global_tmr_indexes;
std::mutex index_map_mutex;

using namespace rocksdb::coolsm;

namespace {

// 辅助函数：根据数值化 Key 找到所属的 SST 文件路径
static std::string FindSSTFileForKey(uint64_t numeric_key) {
    std::lock_guard<std::mutex> lock(index_map_mutex);
    for (const auto& [file_path, tmr] : global_tmr_indexes) {
        if (numeric_key >= tmr.smallest_key && numeric_key <= tmr.largest_key) {
            return file_path;
        }
    }
    return "";
}

uint64_t SafeSliceToUint64(const rocksdb::Slice& slice) {
    if (slice.empty()) return 0;
    std::string s = slice.ToString();
    
    if (s.size() >= 16) {
        bool is_hex = true;
        for (size_t i = 0; i < 16; ++i) {
            if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
                is_hex = false;
                break;
            }
        }
        if (is_hex) {
            uint64_t val = 0;
            std::stringstream ss;
            ss << std::hex << s.substr(0, 16);
            ss >> val;
            return val;
        }
    }

    std::string digits = "";
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits += c;
        }
    }
    if (!digits.empty()) {
        try {
            if (digits.size() > 19) digits = digits.substr(0, 19);
            return std::stoull(digits);
        } catch (...) { }
    }

    uint64_t num = 0;
    size_t start_idx = (s.size() > sizeof(uint64_t)) ? (s.size() - sizeof(uint64_t)) : 0;
    size_t len = std::min(s.size(), sizeof(uint64_t));
    for (size_t i = 0; i < len; ++i) {
        num = (num << 8) | static_cast<uint8_t>(s[start_idx + i]);
    }
    return num;
}

} // anonymous namespace

namespace rocksdb {
namespace coolsm {

// ==================== CooLSMEngine 实现 ====================
CooLSMEngine::CooLSMEngine() : db_(nullptr) {}
CooLSMEngine::~CooLSMEngine() { if (db_) delete db_; }

Status CooLSMEngine::Open(const Options& options, const std::string& dbname) {
    options_ = options;
    compactor_listener_ = std::make_shared<CooLSMCompactor>();
    options_.listeners.push_back(compactor_listener_);
    Status s = DB::Open(options_, dbname, &db_);
    if (!s.ok()) return s;
    ingestor_ = std::make_unique<CooLSMIngestor>(db_);
    reader_ = std::make_unique<CooLSMReader>(db_);
    std::cout << "[CooLSM Base] Architecture deconstructed into Ingestor/Reader/Compactor successfully." << std::endl;
    return Status::OK();
}

Status CooLSMEngine::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
    return ingestor_->Put(options, key, value);
}

// 关键修复：路由到 PointLookup，启用 TMR 表决器
Status CooLSMEngine::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    return reader_->PointLookup(options, key, value);
}

// ==================== Ingestor 实现 ====================
Status CooLSMIngestor::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
    return db_->Put(options, key, value);
}

// ==================== Reader 实现 ====================
Status CooLSMReader::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    return db_->Get(options, key, value);
}

Status CooLSMReader::PointLookup(const ReadOptions& options, const Slice& key, std::string* value) {
    uint64_t numeric_key = SafeSliceToUint64(key);

    std::string target_file = FindSSTFileForKey(numeric_key);
    if (target_file.empty()) {
        std::cout << "[CooLSM Reader] No learned index found for key " << numeric_key 
                  << ", fallback to raw Get." << std::endl;
        return db_->Get(options, key, value);
    }

    TMR_LearnedIndex tmr_copy;
    {
        std::lock_guard<std::mutex> lock(index_map_mutex);
        auto it = global_tmr_indexes.find(target_file);
        if (it == global_tmr_indexes.end()) {
            return db_->Get(options, key, value);
        }
        tmr_copy = it->second;
    }

    auto range1 = tmr_copy.model_v1.search(numeric_key);
    auto range2 = tmr_copy.model_v2.search(numeric_key);
    auto range3 = tmr_copy.model_v3.search(numeric_key);

    bool v1_ok = (range1.lo == range2.lo && range1.hi == range2.hi);
    bool v2_ok = (range2.lo == range3.lo && range2.hi == range3.hi);
    bool need_heal = false;

    if (v1_ok && v2_ok) {
        std::cout << "[CooLSM Reader] All three models agree for key " << numeric_key << std::endl;
    } else if (v1_ok && !v2_ok) {
        std::cout << "[CooLSM Healing] Radiation damage detected in model_v3 for file " 
                  << target_file << "! Healing using model_v1." << std::endl;
        tmr_copy.model_v3 = tmr_copy.model_v1;
        need_heal = true;
    } else if (!v1_ok && v2_ok) {
        std::cout << "[CooLSM Healing] Radiation damage detected in model_v1 for file " 
                  << target_file << "! Healing using model_v2." << std::endl;
        tmr_copy.model_v1 = tmr_copy.model_v2;
        need_heal = true;
    } else {
        std::cout << "[CooLSM Healing] Severe inconsistency! Healing all models using model_v2." << std::endl;
        tmr_copy.model_v1 = tmr_copy.model_v2;
        tmr_copy.model_v3 = tmr_copy.model_v2;
        need_heal = true;
    }

    if (need_heal) {
        std::lock_guard<std::mutex> lock(index_map_mutex);
        global_tmr_indexes[target_file] = tmr_copy;
    }

    auto final_range = tmr_copy.model_v2.search(numeric_key);
    std::cout << "[CooLSM Reader] Final predicted range for key " << numeric_key 
              << " in file " << target_file << ": [" << final_range.lo << ", " << final_range.hi << "]" 
              << std::endl;

    return db_->Get(options, key, value);
}

// ==================== Compactor 实现 ====================
void CooLSMCompactor::OnTableFileCreated(const TableFileCreationInfo& info) {
    if (!info.status.ok() || info.file_size == 0) return;

    std::cout << "[CooLSM Compactor] Detected new file: " << info.file_path
              << " (size: " << info.file_size << " bytes), starting learned index building..." << std::endl;

    rocksdb::Options options;
    rocksdb::SstFileReader reader(options);
    rocksdb::Status s = reader.Open(info.file_path);
    if (!s.ok()) {
        std::cerr << "[CooLSM Compactor] Failed to open SST file: " << s.ToString() << std::endl;
        return;
    }

    rocksdb::ReadOptions ro;
    std::unique_ptr<rocksdb::Iterator> iter(reader.NewIterator(ro));
    if (!iter) {
        std::cerr << "[CooLSM Compactor] Failed to create iterator for SST file" << std::endl;
        return;
    }

    std::vector<uint64_t> keys;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        uint64_t numeric_key = SafeSliceToUint64(iter->key());
        keys.push_back(numeric_key);
    }

    if (keys.empty()) {
        std::cerr << "[CooLSM Compactor] No keys extracted from SST file" << std::endl;
        return;
    }

    auto last = std::unique(keys.begin(), keys.end());
    keys.erase(last, keys.end());

    pgm::PGMIndex<uint64_t, 64> index(keys.begin(), keys.end());
    uint64_t min_key = keys.front();
    uint64_t max_key = keys.back();

    {
        std::lock_guard<std::mutex> lock(index_map_mutex);
        TMR_LearnedIndex tmr;
        tmr.model_v1 = index;
        tmr.model_v2 = index;
        tmr.model_v3 = index;
        tmr.smallest_key = min_key;
        tmr.largest_key = max_key;
        global_tmr_indexes[info.file_path] = tmr;
    }

    std::cout << "[CooLSM Compactor] Extracted " << keys.size() << " unique numeric keys from SST file." << std::endl;
    std::cout << "[CooLSM Compactor] Learned index built. Model segments: " 
              << index.segments_count() << std::endl;
}

void CooLSMCompactor::TriggerManualReorganization(DB* db) {
    db->CompactRange(CompactRangeOptions(), nullptr, nullptr);
}

} // namespace coolsm
} // namespace rocksdb