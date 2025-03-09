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
Managing OP: Free slabs = 100, Reserve slabs = 0, Active slabs = 0
Post OP management: Free slabs = 80, Reserve slabs: 20, Active slabs: 0

=== Running Randomized GC & OP Test with Object Size ===
Generating 10000 random operations with variable object size...
Free slabs: 0 | Active slabs: 80
Triggering GC due to low free slabs...
Managing OP: Free slabs = 0, Reserve slabs = 20, Active slabs = 80
Post OP management: Free slabs = 30, Reserve slabs: 0, Active slabs: 71
Free slabs: 30 | Active slabs: 71
Checking if old keys are deleted after GC...
Deleted keys after GC: 6524/10000
Adding new keys after GC to check reallocation...
Free slabs: 0 | Active slabs: 101

# Evaluation Results
Setups:
blocksPerSlab=128, numChannels=2, overprovisionRatio=0.2, Operations = 30000 </br>

Set Throughput</br>
![set-vs-slab](https://github.com/user-attachments/assets/dae28616-6d53-4d8b-9540-4a75ff1ee9a2)

![set](https://github.com/user-attachments/assets/fed33ca8-5495-4488-b389-9ce56fd10b89)

Set Latency </br>
![set-latency](https://github.com/user-attachments/assets/33814024-85ed-4fae-ad88-be01c419c1aa)


# References
- Paper: Shen, Z. et al. “Optimizing Flash-based Key-value Cache Systems.” HotStorage 2016
- RocksDB: https://github.com/facebook/rocksdb
