#include "rl_trace.h"
#include "cache_set.h"
#include "config.h"
#include "config.hpp"
#include "hooks_manager.h"
#include "lock.h"
#include "log.h"
#include "rl_trace_core.h"
#include "shared_cache_block_info.h"
#include "simulator.h"

#include <cstdio>
#include <memory>

// Headroom-diagnostic trace (rl_trace.h, rl/rl_trace_core.h, docs/rl/HEADROOM.md). Observation only.

namespace RlTrace
{
std::atomic<bool> g_enabled{false};

namespace
{
struct Sink
{
   FILE *file;
   void operator()(const void *data, std::size_t bytes) const
   {
      if (fwrite(data, 1, bytes, file) != bytes)
         LOG_PRINT_ERROR("RL trace: write failed");
   }
};

struct Tracer
{
   Lock lock; // leaf lock: nothing is called while holding it
   String cfgname, name, policy, bin_path, json_path;
   UInt32 cores = 0, sets = 0, ways = 0, blocksize = 0, log_block = 0;
   rltrace::Sampling sampling;
   FILE *file = nullptr;
   std::unique_ptr<rltrace::Writer<Sink>> writer;
   std::unique_ptr<rltrace::WayMirror> mirror;
   std::unique_ptr<rltrace::L1Filter> filter;
   UInt32 caches = 0;
   UInt64 l1_seen = 0;           // L1 accesses to lines of sampled sets
   UInt64 l1_epoch_first = 0;    // ... recorded by the epoch rule
   UInt64 l1_store_filter = 0;   // ... recorded by the store rule
   UInt64 roi_markers[2] = {};   // end, begin
   UInt64 decisions_without_full_view = 0; // eviction although the pre-insert view had an invalid way (expect 0)
   UInt64 full_view_without_eviction = 0;  // the reverse (expect 0)
   bool closed = false;
};

Tracer *g_tracer = nullptr; // created by the first traced cache (single-threaded construction), closed at HOOK_SIM_END

rltrace::State toState(CacheState::cstate_t c)
{
   switch (c)
   {
      case CacheState::MODIFIED:
      case CacheState::OWNED:
         return rltrace::State::M;
      case CacheState::EXCLUSIVE:
         return rltrace::State::E;
      case CacheState::SHARED:
      case CacheState::SHARED_UPGRADING:
         return rltrace::State::S;
      default:
         return rltrace::State::I;
   }
}

// The trace's view of every way of a set (reads simulator state only)
void snapshot(CacheSet &s, UInt32 ways, rltrace::WayState *w)
{
   for (UInt32 i = 0; i < ways; ++i)
   {
      CacheBlockInfo *b = s.peekBlock(i);
      rltrace::WayState &x = w[i];
      x = rltrace::WayState();
      x.rank = s.getRecencyBits(i);
      x.eligible = s.isValidReplacement(i);
      if (!b->isValid())
         continue; // normalised: tag ~0, state I, no hint, no L1 copy
      x.valid = true;
      x.tag = b->getTag();
      x.state = toState(b->getCState());
      x.hint = b->getSharerHint();
      if (SharedCacheBlockInfo *sb = dynamic_cast<SharedCacheBlockInfo *>(b))
         x.l1_mask = (UInt8)(sb->getCachedLocs().to_ulong() & 0xff);
   }
}

void append(Tracer &t, const rltrace::Record &r)
{
   t.writer->append(r);
}

void sync(Tracer &t, core_id_t core, UInt32 set, const rltrace::WayState *now)
{
   t.mirror->sync((UInt32)core, set, now, [&t](const rltrace::Record &r) { append(t, r); });
}

void writeJson(Tracer &t)
{
   FILE *f = fopen(t.json_path.c_str(), "w");
   if (!f)
   {
      fprintf(stderr, "[RL-TRACE] cannot write %s\n", t.json_path.c_str());
      return;
   }
   const rltrace::Writer<Sink> &w = *t.writer;
   UInt32 sampled = 0;
   for (UInt32 s = 0; s < t.sets; ++s)
      sampled += t.sampling.sampled(s);
   fprintf(f, "{\n");
   fprintf(f, "  \"schema\": \"rl-trace\",\n  \"schema_version\": %u,\n  \"record_bytes\": %zu,\n", rltrace::kSchemaVersion, sizeof(rltrace::Record));
   fprintf(f, "  \"trace_file\": \"%s\",\n", t.bin_path.c_str());
   fprintf(f, "  \"cache\": {\"cfgname\": \"%s\", \"name\": \"%s\", \"replacement_policy\": \"%s\", \"caches\": %u,\n", t.cfgname.c_str(), t.name.c_str(),
           t.policy.c_str(), t.caches);
   fprintf(f, "            \"sets\": %u, \"ways\": %u, \"line_bytes\": %u, \"address_hash\": \"mask\"},\n", t.sets, t.ways, t.blocksize);
   fprintf(f, "  \"cores\": %u,\n", t.cores);
   fprintf(f, "  \"sampling\": {\"set_modulus\": %u, \"set_residue\": %u, \"sampled_sets_per_cache\": %u},\n", t.sampling.modulus, t.sampling.residue,
           sampled);
   fprintf(f, "  \"records\": {\"total\": %llu, \"header\": %llu, \"access\": %llu, \"way\": %llu, \"decision\": %llu, \"fill\": %llu, "
              "\"inval\": %llu, \"l1\": %llu, \"roi\": %llu, \"end\": %llu},\n",
           (unsigned long long)w.records(), (unsigned long long)w.count(rltrace::Type::header), (unsigned long long)w.count(rltrace::Type::access),
           (unsigned long long)w.count(rltrace::Type::way), (unsigned long long)w.count(rltrace::Type::decision),
           (unsigned long long)w.count(rltrace::Type::fill), (unsigned long long)w.count(rltrace::Type::inval),
           (unsigned long long)w.count(rltrace::Type::l1), (unsigned long long)w.count(rltrace::Type::roi),
           (unsigned long long)w.count(rltrace::Type::end));
   fprintf(f, "  \"l1_filter\": {\"l1_accesses_in_sampled_sets\": %llu, \"recorded_epoch_first\": %llu, \"recorded_store_filter\": %llu, "
              "\"filter_entries\": %zu},\n",
           (unsigned long long)t.l1_seen, (unsigned long long)t.l1_epoch_first, (unsigned long long)t.l1_store_filter, t.filter->entries());
   fprintf(f, "  \"roi_markers\": {\"begin\": %llu, \"end\": %llu},\n", (unsigned long long)t.roi_markers[1], (unsigned long long)t.roi_markers[0]);
   fprintf(f, "  \"checks\": {\"decisions_without_full_view\": %llu, \"full_view_without_eviction\": %llu}\n",
           (unsigned long long)t.decisions_without_full_view, (unsigned long long)t.full_view_without_eviction);
   fprintf(f, "}\n");
   fclose(f);
}

void close(Tracer &t)
{
   ScopedLock sl(t.lock);
   if (t.closed)
      return;
   const UInt64 before = t.writer->records();
   append(t, rltrace::make(rltrace::Type::end, 0, 0, 0, 0, before));
   t.writer->flush();
   fclose(t.file);
   t.file = nullptr;
   t.closed = true;
   g_enabled.store(false);
   writeJson(t);
   printf("[RL-TRACE] %s: %llu records (%llu bytes), %s\n", t.bin_path.c_str(), (unsigned long long)t.writer->records(),
          (unsigned long long)(t.writer->records() * sizeof(rltrace::Record)), t.json_path.c_str());
}

SInt64 hookRoi(UInt64 begin, UInt64)
{
   Tracer &t = *g_tracer;
   ScopedLock sl(t.lock);
   if (t.closed)
      return 0;
   append(t, rltrace::make(rltrace::Type::roi, 0, begin ? 1 : 0, 0, 0, 0));
   ++t.roi_markers[begin ? 1 : 0];
   return 0;
}
SInt64 hookRoiBegin(UInt64, UInt64) { return hookRoi(1, 0); }
SInt64 hookRoiEnd(UInt64, UInt64) { return hookRoi(0, 0); }
SInt64 hookSimEnd(UInt64, UInt64)
{
   close(*g_tracer);
   return 0;
}

UInt32 log2u(UInt32 v)
{
   UInt32 l = 0;
   while ((1u << l) < v)
      ++l;
   return l;
}
} // namespace

bool configure(String cfgname, String name, core_id_t core_id, UInt32 num_sets, UInt32 associativity, UInt32 blocksize,
               CacheBase::hash_t hash, String replacement_policy)
{
   if (!Sim()->getCfg()->getBoolDefault(cfgname + "/rl_trace/enabled", false))
      return false;
   LOG_ASSERT_ERROR(hash == CacheBase::HASH_MASK, "%s/rl_trace needs address_hash = mask (set = line mod sets)", cfgname.c_str());
   LOG_ASSERT_ERROR(associativity <= rltrace::kMaxWays, "%s/rl_trace supports at most %u ways (%u)", cfgname.c_str(), rltrace::kMaxWays, associativity);
   LOG_ASSERT_ERROR(num_sets <= rltrace::kMaxSets && (num_sets & (num_sets - 1)) == 0, "%s/rl_trace needs a power-of-two number of sets <= %u (%u)",
                    cfgname.c_str(), rltrace::kMaxSets, num_sets);
   LOG_ASSERT_ERROR(blocksize > 0 && (blocksize & (blocksize - 1)) == 0, "%s/rl_trace: line size %u is not a power of two", cfgname.c_str(), blocksize);
   LOG_ASSERT_ERROR(core_id >= 0 && core_id < 255, "%s/rl_trace: core id %d out of range", cfgname.c_str(), core_id);

   if (!g_tracer)
   {
      Tracer *t = new Tracer();
      t->cfgname = cfgname;
      t->name = name;
      t->policy = replacement_policy;
      t->sets = num_sets;
      t->ways = associativity;
      t->blocksize = blocksize;
      t->log_block = log2u(blocksize);
      t->cores = Config::getSingleton()->getTotalCores();
      LOG_ASSERT_ERROR(t->cores <= 255, "rl_trace: at most 255 cores (%u)", t->cores);
      const SInt64 modulus = Sim()->getCfg()->getIntDefault(cfgname + "/rl_trace/set_modulus", 8);
      const SInt64 residue = Sim()->getCfg()->getIntDefault(cfgname + "/rl_trace/set_residue", 0);
      LOG_ASSERT_ERROR(modulus >= 1 && modulus <= (SInt64)num_sets, "%s/rl_trace/set_modulus must be in [1, %u] (%ld)", cfgname.c_str(), num_sets, (long)modulus);
      LOG_ASSERT_ERROR(residue >= 0 && residue < modulus, "%s/rl_trace/set_residue must be in [0, set_modulus) (%ld)", cfgname.c_str(), (long)residue);
      t->sampling.modulus = (UInt32)modulus;
      t->sampling.residue = (UInt32)residue;
      t->bin_path = Sim()->getConfig()->formatOutputFileName("rl-trace.bin");
      t->json_path = Sim()->getConfig()->formatOutputFileName("rl-trace.json");
      t->file = fopen(t->bin_path.c_str(), "wb");
      LOG_ASSERT_ERROR(t->file != NULL, "Cannot open %s", t->bin_path.c_str());
      t->writer.reset(new rltrace::Writer<Sink>(Sink{t->file}));
      t->mirror.reset(new rltrace::WayMirror(t->cores, num_sets, associativity));
      t->filter.reset(new rltrace::L1Filter(t->cores, num_sets));
      append(*t, rltrace::make(rltrace::Type::header, 0, t->cores, associativity, rltrace::kSchemaVersion, rltrace::kMagic));
      append(*t, rltrace::make(rltrace::Type::header, 1, t->log_block, 0, num_sets, ((UInt64)t->sampling.modulus << 32) | t->sampling.residue));
      g_tracer = t;
      Sim()->getHooksManager()->registerHook(HookType::HOOK_ROI_BEGIN, hookRoiBegin, 0);
      Sim()->getHooksManager()->registerHook(HookType::HOOK_ROI_END, hookRoiEnd, 0);
      Sim()->getHooksManager()->registerHook(HookType::HOOK_SIM_END, hookSimEnd, 0);
      g_enabled.store(true);
      printf("[RL-TRACE] %s (%s, %s): sets %% %u == %u traced, %u ways, %u B lines, %u cores -> %s (observation only)\n", name.c_str(),
             cfgname.c_str(), replacement_policy.c_str(), t->sampling.modulus, t->sampling.residue, associativity, blocksize, t->cores,
             t->bin_path.c_str());
   }
   Tracer &t = *g_tracer;
   LOG_ASSERT_ERROR(t.cfgname == cfgname, "rl_trace: only one cache configuration can be traced (%s and %s)", t.cfgname.c_str(), cfgname.c_str());
   LOG_ASSERT_ERROR(t.sets == num_sets && t.ways == associativity && t.blocksize == blocksize, "rl_trace: traced caches of %s differ in geometry",
                    cfgname.c_str());
   LOG_ASSERT_ERROR((UInt32)core_id < t.cores, "rl_trace: core id %d >= %u cores", core_id, t.cores);
   ++t.caches;
   return true;
}

bool sampled(UInt32 set)
{
   return g_tracer && g_tracer->sampling.sampled(set);
}

void access(core_id_t core, UInt32 set, CacheSet &set_obj, IntPtr tag, bool store, bool read_ex, bool hit, CacheState::cstate_t pre, bool from_l1i,
            bool prefetch, bool overlap)
{
   Tracer &t = *g_tracer;
   // The L1 side computes a line's set as line mod sets (address_hash = mask, no home-lookup remapping): check it
   LOG_ASSERT_ERROR(set == (UInt32)(tag & (t.sets - 1)), "rl_trace: line %lx is in set %u, not line mod %u sets", tag, set, t.sets);
   rltrace::WayState now[rltrace::kMaxWays];
   snapshot(set_obj, t.ways, now);
   UInt32 way = rltrace::kNoWay, index = 0;
   if (hit && set_obj.find(tag, &index))
      way = index;
   ScopedLock sl(t.lock);
   if (t.closed)
      return;
   sync(t, core, set, now);
   const UInt64 at = t.writer->records();
   append(t, rltrace::access_record((UInt32)core, set, tag, store, read_ex, hit, toState(pre), from_l1i, prefetch, way, overlap));
   t.filter->on_l2((UInt32)core, set, tag, at, true);
}

InsertView beforeInsert(core_id_t core, UInt32 set, CacheSet &set_obj)
{
   Tracer &t = *g_tracer;
   rltrace::WayState now[rltrace::kMaxWays];
   snapshot(set_obj, t.ways, now);
   InsertView v;
   v.full = true;
   for (UInt32 i = 0; i < t.ways; ++i)
      v.full &= now[i].valid;
   rltrace::pack_ways(now, t.ways, v.ranks, v.eligible);
   ScopedLock sl(t.lock);
   if (!t.closed)
      sync(t, core, set, now);
   return v;
}

void afterInsert(core_id_t core, UInt32 set, CacheSet &set_obj, const InsertView &view, IntPtr tag, CacheSetEvent::fill_t fill, bool eviction,
                 IntPtr victim_tag)
{
   Tracer &t = *g_tracer;
   UInt32 way = 0;
   const bool found = set_obj.find(tag, &way) != NULL;
   ScopedLock sl(t.lock);
   if (t.closed)
      return;
   LOG_ASSERT_ERROR(found, "rl_trace: inserted line %lx not found in its set", tag);
   if (eviction && !view.full)
      ++t.decisions_without_full_view;
   if (view.full && !eviction)
      ++t.full_view_without_eviction;
   if (eviction)
      append(t, rltrace::decision_record((UInt32)core, way, view.eligible, view.ranks, (UInt32)fill, victim_tag));
   const UInt64 at = t.writer->records();
   append(t, rltrace::fill_record((UInt32)core, set, way, tag, (UInt32)fill, eviction));
   t.filter->on_l2((UInt32)core, set, tag, at, false);
}

void beforeInvalidate(core_id_t core, UInt32 set, CacheSet &set_obj, IntPtr tag, CacheSetEvent::invalidation_t reason)
{
   Tracer &t = *g_tracer;
   UInt32 way = 0;
   if (set_obj.find(tag, &way) == NULL)
      return; // not present: invalidate() changes nothing
   rltrace::WayState now[rltrace::kMaxWays];
   snapshot(set_obj, t.ways, now);
   ScopedLock sl(t.lock);
   if (t.closed)
      return;
   sync(t, core, set, now);
   append(t, rltrace::inval_record((UInt32)core, set, way, tag, (UInt32)reason));
}

void noteL1Access(IntPtr address, core_id_t core, bool l1d, bool store)
{
   Tracer *t = g_tracer;
   if (!t)
      return;
   const UInt64 line = (UInt64)address >> t->log_block;
   const UInt32 set = (UInt32)(line & (t->sets - 1));
   if (!t->sampling.sampled(set))
      return;
   ScopedLock sl(t->lock);
   if (t->closed)
      return;
   ++t->l1_seen;
   const std::uint8_t flags = t->filter->on_l1((UInt32)core, set, line, store, t->writer->records());
   if (!flags)
      return;
   t->l1_epoch_first += (flags & rltrace::kL1EpochFirst) != 0;
   t->l1_store_filter += (flags & rltrace::kL1StoreFilter) != 0;
   append(*t, rltrace::l1_record((UInt32)core, l1d, flags, set, line));
}
} // namespace RlTrace
