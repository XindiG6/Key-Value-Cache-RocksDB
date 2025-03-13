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
        rocksdb::DB::Open(opts, path, &db);
    }
    ~RocksDBWrapper() { delete db; }
    
    void put(const std::string &key, std::string val) {
        if (val.size() > BLOCK_SIZE) {
            val = val.substr(0, BLOCK_SIZE);  // Truncate
        } else if (val.size() < BLOCK_SIZE) {
            val.append(BLOCK_SIZE - val.size(), '\0');  // Pad
        }
        rocksdb::Status s = db->Put(rocksdb::WriteOptions(), key, val);
        if (!s.ok()) {
            std::cerr << "Error in Put: " << s.ToString() << std::endl;
        }
    }
    
    std::string get(const std::string &key) {
        std::string val;
        rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &val);
        if (!s.ok()) {
            return "";  // return empty string if key not found
        }
        if (val.size() < BLOCK_SIZE) {
            val.append(BLOCK_SIZE - val.size(), '\0');  // ensure full 4KB block
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

        // ddjust watermarks dynamically
        if (free_slabs.size() < dynamic_low_wm && dynamic_low_wm < total_slabs / 2) {
            dynamic_low_wm = std::min(static_cast<int>(dynamic_low_wm * growth_factor), total_slabs / 2);
            dynamic_high_wm = std::min(static_cast<int>(dynamic_high_wm * growth_factor), static_cast<int>(total_slabs * 0.9));
        } 
        else if (free_slabs.size() > dynamic_high_wm && dynamic_low_wm > total_slabs * 0.05) {
            dynamic_low_wm = std::max(static_cast<int>(dynamic_low_wm * shrink_factor), static_cast<int>(total_slabs * 0.05));
            dynamic_high_wm = std::max(static_cast<int>(dynamic_high_wm * shrink_factor), static_cast<int>(total_slabs * 0.6));
        }

        // move some free slabs to reserve
        while (free_slabs.size() > dynamic_low_wm * 1.5 && reserve_slabs.size() < max_reserve) {
            std::string slab = free_slabs.front();
            free_slabs.pop_front();
            reserve_slabs.push_back(slab);
        }

        //use reserved slabs first if free slabs are too low
        while (!reserve_slabs.empty() && free_slabs.size() < dynamic_low_wm) {
            std::string slab = reserve_slabs.front();
            reserve_slabs.pop_front();
            free_slabs.push_back(slab);
        }

        // only trigger GC If No Other Option
        if (free_slabs.size() < dynamic_low_wm / 2 && reserve_slabs.empty()) {
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

        // free at least 50% of active slabs or 2000
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
        total_slabs =2700;
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
        //call manage_op() to move from reserve before triggering GC
        manage_op();

        if (free_slabs.empty()) {
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

    void print_stats() const {
        std::cout << "Free slabs: " << free_slabs.size()
                  << " | Active slabs: " << active_slabs.size()
                  << " | Reserved slabs: " << reserve_slabs.size() << "\n";
    }
};


//test functions

void test_gc_impact() {
    std::cout << "\nRunning GC Test\n";
    KeyValueCache cache("/tmp/kvcache_testGC");
    std::string test_value(4096, 'x');
    int num_operations = 10000;
    // Insert until GC is forced
    for (int i = 0; i < num_operations; i++) {
        std::string key = "key_" + std::to_string(i);
        cache.put(key, test_value);
        if (i % 1000 == 0) {
            cache.print_stats(); // Show free slabs vs. active slabs
        }
    }
}

void test_read_write_erase() {
    std::cout << "\nRunning Read-Write-Erase Test\n";

    KeyValueCache cache("/tmp/kvcache_test");

    // Test data
    std::string short_data = "ShortData";
    std::string exact_4kb_data(4096, 'A');  // Exactly 4KB of 'A'
    std::string long_data(5000, 'X');  // 5000 bytes of 'X'

    // Write to cache
    std::cout << "Writing short data to key1...\n";
    cache.put("key1", short_data);

    std::cout << "Writing exact 4KB data to key2...\n";
    cache.put("key2", exact_4kb_data);

    std::cout << "Writing long data (5000 bytes) to key3...\n";
    cache.put("key3", long_data);

    // Read from cache
    std::string read_short = cache.get("key1");
    std::string read_exact = cache.get("key2");
    std::string read_long = cache.get("key3");

    // Verify Data size
    std::cout << " Checking stored data sizes...\n";
    std::cout << "   - key1 size: " << read_short.size() << " bytes (Expected: 4KB)\n";
    std::cout << "   - key2 size: " << read_exact.size() << " bytes (Expected: 4KB)\n";
    std::cout << "   - key3 size: " << read_long.size() << " bytes (Expected: 4KB)\n";

    if (read_short.size() == 4096 && read_exact.size() == 4096 && read_long.size() == 4096) {
        std::cout << "Size verification PASSED!\n";
    } else {
        std::cout << "Size verification FAILED!\n";
    }

    // Erase a Key and Verify
    std::cout << "Erasing key1...\n";
    cache.del("key1");

    if (cache.get("key1").empty()) {
        std::cout << "Erase test PASSED!\n";
    } else {
        std::cout << "Erase test FAILED!\n";
    }

    // Print cache stats
    cache.print_stats();
}

int main() {
    test_read_write_erase();
    test_gc_impact();
    return 0;
}