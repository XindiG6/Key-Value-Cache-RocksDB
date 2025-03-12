# Key-Value-Cache-RocksDB
This repository demonstrates a simplified implementation of a flash-based key-value cache system with RocksDB. Basic design is inspired by the “Optimizing Flash-based Key-value Cache Systems” paper.

# Background
Designing an extremely large KVCache system, that can support TB-level vLLM pages offloaded from GPU memory or CPU memory to SSDs during the LLM inference. At the same time, the cache can also support RAG and other LLM-related applications. Therefore, it is important to design an efficient flash-based cache framework for large-scale LLMs. 
Following the paper’s architecture, moving GC and mapping to the application layer eliminates the redundant FTL overhead in commercial SSDs. In this simplified version, open-channel SSD behavior is simulated via RocksDB.

Why use RocksDB:
- RocksDB makes it significantly easier to build a high-performance key-value cache without worrying about raw SSD constraints, simply focus on just cache logic.
- Great flexibility and portability, making the cache system deployable across different infrastructures (local, cloud, and disaggregated systems).

# Design
- Slab Management
  - Partition the "flash space" into slabs, each containing multiple 4KB blocks.
  - slab is represented by a Slab struct with a free_blocks set for tracking available blocks.

- Single-Level Mapping
  - The application maintains a simple hash map from userKey → <slab ID, block number>.
  - No device-level Flash Translation Layer (FTL) mapping is needed, as RocksDB handles storage abstraction.

- Application-Driven GC
  - When no free blocks are available, GC will fully erase a victim slab (quick clean) based on the Least Recently Used (LRU) policy.

- Dynamic Over-Provisioning (OP)
  - A minimal design that dynamically adjusts watermarks (low/high) to expand or shrink OP size based on the number of free slabs.
  - Ensures efficient memory usage while maintaining performance.

- RocksDB Emulation
  - Instead of a real open-channel SSD driver, each 4KB chunk is stored as a (key, value) pair in RocksDB.
  - RocksDB keys are formatted as <slab ID>:<block number>, aligning with the slab-based memory management in this implementation.

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

# Expected Outputs
Managing OP: Free slabs = 100, Reserve slabs = 0, Active slabs = 0</br>
Post OP management: Free slabs = 80, Reserve slabs: 20, Active slabs: 0</br>

=== Running Randomized GC & OP Test with Object Size ===</br>
Generating 10000 random operations with variable object size...</br>
Free slabs: 0 | Active slabs: 80</br>
Triggering GC due to low free slabs...</br>
Managing OP: Free slabs = 0, Reserve slabs = 20, Active slabs = 80</br>
Post OP management: Free slabs = 30, Reserve slabs: 0, Active slabs: 71</br>
Free slabs: 30 | Active slabs: 71</br>
Checking if old keys are deleted after GC...</br>
Deleted keys after GC: 6524/10000</br>
Adding new keys after GC to check reallocation...</br>
Free slabs: 0 | Active slabs: 101</br>

# Evaluation Results
Setups:
blocksPerSlab=128, numChannels=2, overprovisionRatio=0.2, Operations = 100000 </br>

Throughput</br>
![throughtputs](https://github.com/user-attachments/assets/96a5e19c-3110-4848-9521-c176c30c5289)

Latency </br>
![latencys](https://github.com/user-attachments/assets/fdc812b1-0a6a-4c82-b7e4-6c29d3047225)



# References
- Paper: Shen, Z. et al. “Optimizing Flash-based Key-value Cache Systems.” HotStorage 2016
- RocksDB: https://github.com/facebook/rocksdb
