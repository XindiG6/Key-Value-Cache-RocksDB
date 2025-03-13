#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <memory>
#include <chrono>
#include <random>
#include <vector>
#include <filesystem>
#include <rocksdb/db.h>


const int BLOCK_SIZE = 4096; 
const int BATCH_SIZE = 32;

struct Slab {
    std::string id;
    int channel;
    std::unordered_set<int> free_blocks;
    std::chrono::steady_clock::time_point lru;

    Slab(const std::string &id, int chan, int blocks) 
        : id(id), channel(chan), lru(std::chrono::steady_clock::now()) {
        for(int i = 0; i < blocks; i++) free_blocks.insert(i);
    }

    int alloc() {
        if (free_blocks.empty()) return -1;
        auto it = free_blocks.begin();
        int idx = *it;
        free_blocks.erase(it);
        lru = std::chrono::steady_clock::now();
        return idx;
    }

    void free(int idx) {
        free_blocks.insert(idx);
        lru = std::chrono::steady_clock::now();
    }
};

class RocksDBWrapper {
    rocksdb::DB* db;
public:
    explicit RocksDBWrapper(const std::string& path) {
        rocksdb::Options opts;
        opts.create_if_missing = true;
        opts.compression = rocksdb::kNoCompression;  // No compaction
        opts.disable_auto_compactions = true;  // Disable compaction
        rocksdb::DB::Open(opts, path, &db); 
    }
    ~RocksDBWrapper() { delete db; }
    
    void put(const std::string &key, std::string val) {
        if (val.size() > BLOCK_SIZE) {
            val = val.substr(0, BLOCK_SIZE);
        } else if (val.size() < BLOCK_SIZE) {
            val.append(BLOCK_SIZE - val.size(), '\0');
        }
        rocksdb::Status s = db->Put(rocksdb::WriteOptions(), key, val);
        if (!s.ok()) {
            std::cerr << "Error in Put: " << s.ToString() << std::endl;
        }
    }

    void batch_put(const std::vector<std::pair<std::string, std::string>>& kv_pairs) {
        rocksdb::WriteBatch batch;
        for (const auto& [key, val] : kv_pairs) {
            std::string new_val = val;
            if (new_val.size() > BLOCK_SIZE) {
                new_val = new_val.substr(0, BLOCK_SIZE);
            } else if (new_val.size() < BLOCK_SIZE) {
                new_val.append(BLOCK_SIZE - new_val.size(), '\0');
            }
            batch.Put(key, new_val);
        }
        rocksdb::Status s = db->Write(rocksdb::WriteOptions(), &batch);
        if (!s.ok()) {
            std::cerr << "Error in Batch Put: " << s.ToString() << std::endl;
        }
    }
    
    std::string get(const std::string &key) {
        std::string val;
        rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &val);
        if (!s.ok()) {
            return "";
        }
        if (val.size() < BLOCK_SIZE) {
            val.append(BLOCK_SIZE - val.size(), '\0');
        }
        return val;
    }

    void del(const std::string &key) {
        rocksdb::Status s = db->Delete(rocksdb::WriteOptions(), key);
        if (!s.ok()) {
            std::cerr << "Error in Delete: " << s.ToString() << std::endl;
        }
    }

};

class KeyValueCache {
    std::unique_ptr<RocksDBWrapper> db;
    std::unordered_map<std::string, std::pair<std::string, int>> kv_map;
    std::unordered_map<std::string, std::shared_ptr<Slab>> slabs;
    std::deque<std::string> free_slabs, active_slabs, reserve_slabs;
    std::map<std::chrono::steady_clock::time_point, std::string> lru;

    const int BLOCKS_PER_SLAB = 128;
    const int NUM_CHANNELS = 12;
    const double OP_RATIO = 0.2;
    int total_slabs, low_wm, high_wm;

    std::string block_key(const std::string &slab, int idx) {
        return slab + ":" + std::to_string(idx);
    }

    void manage_op() {
        static int dynamic_low_wm = total_slabs * 0.15;
        static int dynamic_high_wm = total_slabs * 0.75;
        const double growth_factor = 2.0;
        const double shrink_factor = 0.9;

        int max_reserve = total_slabs * 0.20; // max 20% in reserve
        int min_reserve = total_slabs * 0.10; // maintain at least 10% of slabs in reserve

        int prev_low_wm = dynamic_low_wm;
        int prev_high_wm = dynamic_high_wm;

        // adjust watermarks dynamically
        if (free_slabs.size() < dynamic_low_wm && dynamic_low_wm < total_slabs / 2) {
            dynamic_low_wm = std::min(static_cast<int>(dynamic_low_wm * growth_factor), total_slabs / 2);
            dynamic_high_wm = std::min(static_cast<int>(dynamic_high_wm * growth_factor), static_cast<int>(total_slabs * 0.9));
        } 
        else if (free_slabs.size() > dynamic_high_wm && dynamic_low_wm > total_slabs * 0.05) {
            dynamic_low_wm = std::max(static_cast<int>(dynamic_low_wm * shrink_factor), static_cast<int>(total_slabs * 0.05));
            dynamic_high_wm = std::max(static_cast<int>(dynamic_high_wm * shrink_factor), static_cast<int>(total_slabs * 0.6));
        }

        // move some free slabs to reserve (ENSURE RESERVE FILLS)
        while (free_slabs.size() > dynamic_low_wm * 1.5 && reserve_slabs.size() < max_reserve) {
            std::string slab = free_slabs.front();
            free_slabs.pop_front();
            reserve_slabs.push_back(slab);
            //std::cout << "Moving slab to reserve: " << slab << "\n";
        }

        //use reserved slabs first if free slabs are too low
        while (!reserve_slabs.empty() && free_slabs.size() < dynamic_low_wm) {
            std::string slab = reserve_slabs.front();
            reserve_slabs.pop_front();
            free_slabs.push_back(slab);
        }

        if (free_slabs.size() < dynamic_low_wm / 2 && reserve_slabs.empty()) {
            gc();
        }
    }

    int gc_invoked_count = 0;  // global or class member variable
        
    void gc() {
        gc_invoked_count++;  
        int slabs_freed = 0;

        if (lru.empty()) {
            std::cout << "GC: No slabs available for collection.\n";
            return;
        }

        // calculate dynamic GC threshold: free at least 50% of active slabs or 2000
        int gc_threshold = std::max((int)(active_slabs.size() * 0.50), 2000);

        while (!lru.empty() && slabs_freed < gc_threshold) {
            auto victim = lru.begin()->second;
            lru.erase(lru.begin());
            auto s = slabs[victim];

            for (int i = 0; i < BLOCKS_PER_SLAB; i++) {
                if (!s->free_blocks.count(i)) {
                    std::string key_to_delete = block_key(victim, i);
                    db->del(key_to_delete);
                    s->free(i);
                }
            }

            auto it = std::find(active_slabs.begin(), active_slabs.end(), victim);
            if (it != active_slabs.end()) {
                active_slabs.erase(it);
            }

            free_slabs.push_back(victim);
            slabs_freed++;
        }
    }

public:
    KeyValueCache(const std::string &db_path) 
        : db(std::make_unique<RocksDBWrapper>(db_path)) {
        total_slabs =192;
        for (int i = 0; i < total_slabs; i++) {
            auto sid = "slab_" + std::to_string(i);
            slabs[sid] = std::make_shared<Slab>(sid, i % NUM_CHANNELS, BLOCKS_PER_SLAB);
            free_slabs.push_back(sid);
        }
        manage_op();
    }

    void trigger_gc_op() {
        manage_op();  // Call the private function
    }

    void put(const std::string &key, const std::string &val) {
        if (kv_map.count(key)) {
            auto &[slab, idx] = kv_map[key];
            slabs[slab]->free(idx);
            lru.erase(slabs[slab]->lru);
        }
        manage_op();

        if (free_slabs.empty()) {
            std::cout << "🚨 No free slabs available after manage_op(). Exiting!\n";
            return;
        }

        std::string slab_id = free_slabs.front();
        free_slabs.pop_front();
        active_slabs.push_back(slab_id);

        int block_idx = slabs[slab_id]->alloc();
        db->put(block_key(slab_id, block_idx), val);
        kv_map[key] = {slab_id, block_idx};
        lru[slabs[slab_id]->lru] = slab_id;
    }

    void batch_put(const std::vector<std::pair<std::string, std::string>>& kv_pairs) {
        db->batch_put(kv_pairs);
    }

    std::string get(const std::string &key) {
        if (!kv_map.count(key)) {
            return "";
        }
        auto &[slab, idx] = kv_map[key];
        auto s = slabs[slab];
        lru.erase(s->lru);
        s->lru = std::chrono::steady_clock::now();
        lru[s->lru] = slab;
        return db->get(block_key(slab, idx));
    }
};

void test_batch_latency_and_throughput(KeyValueCache &cache, int num_operations, size_t object_size) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> key_dist(0, num_operations - 1);
    std::string test_value(object_size, 'x');

    double total_batch_put_latency = 0;
    double total_get_latency = 0;

    // batch put
    auto start_put = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_operations; i += BATCH_SIZE) {
        std::vector<std::pair<std::string, std::string>> batch;
        for (int j = 0; j < BATCH_SIZE && (i + j) < num_operations; j++) {
            std::string key = "key_" + std::to_string(key_dist(gen));
            batch.emplace_back(key, test_value);
        }
        auto op_start = std::chrono::high_resolution_clock::now();
        cache.batch_put(batch);
        auto op_end = std::chrono::high_resolution_clock::now();
        total_batch_put_latency += std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
    }
    auto end_put = std::chrono::high_resolution_clock::now();
    double put_throughput = num_operations / std::chrono::duration<double>(end_put - start_put).count();
    double avg_put_latency = total_batch_put_latency / (num_operations / BATCH_SIZE);

    auto start_get = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_operations; i++) {
        std::string key = "key_" + std::to_string(key_dist(gen));
        auto op_start = std::chrono::high_resolution_clock::now();
        cache.get(key);
        auto op_end = std::chrono::high_resolution_clock::now();
        total_get_latency += std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
    }
    auto end_get = std::chrono::high_resolution_clock::now();
    double get_throughput = num_operations / std::chrono::duration<double>(end_get - start_get).count();
    double avg_get_latency = total_get_latency / num_operations;

    //results
    std::cout << "BATCH PUT Throughput: " << put_throughput << " ops/sec\n";
    std::cout << "Average BATCH PUT Latency: " << avg_put_latency << " µs\n";
    std::cout << "GET Throughput: " << get_throughput << " ops/sec\n";
    std::cout << "Average GET Latency: " << avg_get_latency << " µs\n";
}

int main() {
    std::cout << "=== Initializing RocksDB-based Key-Value Cache with BATCH PUT ===\n";
    KeyValueCache cache("/tmp/kvcache");

    int num_operations = 1000000;
    size_t object_size = 256;

    std::cout << "\n=== Running Batch PUT Throughput and Latency Tests ===\n";
    test_batch_latency_and_throughput(cache, num_operations, object_size);

    std::cout << "\n=== Cleaning up RocksDB database ===\n";
    cache.~KeyValueCache();
    std::filesystem::remove_all("/tmp/kvcache");

    return 0;
}