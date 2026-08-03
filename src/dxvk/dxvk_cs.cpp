#include "dxvk_cs.h"

#include <cstdlib>

namespace dxvk {

  namespace {

    // Default to producer sharding in Helios. Setting this to zero selects
    // only shard zero and reproduces the former single-pool mutex behavior.
    bool heliosCsPoolSharding() {
      static const bool enabled = [] {
        const char* env = std::getenv("HELIOS_DXVK_CS_POOL_SHARDING");
        return !(env && env[0] == '0');
      }();

      return enabled;
    }

  }
  
  DxvkCsChunk::DxvkCsChunk() {
    
  }
  
  
  DxvkCsChunk::~DxvkCsChunk() {
    this->reset();
  }
  
  
  void DxvkCsChunk::init(DxvkCsChunkFlags flags) {
    m_flags = flags;
  }


  void DxvkCsChunk::executeAll(DxvkContext* ctx) {
    auto cmd = m_head;
    
    if (m_flags.test(DxvkCsChunkFlag::SingleUse)) {
      m_commandOffset = 0;
      
      while (cmd != nullptr) {
        auto next = cmd->next();
        cmd->exec(ctx);
        cmd->~DxvkCsCmd();
        cmd = next;
      }

      m_head = nullptr;
      m_next = &m_head;
    } else {
      while (cmd != nullptr) {
        cmd->exec(ctx);
        cmd = cmd->next();
      }
    }
  }
  
  
  void DxvkCsChunk::reset() {
    auto cmd = m_head;

    while (cmd != nullptr) {
      auto next = cmd->next();
      cmd->~DxvkCsCmd();
      cmd = next;
    }
    
    m_head = nullptr;
    m_next = &m_head;

    m_commandOffset = 0;
  }
  
  
  DxvkCsChunkPool::DxvkCsChunkPool()
  : m_shardMask(heliosCsPoolSharding() ? ShardCount - 1u : 0u) { }
  
  
  DxvkCsChunkPool::~DxvkCsChunkPool() {
    for (auto& shard : m_shards) {
      for (DxvkCsChunk* chunk : shard.chunks)
        delete chunk;
    }
  }
  
  
  uint32_t DxvkCsChunkPool::pickShard() const {
    // Keep HELIOS_DXVK_CS_POOL_SHARDING=0 a true single-pool baseline:
    // do not materialize a thread ID or run the mixer when every operation
    // necessarily selects shard zero.
    if (!m_shardMask)
      return 0u;

    // Mix the producer thread ID before masking: Windows thread IDs often
    // have aligned low bits, which would otherwise leave most shards idle.
    uint32_t threadId = this_thread::get_id();
    threadId *= 0x9e3779b9u;
    threadId ^= threadId >> 16u;
    return threadId & m_shardMask;
  }


  DxvkCsChunkPool::Allocation DxvkCsChunkPool::allocChunk(DxvkCsChunkFlags flags) {
    uint32_t shardIndex = pickShard();
    auto& shard = m_shards[shardIndex];
    DxvkCsChunk* chunk = nullptr;

    { std::lock_guard<dxvk::mutex> lock(shard.mutex);
      
      if (shard.chunks.size() != 0) {
        chunk = shard.chunks.back();
        shard.chunks.pop_back();
      }
    }
    
    if (!chunk)
      chunk = new DxvkCsChunk();
    
    chunk->init(flags);
    return { chunk, shardIndex };
  }
  
  
  void DxvkCsChunkPool::freeChunk(
          DxvkCsChunk* chunk,
          uint32_t     shardIndex) {
    chunk->reset();

    auto& shard = m_shards[shardIndex];
    std::lock_guard<dxvk::mutex> lock(shard.mutex);
    shard.chunks.push_back(chunk);
  }
  
  
  DxvkCsThread::DxvkCsThread(
    const Rc<DxvkDevice>&   device,
    const Rc<DxvkContext>&  context)
  : m_device(device), m_context(context),
    m_thread([this] { threadFunc(); }) {
    
  }
  
  
  DxvkCsThread::~DxvkCsThread() {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }
    
    m_condOnAdd.notify_one();
    m_thread.join();
  }
  
  
  uint64_t DxvkCsThread::dispatchChunk(DxvkCsChunkRef&& chunk) {
    uint64_t seq;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      seq = ++m_queueOrdered.seqDispatch;

      auto& entry = m_queueOrdered.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = seq;

      m_condOnAdd.notify_one();
    }
    
    return seq;
  }


  void DxvkCsThread::injectChunk(DxvkCsQueue queue, DxvkCsChunkRef&& chunk, bool synchronize) {
    uint64_t timeline = 0u;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      auto& q = getQueue(queue);

      if (synchronize)
        timeline = ++q.seqDispatch;

      auto& entry = q.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = timeline;

      m_condOnAdd.notify_one();

      if (queue == DxvkCsQueue::HighPriority) {
        // Worker will check this flag after executing any
        // chunk without causing additional lock contention
        m_hasHighPrio.store(true);
      }
    }

    if (synchronize) {
      std::unique_lock<dxvk::mutex> lock(m_counterMutex);

      m_condOnSync.wait(lock, [this, queue, timeline] {
        return getCounter(queue).load() >= timeline;
      });
    }
  }


  void DxvkCsThread::synchronize(uint64_t seq) {
    // Avoid locking if we know the sync is a no-op, may
    // reduce overhead if this is being called frequently
    if (seq > m_seqOrdered.load()) {
      // We don't need to lock the queue here, if synchronization
      // happens while another thread is submitting then there is
      // an inherent race anyway
      if (seq == SynchronizeAll)
        seq = m_queueOrdered.seqDispatch;

      auto t0 = dxvk::high_resolution_clock::now();

      { std::unique_lock<dxvk::mutex> lock(m_counterMutex);
        m_condOnSync.wait(lock, [this, seq] {
          return m_seqOrdered.load() >= seq;
        });
      }

      auto t1 = dxvk::high_resolution_clock::now();
      auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

      m_device->addStatCtr(DxvkStatCounter::CsSyncCount, 1);
      m_device->addStatCtr(DxvkStatCounter::CsSyncTicks, ticks.count());
    }
  }
  
  
  void DxvkCsThread::threadFunc() {
    env::setThreadName("dxvk-cs");

    // Local chunk queues, we use two queues and swap between
    // them in order to potentially reduce lock contention.
    std::vector<DxvkCsQueuedChunk> ordered;
    std::vector<DxvkCsQueuedChunk> highPrio;

    try {
      while (!m_stopped.load()) {
        { std::unique_lock<dxvk::mutex> lock(m_mutex);

          auto pred = [this] { return
              !m_queueOrdered.queue.empty()
              || !m_queueHighPrio.queue.empty()
              || m_stopped.load();
          };

          if (unlikely(!pred())) {
            auto t0 = dxvk::high_resolution_clock::now();

            m_condOnAdd.wait(lock, [&] {
              return pred();
            });

            auto t1 = dxvk::high_resolution_clock::now();
            m_device->addStatCtr(DxvkStatCounter::CsIdleTicks, std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
          }

          std::swap(ordered, m_queueOrdered.queue);
          std::swap(highPrio, m_queueHighPrio.queue);

          m_hasHighPrio.store(false);
        }

        size_t orderedIndex = 0u;
        size_t highPrioIndex = 0u;

        while (highPrioIndex < highPrio.size() || orderedIndex < ordered.size()) {
          // Re-fill local high-priority queue if the app has queued anything up
          // in the meantime, we want to reduce possible synchronization delays.
          if (highPrioIndex >= highPrio.size() && m_hasHighPrio.load()) {
            highPrio.clear();
            highPrioIndex = 0u;

            std::unique_lock<dxvk::mutex> lock(m_mutex);
            std::swap(highPrio, m_queueHighPrio.queue);

            m_hasHighPrio.store(false);
          }

          // Drain high-priority queue first
          bool isHighPrio = highPrioIndex < highPrio.size();
          auto& entry = isHighPrio ? highPrio[highPrioIndex++] : ordered[orderedIndex++];

          m_context->addStatCtr(DxvkStatCounter::CsChunkCount, 1);

          entry.chunk->executeAll(m_context.ptr());

          if (entry.seq) {
            // Use a separate mutex for the chunk counter, this will only
            // ever be contested if synchronization is actually necessary.
            std::lock_guard lock(m_counterMutex);

            auto& counter = isHighPrio ? m_seqHighPrio : m_seqOrdered;
            counter.store(entry.seq);

            m_condOnSync.notify_one();
          }

          // Immediately free the chunk to release
          // references to any resources held by it
          entry.chunk = DxvkCsChunkRef();
        }

        ordered.clear();
        highPrio.clear();
      }
    } catch (const DxvkError& e) {
      Logger::err("Exception on CS thread!");
      Logger::err(e.message());
    }
  }
  
}
