// test_amzn.cc - 使用 SOSD 真实数据集 (books_200M_uint32) 的独立测试程序

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "coolsm_engine.h"
#include "pgm/pgm_index.hpp"
#include "rocksdb/db.h"
#include "rocksdb/options.h"

using namespace rocksdb::coolsm;

// ======================= 辅助函数 =======================
// 根据数值化 Key 找到所属的 SST 文件路径（线性扫描 global_tmr_indexes）
std::string FindSSTFileForKey(uint64_t numeric_key) {
    extern std::unordered_map<std::string, TMR_LearnedIndex> global_tmr_indexes;
    extern std::mutex index_map_mutex;

    std::lock_guard<std::mutex> lock(index_map_mutex);
    for (const auto& [file_path, tmr] : global_tmr_indexes) {
        if (numeric_key >= tmr.smallest_key && numeric_key <= tmr.largest_key) {
            return file_path;
        }
    }
    return "";
}

// 加载 SOSD 数据集的键 (批量读取，使用 pread)
std::vector<uint64_t> load_sosd_keys_batch(const std::string& filepath, size_t offset, size_t count) {
    std::vector<uint64_t> keys;
    keys.reserve(count);

    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("open");
        return keys;
    }

    off_t data_offset = 8 + offset * sizeof(uint32_t);
    size_t read_bytes = count * sizeof(uint32_t);

    std::vector<uint32_t> buffer(count);
    ssize_t n = pread(fd, buffer.data(), read_bytes, data_offset);
    if (n != static_cast<ssize_t>(read_bytes)) {
        if (n < 0) perror("pread");
        else std::cerr << "pread returned short count: " << n << std::endl;
        close(fd);
        return keys;
    }

    keys.resize(count);
    for (size_t i = 0; i < count; ++i) {
        keys[i] = static_cast<uint64_t>(buffer[i]);
    }

    close(fd);
    return keys;
}

// 格式化 Key (与 PGM 索引训练格式对齐)
std::string format_key(uint64_t numeric_key) {
    std::stringstream ss;
    ss << std::setw(16) << std::setfill('0') << std::hex << numeric_key << "_usertag";
    return ss.str();
}

// 辐射模拟函数：破坏指定 SST 文件的 model_v1
void SimulateRadiationForSST(const std::string& sst_path) {
    std::cout << "========= [RADIATION SIMULATION] =========" << std::endl;
    std::cout << "Simulating high-energy cosmic ray striking memory..." << std::endl;

    extern std::unordered_map<std::string, TMR_LearnedIndex> global_tmr_indexes;
    extern std::mutex index_map_mutex;

    std::lock_guard<std::mutex> lock(index_map_mutex);
    auto it = global_tmr_indexes.find(sst_path);
    if (it == global_tmr_indexes.end()) {
        std::cout << "[Radiation] SST path " << sst_path << " not found in TMR indexes." << std::endl;
        return;
    }

    auto& tmr = it->second;
    std::vector<uint64_t> wrong_keys = {tmr.smallest_key, tmr.smallest_key + 1};
    pgm::PGMIndex<uint64_t, 64> corrupted_model(wrong_keys.begin(), wrong_keys.end());
    tmr.model_v1 = corrupted_model;

    std::cout << "[Radiation] model_v1 corrupted for SST " << sst_path
              << " (replaced with short index)." << std::endl;
    std::cout << "[Radiation] Original smallest_key/largest_key: "
              << tmr.smallest_key << " - " << tmr.largest_key << std::endl;
}

// 安全的键转换函数（复制自 coolsm_engine.cc，保持一致性）
uint64_t SafeSliceToUint64(const rocksdb::Slice& slice) {
    if (slice.empty()) return 0;
    std::string s = slice.ToString();
    // ... 完整实现见原文件，此处省略，实际使用时需要复制完整实现 ...
    // 为了编译通过，这里给出简化版（实际应使用原有完整实现）
    uint64_t num = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
        }
    }
    return num;
}

// ======================= main 函数 =======================
int main(int argc, char* argv[]) {
    std::string data_file = "../../data/books_200M_uint32";
    size_t total_keys_limit = 10000000;   // 默认500万，可通过命令行参数覆盖
    size_t batch_size = 100000;

    if (argc > 1) data_file = argv[1];
    if (argc > 2) total_keys_limit = std::stoul(argv[2]);

    std::cout << ">>> Loading SOSD dataset: " << data_file << std::endl;
    std::cout << ">>> Total keys limit: " << total_keys_limit << std::endl;

    // 获取数据集总大小
    int fd = open(data_file.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error: cannot open file " << data_file << std::endl;
        return 1;
    }
    uint64_t total_count = 0;
    if (read(fd, &total_count, sizeof(total_count)) != sizeof(total_count)) {
        std::cerr << "Error: cannot read 64-bit header" << std::endl;
        close(fd);
        return 1;
    }
    close(fd);

    std::cout << ">>> Dataset header indicates " << total_count << " uint32 elements" << std::endl;
    size_t actual_count = (total_keys_limit < total_count) ? total_keys_limit : total_count;
    std::cout << ">>> Processing " << actual_count << " keys..." << std::endl;

    // 配置 RocksDB
    std::string dbpath = "/tmp/coolsm_amzn_test_db";
    rocksdb::DestroyDB(dbpath, rocksdb::Options());

    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024 * 1024;   // 64 MB
    options.target_file_size_base = 64 * 1024 * 1024;
    options.max_background_flushes = 4;
    options.max_background_compactions = 4;

    std::cout << ">>> Opening database at " << dbpath << std::endl;
    CooLSMEngine engine;
    rocksdb::Status s = engine.Open(options, dbpath);
    assert(s.ok());

    // 写入数据
    std::cout << ">>> Writing keys to RocksDB..." << std::endl;
    rocksdb::WriteOptions write_opts;
    std::string value(512, 'x');

    size_t written = 0;
    std::string target_key;
    uint64_t numeric_target = 0;

    while (written < actual_count) {
        size_t remain = actual_count - written;
        size_t cur_batch = (remain < batch_size) ? remain : batch_size;

        std::vector<uint64_t> keys = load_sosd_keys_batch(data_file, written, cur_batch);
        if (keys.empty()) break;

        for (size_t i = 0; i < keys.size(); ++i) {
            std::string formatted = format_key(keys[i]);
            s = engine.Put(write_opts, formatted, value);
            if (!s.ok()) {
                std::cerr << "Put failed at offset " << written + i << ": " << s.ToString() << std::endl;
                return 1;
            }

            // 记录中间位置的键用于后续验证
            if (written + i == actual_count / 2) {
                target_key = formatted;
                numeric_target = keys[i];
            }
        }

        written += cur_batch;
        std::cout << "Progress: " << written << " / " << actual_count << " keys written." << std::endl;
    }
    std::cout << ">>> Write phase completed." << std::endl;

    // 查找目标键所属的 SST 文件
    std::string target_sst_file = FindSSTFileForKey(numeric_target);
    if (target_sst_file.empty()) {
        std::cerr << "[Error] Could not find SST file for target key." << std::endl;
        return 1;
    }
    std::cout << "[Debug] Target key " << target_key << " belongs to SST file: " << target_sst_file << std::endl;

    // 辐射模拟：破坏目标 SST 的 model_v1
    SimulateRadiationForSST(target_sst_file);

    // 读取验证（触发 TMR 表决与自愈）
    std::cout << ">>> Launching target read to trigger TMR Voter..." << std::endl;
    rocksdb::ReadOptions read_opts;
    std::string retrieved_value;
    s = engine.Get(read_opts, target_key, &retrieved_value);
    if (s.ok()) {
        std::cout << ">>> [Success] Data verified! Key: " << target_key
                  << ", Value size: " << retrieved_value.size() << std::endl;
    } else {
        std::cerr << ">>> [Failure] Read failed for key " << target_key
                  << ": " << s.ToString() << std::endl;
    }

    std::cout << ">>> Test finished." << std::endl;
    return 0;
}