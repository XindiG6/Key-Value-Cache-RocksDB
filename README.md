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

# Design

- Slab Management
  - Partition the “flash space” into slabs, each contains multiple 4KB blocks.
  - A slab is represented by a Slab struct with a freeBlocks set.

- Single-level Mapping
  - The application keeps a simple hash map from userKey → (slabID, blockIndex).
  - No device-level FTL mapping is needed.

- Application-driven GC
   - When no free blocks are available, runGC() will fully erase a victim slab (quick clean) based on LRU.

- Dynamic Over-Provisioning (OP)
  - A minimal design that changes watermarks (low/high) to “expand” or “shrink” OP size depending on the number of free slabs.

- RocksDB Emulation
  - Instead of a real open-channel SSD driver, we store each 4KB chunk as a (key, value) pair in RocksDB.
  - RocksDB keys look like &lt;channel ID&gt;:&lt;block ID&gt;.
 
# Implementation

- RocksDBWrapper (Emulate Open Channel SSD)
  - Provides a simple key-value interface to store and retrieve 4KB blocks.
  - Supports PUT, GET, and DELETE operations on RocksDB.
  - Each key-value pair maps to one flash block.

- KeyValueCache (Slab-Based Cache Manager)
  - Manages slabs, allocation, and eviction.
  - Maintains active and free slab queues.
  - Implements GC and OP strategies to optimize flash usage.
  - Tracks access frequency for LRU-based GC.

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
![output (28)](https://github.com/user-attachments/assets/fc1ac8c4-e5a9-4a4e-9bc6-0afc5baf28b5)

Latency </br>
![output (29)](https://github.com/user-attachments/assets/bd410851-fc6d-44b0-88e1-0d5bf70e41c3)


# References
- Paper: Shen, Z. et al. “Optimizing Flash-based Key-value Cache Systems.” HotStorage 2016
- RocksDB: https://github.com/facebook/rocksdb
