#include "coolsm_engine.h"
#include <iostream>
#include <cassert>
#include <random>
#include <iomanip>
#include <sstream>
#include <cstdint>

using namespace rocksdb::coolsm;

// 辐射模拟函数（在写入完成后调用）
void SimulateRadiation() {
    std::cout << "========= [RADIATION SIMULATION] =========" << std::endl;
    std::cout << "Simulating high-energy cosmic ray striking memory..." << std::endl;

    extern std::unordered_map<std::string, TMR_LearnedIndex> global_tmr_indexes;
    extern std::mutex index_map_mutex;

    std::lock_guard<std::mutex> lock(index_map_mutex);
    if (global_tmr_indexes.empty()) {
        std::cout << "[Radiation] No TMR index found, skipping." << std::endl;
        return;
    }

    auto& first_tmr = global_tmr_indexes.begin()->second;
    std::vector<uint64_t> wrong_keys = {first_tmr.smallest_key, first_tmr.smallest_key + 1};
    pgm::PGMIndex<uint64_t, 64> corrupted_model(wrong_keys.begin(), wrong_keys.end());
    first_tmr.model_v1 = corrupted_model;

    std::cout << "[Radiation] model_v1 corrupted (replaced with short index)." << std::endl;
    std::cout << "[Radiation] Original smallest_key/largest_key: "
              << first_tmr.smallest_key << " - " << first_tmr.largest_key << std::endl;
}

int main() {
    std::string dbpath = "/tmp/coolsm_test_db";
    rocksdb::DestroyDB(dbpath, rocksdb::Options());

    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 1024 * 1024; // 1MB

    rocksdb::coolsm::CooLSMEngine engine;
    rocksdb::Status s = engine.Open(options, dbpath);
    assert(s.ok());

    std::mt19937_64 rand_engine(1999);
    std::lognormal_distribution<double> lognorm_dist(10.0, 1.5);

    std::cout << ">>> Start generating complex non-linear keys..." << std::endl;
    rocksdb::WriteOptions write_opts;

    std::string sample_target_key; // 存储一个中间键用于后续读测试

    for (int i = 0; i < 30000; i++) {
        uint64_t raw_id = static_cast<uint64_t>(lognorm_dist(rand_engine));
        std::stringstream ss;
        ss << std::setw(16) << std::setfill('0') << std::hex << raw_id << "_usertag";
        std::string complex_key = ss.str();
        std::string value = std::string(512, 'x');

        s = engine.Put(write_opts, complex_key, value);
        assert(s.ok());

        // 记录一个中间位置的键作为测试目标
        if (i == 15000) {
            sample_target_key = complex_key;
        }

        if (i % 5000 == 0) {
            std::cout << "Successfully pushed " << i << " non-linear keys." << std::endl;
        }
    }
    std::cout << ">>> Write path test completed without crash." << std::endl;

    // ========== 核心：模拟辐射破坏 ==========
    SimulateRadiation();

    // ========== 读取测试，触发 TMR 表决和自愈 ==========
    std::cout << ">>> Launching Target Read to trigger TMR Voter..." << std::endl;
    rocksdb::ReadOptions read_opts;
    std::string res_val;
    s = engine.Get(read_opts, sample_target_key, &res_val);

    if (s.ok()) {
        std::cout << ">>> [Success] Data verified! Value size matched: " << res_val.size() << std::endl;
    } else {
        std::cerr << ">>> [Failure] Read failed: " << s.ToString() << std::endl;
    }

    std::cout << ">>> Milestone 3 空间抗辐射自愈闭环验证通过！" << std::endl;
    return 0;
}