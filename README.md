# Key-Value-Cache-RocksDB
This repository demonstrates a simplified implementation of a flash-based key-value cache system inspired by the “Optimizing Flash-based Key-value Cache Systems” paper.

It uses RocksDB to emulate the underlying open-channel SSD, while focusing on:
- Slab-based space management
- Single-level mapping (key → (slab, block))
- Application-driven GC (quick clean)
- A minimal Over-Provisioning design (watermarks)
  
Note: This prototype is purely for educational & demonstration purposes. It does not represent a production-grade solution.

# Background
When building TB-level cache systems for large-scale LLM (e.g., storing or paging out vLLM pages, retrieving doc embeddings in RAG scenarios), efficient flash-based KV caches are critical for:

- Cost: DRAM is expensive; flash storage offers a cheaper memory tier.
- Performance: Minimizing write amplification & GC overhead is essential for stable throughput.
- Scalability: Dynamic Over-Provisioning can adapt to changing write loads.

Following the paper’s architecture, moving GC and mapping to the application layer eliminates the redundant FTL overhead in commercial SSDs. In this simplified version, we simulate open-channel SSD behavior via RocksDB for easy portability.

# Main Features
Slab-based Key-Value Cache

We partition the “flash space” into slabs, each containing multiple 4KB blocks.
A slab is represented by a Slab struct with a freeBlocks set.
Single-level Mapping
