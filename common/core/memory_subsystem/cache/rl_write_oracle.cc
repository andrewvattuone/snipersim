#include "rl_write_oracle.h"
#include "lock.h"
#include "log.h"

#include <unordered_map>

namespace RlWriteOracle
{
std::atomic<bool> g_enabled{false};

namespace
{
struct Record
{
   UInt64 last_seq = 0;                 // most recent write
   core_id_t last_writer = INVALID_CORE_ID;
   UInt64 other_seq = 0;                // most recent write by a core other than last_writer
};

constexpr UInt32 SHARDS = 64;

struct Shard
{
   Lock lock;
   std::unordered_map<IntPtr, Record> lines;
};

Shard g_shards[SHARDS];
std::atomic<UInt64> g_seq{0};
std::atomic<UInt32> g_log_line_size{0};

IntPtr lineOf(IntPtr address) { return address >> g_log_line_size.load(std::memory_order_relaxed); }
Shard &shardOf(IntPtr line) { return g_shards[(line ^ (line >> 7)) % SHARDS]; }

bool lookup(IntPtr address, Record &out)
{
   const IntPtr line = lineOf(address);
   Shard &s = shardOf(line);
   ScopedLock sl(s.lock);
   auto it = s.lines.find(line);
   if (it == s.lines.end())
      return false;
   out = it->second;
   return true;
}
} // namespace

void enable(UInt32 line_size)
{
   LOG_ASSERT_ERROR(line_size > 0 && (line_size & (line_size - 1)) == 0, "RL write oracle: line size %u is not a power of two", line_size);
   UInt32 log = 0;
   while ((1u << log) < line_size)
      ++log;
   UInt32 expected = 0;
   if (!g_log_line_size.compare_exchange_strong(expected, log))
      LOG_ASSERT_ERROR(expected == log, "RL write oracle: caches with different line sizes (%u and %u bytes)", 1u << expected, line_size);
   g_enabled.store(true);
}

void noteWrite(IntPtr address, core_id_t core)
{
   const IntPtr line = lineOf(address);
   Shard &s = shardOf(line);
   ScopedLock sl(s.lock);
   const UInt64 seq = ++g_seq;
   Record &r = s.lines[line];
   if (r.last_writer != core)
   {
      r.other_seq = r.last_seq;
      r.last_writer = core;
   }
   r.last_seq = seq;
}

UInt64 now()
{
   return g_seq.load();
}

bool writtenByOtherSince(IntPtr address, core_id_t core, UInt64 since)
{
   Record r;
   if (!lookup(address, r))
      return false;
   return (r.last_writer != core ? r.last_seq : r.other_seq) > since;
}

bool writtenBySince(IntPtr address, core_id_t core, UInt64 since)
{
   Record r;
   if (!lookup(address, r))
      return false;
   return r.last_writer == core && r.last_seq > since;
}
} // namespace RlWriteOracle
