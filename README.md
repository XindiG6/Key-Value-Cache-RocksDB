# Key-Value-Cache-RocksDB
This repository demonstrates a simplified implementation of a flash-based key-value cache system with RocksDB. Basic design is inspired by the “Optimizing Flash-based Key-value Cache Systems” paper.

cpp files:
 - prototype: Single write. Simple demo for basic write, read, erase operations and evaluations for system performance based on simple GC and OPS management.
 - flash-kv-cache: Enable put in batch. To achieve or compete with the preliminary results in the paper.

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
Running Read-Write-Erase Test </br>
Writing short data to key1... </br>
Writing exact 4KB data to key2...</br>
Writing long data (5000 bytes) to key3...</br>
Checking stored data sizes...</br>
key1 size: 4096 bytes (Expected: 4KB)</br>
key2 size: 4096 bytes (Expected: 4KB)</br>
key3 size: 4096 bytes (Expected: 4KB)</br>
Size verification PASSED!</br>
Erasing key1...</br>
Erase test PASSED!</br>
Free slabs: 2157 | Active slabs: 3 | Reserved slabs: 540</br>

Running GC Test</br>
Free slabs: 2159 | Active slabs: 1 | Reserved slabs: 540</br>
Free slabs: 1159 | Active slabs: 1001 | Reserved slabs: 540</br>
Free slabs: 159 | Active slabs: 2001 | Reserved slabs: 540</br>
Free slabs: 1349 | Active slabs: 1001 | Reserved slabs: 350</br>

# Evaluation Results
Setups:
blocksPerSlab=128, numChannels=2, overprovisionRatio=0.2, Operations = 1x10^6 </br>

Throughput</br>
![throughtputs](https://github.com/user-attachments/assets/96a5e19c-3110-4848-9521-c176c30c5289)

Latency </br>
![latencys](https://github.com/user-attachments/assets/fdc812b1-0a6a-4c82-b7e4-6c29d3047225)

cache hit rate and GC counts</br>
![output (32)](https://github.com/user-attachments/assets/dcaa0848-29fb-42b7-905c-ec5896cee67f)

Enable put in batch. Setups: batch size = 32, blocksPerSlab=128, numChannels=12, overprovisionRatio=0.2, Operations = 1x10^6, numSlabs = 192
![output (41)](https://github.com/user-attachments/assets/732ea2f5-0c50-48e7-a82f-dcdb21a03d71)


# References
- Paper: Shen, Z. et al. “Optimizing Flash-based Key-value Cache Systems.” HotStorage 2016
- RocksDB: https://github.com/facebook/rocksdb
