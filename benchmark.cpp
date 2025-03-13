#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <memory>
#include <chrono>
#include <random>
#include <vector>
#include <rocksdb/db.h>

const int BLOCK_SIZE = 4096; 

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
        opts.compression = rocksdb::kNoCompression;  // Disable compression
        opts.disable_auto_compactions = true;  // Disable compaction
        rocksdb::DB::Open(opts, path, &db);
    }
    ~RocksDBWrapper() { delete db; }

    void put(const std::string &key, std::string val) {
        if (val.size() > BLOCK_SIZE) {
            val = val.substr(0, BLOCK_SIZE);  // Truncate
        } else if (val.size() < BLOCK_SIZE) {
            val.append(BLOCK_SIZE - val.size(), '\0');  // Pad
        }
        rocksdb::WriteOptions write_opts;
        write_opts.disableWAL = true;  // Disable WAL to reduce overhead
        db->Put(write_opts, key, val);
    }
    
    std::string get(const std::string &key) {
        std::string val;
        rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &val);
        if (!s.ok()) {
            return "";  // Return empty string if key not found
        }
        if (val.size() < BLOCK_SIZE) {
            val.append(BLOCK_SIZE - val.size(), '\0');  // Ensure full 4KB block
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
    const int NUM_CHANNELS = 2;
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

        int max_reserve = total_slabs * 0.20; // Max 20% in reserve
        int min_reserve = total_slabs * 0.10; // Maintain at least 10% of slabs in reserve

        int prev_low_wm = dynamic_low_wm;
        int prev_high_wm = dynamic_high_wm;

        // Adjust watermarks dynamically
        if (free_slabs.size() < dynamic_low_wm && dynamic_low_wm < total_slabs / 2) {
            dynamic_low_wm = std::min(static_cast<int>(dynamic_low_wm * growth_factor), total_slabs / 2);
            dynamic_high_wm = std::min(static_cast<int>(dynamic_high_wm * growth_factor), static_cast<int>(total_slabs * 0.9));
        } 
        else if (free_slabs.size() > dynamic_high_wm && dynamic_low_wm > total_slabs * 0.05) {
            dynamic_low_wm = std::max(static_cast<int>(dynamic_low_wm * shrink_factor), static_cast<int>(total_slabs * 0.05));
            dynamic_high_wm = std::max(static_cast<int>(dynamic_high_wm * shrink_factor), static_cast<int>(total_slabs * 0.6));
        }

        while (free_slabs.size() > dynamic_low_wm * 1.5 && reserve_slabs.size() < max_reserve) {
            std::string slab = free_slabs.front();
            free_slabs.pop_front();
            reserve_slabs.push_back(slab);
            //std::cout << "Moving slab to reserve: " << slab << "\n";
        }

        while (!reserve_slabs.empty() && free_slabs.size() < dynamic_low_wm) {
            std::string slab = reserve_slabs.front();
            reserve_slabs.pop_front();
            free_slabs.push_back(slab);
            //std::cout << "Reusing slab from reserve: " << slab << "\n";
        }

        if (free_slabs.size() < dynamic_low_wm / 2 && reserve_slabs.empty()) {
            //std::cout << " CRITICAL: Free & Reserve slabs too low! Triggering GC...\n";
            gc();
        }
    }

    int gc_invoked_count = 0;  // Global or class member variable
        
    void gc() {
        gc_invoked_count++;  
        int slabs_freed = 0;

        if (lru.empty()) {
            std::cout << "GC: No slabs available for collection.\n";
            return;
        }

        int gc_threshold = std::max((int)(active_slabs.size() * 0.50), 3000);
        
        //std::cout << " GC Invocation #" << gc_invoked_count 
                //<< ": Attempting to free " << gc_threshold << " slabs...\n";

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

        //std::cout << " GC Completed. Freed " << slabs_freed << " slabs.\n";
        //std::cout << "Total GC Invocations: " << gc_invoked_count << "\n";
    }



public:
    KeyValueCache(const std::string &db_path) 
        : db(std::make_unique<RocksDBWrapper>(db_path)) {
        total_slabs =2000;
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
            std::cout << "no free slabs available after manage_op(). exiting!\n";
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


    int hit_count = 0, miss_count = 0;
    std::string get(const std::string &key) {
        if (!kv_map.count(key)) {
            miss_count++;
            return "";
        }
        hit_count++;
        auto &[slab, idx] = kv_map[key];
        auto s = slabs[slab];
        lru.erase(s->lru);
        s->lru = std::chrono::steady_clock::now();
        lru[s->lru] = slab;
        return db->get(block_key(slab, idx));
    }

    void del(const std::string &key) {
        if (kv_map.count(key)) {
            auto &[slab, idx] = kv_map[key];

            // Free the allocated block
            slabs[slab]->free(idx);

            // Remove from LRU tracking
            lru.erase(slabs[slab]->lru);

            // Delete from RocksDB
            db->del(block_key(slab, idx));

            // Remove from kv_map
            kv_map.erase(key);
        }
    }

    void print_hit_ratio() const {
        int total = hit_count + miss_count;
        if (total == 0) return;
        std::cout << "Cache Hit Ratio: " << (hit_count * 100.0 / total) << "%\n";
    }
    void print_stats() const {
        std::cout << "Free slabs: " << free_slabs.size()
                  << " | Active slabs: " << active_slabs.size()
                  << " | Reserved slabs: " << reserve_slabs.size() << "\n";
    }
};

void test_average_latency_and_throughput(KeyValueCache &cache, int num_operations, size_t object_size) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> key_dist(0, num_operations - 1);
    std::string test_value(object_size, 'x'); 

    double total_put_latency = 0;
    double total_get_latency = 0;

    // Measure PUT throughput and average latency
    auto start_put = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_operations; i++) {
        std::string key = "key_" + std::to_string(i);  // ✅ 确保每次写入唯一 key
        auto op_start = std::chrono::high_resolution_clock::now();
        cache.put(key, test_value);
        auto op_end = std::chrono::high_resolution_clock::now();
        total_put_latency += std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
    }
    auto end_put = std::chrono::high_resolution_clock::now();
    double put_throughput = num_operations / std::chrono::duration<double>(end_put - start_put).count();
    double avg_put_latency = total_put_latency / num_operations;

    // Measure GET throughput and average latency
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

    // Output results
    std::cout << "PUT Throughput: " << put_throughput << " ops/sec\n";
    std::cout << "GET Throughput: " << get_throughput << " ops/sec\n";
    std::cout << "Average PUT Latency: " << avg_put_latency << " µs\n";
    std::cout << "Average GET Latency: " << avg_get_latency << " µs\n";
}

void test_cache_hit_ratio(KeyValueCache &cache, int num_operations) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> key_dist(0, num_operations - 1);
    std::string test_value(4096, 'x');  

    // Insert values
    for (int i = 0; i < num_operations; i++) {
        std::string key = "key_" + std::to_string(i);
        cache.put(key, test_value);
    }

    // Random GETs
    int hits = 0, misses = 0;
    for (int i = 0; i < num_operations; i++) {
        std::string key = "key_" + std::to_string(key_dist(gen));
        if (!cache.get(key).empty()) {
            hits++;
        } else {
            misses++;
        }
    }

    double hit_ratio = (hits * 100.0) / (hits + misses);
    std::cout << "Cache Hit Ratio: " << hit_ratio << "%\n";
}

void test_gc_impact(KeyValueCache &cache, int num_operations) {
    std::string test_value(4096, 'x');

    // Insert until GC is forced
    for (int i = 0; i < num_operations; i++) {
        std::string key = "key_" + std::to_string(i);
        cache.put(key, test_value);
        if (i % 1000 == 0) {
            cache.print_stats(); // Show free slabs vs. active slabs
        }
    }

    // Trigger GC manually and observe its effect
    //std::cout << "Triggering GC...\n";
    //cache.trigger_gc_op();
    //cache.print_stats();
}

int main() {
    std::cout << "=== Initializing RocksDB-based Key-Value Cache ===\n";
    KeyValueCache cache("/tmp/kvcache4");

    int num_operations = 1000000;
    std::vector<int> object_sizes = {256, 512, 1024, 2048, 4096};  // Different sizes for testing

    std::cout << "\n=== Running Throughput and Latency Tests ===\n";
    for (int obj_size : object_sizes) {
       std::cout << "\nTesting with object size: " << obj_size << " bytes\n";
       test_average_latency_and_throughput(cache, num_operations, obj_size);
    }

    //std::cout << "\n=== Running Cache Hit Ratio Test ===\n";
    //test_cache_hit_ratio(cache, num_operations);

    //std::cout << "\n=== Running Garbage Collection Impact Test ===\n";
    //test_gc_impact(cache, num_operations);

    //std::cout << "\n=== Final Cache Stats ===\n";
    //cache.print_stats();

    return 0;
}

