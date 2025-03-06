# Key-Value-Cache-RocksDB
This repository demonstrates a simplified implementation of a flash-based key-value cache system inspired by the “Optimizing Flash-based Key-value Cache Systems” paper.

It uses RocksDB to emulate the underlying open-channel SSD, while focusing on:
- Slab-based space management
- Single-level mapping (key → (slab, block))
- Application-driven GC (LRU based quick clean)
- A minimal Over-Provisioning design (watermarks)
  
Note: This prototype is purely for educational & demonstration purposes. It does not represent a production-grade solution.

# Background
When building TB-level cache systems for large-scale LLM (e.g., storing or paging out vLLM pages, retrieving doc embeddings in RAG scenarios), efficient flash-based KV caches are critical for:

- Cost: DRAM is expensive; flash storage offers a cheaper memory tier.
- Performance: Minimizing write amplification & GC overhead is essential for stable throughput.
- Scalability: Dynamic Over-Provisioning can adapt to changing write loads.

Following the paper’s architecture, moving GC and mapping to the application layer eliminates the redundant FTL overhead in commercial SSDs. In this simplified version, we simulate open-channel SSD behavior via RocksDB for easy portability.

# Implementation
- Slab Management
  - Partition the “flash space” into slabs, each contains multiple 4KB blocks.
  - A slab is represented by a Slab struct with a freeBlocks set.

- Single-level Mapping
  - The application keeps a simple hash map from userKey → (slabID, blockIndex).
  - No device-level FTL mapping is needed.

- Application-driven GC
   - When no free blocks are available, runGC() will fully erase a victim slab (quick clean).

- Dynamic Over-Provisioning (OP)
  - A minimal design that changes watermarks (low/high) to “expand” or “shrink” OP size depending on the number of free slabs.

- RocksDB Emulation
  - Instead of a real open-channel SSD driver, we store each 4KB chunk as a (key, value) pair in RocksDB.
  - RocksDB keys look like &lt;channel ID&gt;:&lt;block ID&gt;.

# Dependencies
- C++17 compiler
- Prepare for the dependencies of RocksDB: https://github.com/facebook/rocksdb/blob/main/INSTALL.md
- RocksDB

Linux:
```
git clone https://github.com/facebook/rocksdb.git
cd rocksdb
make shared_lib
sudo make install
```
MacOS with Homebrew:
```
brew install rocksdb
```
# Build & Run
If install RocksDB with homebrew:
```
g++ -o flash_kv_cache flash_kv_cache.cpp \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib -lrocksdb -std=c++17
./flash_kv_cache
```
If install RocksDB locally:
```
g++ -o flash_kv_cache flash_kv_cache.cpp -lrocksdb -std=c++17
./flash_kv_cache
```

Or replace the path of -I and -L (where the RocksDB is installed):
```
g++ -o flash_kv_cache flash_kv_cache.cpp -I/usr/local/include -L/usr/local/lib -lrocksdb -std=c++17
./flash_kv_cache
```

# Evaluation Results
Set Throughput

![Figure_1](https://github.com/user-attachments/assets/9f342b87-cdba-44df-b94b-4fa989b3e9e4)

![output](https://github.com/user-attachments/assets/4373d5ac-df89-4a60-aed2-1c9add459bf4)

GC Invoked Count
![gc_counts](https://github.com/user-attachments/assets/64238169-de70-4d20-8001-5aa83e914ee4)





# References
- Paper: Shen, Z. et al. “Optimizing Flash-based Key-value Cache Systems.” HotStorage 2016
- RocksDB: https://github.com/facebook/rocksdb
