#ifndef RL_TRACE_CORE_H
#define RL_TRACE_CORE_H

/*
 * Headroom-diagnostic trace (docs/rl/HEADROOM.md): the record format and the simulator-independent parts of the
 * recorder. Observation only: the simulator never reads a record back, and nothing here affects a replacement
 * decision, the coherence protocol or timing. The Sniper glue is cache/rl_trace.{h,cc}; the offline reader and the
 * shadow model are experiments/rl/headroom/.
 *
 * File: a stream of fixed 16-byte little-endian records, in the order the simulator produced them (one global
 * order, appended under one lock). The record index is the time axis of every offline analysis.
 *
 *   type      core  a                   b                      aux                         tag
 *   header    0     cores               ways                   schema version              kMagic
 *   header    1     log2(line bytes)    0                      sets of the traced cache    modulus << 32 | residue
 *   access    c     access flags        way if hit, else 0xff  set                         line   (flags: kAcc*)
 *   way       c     way                 state | valid << 2     hint | l1_mask << 8 | set << 16   line (or ~0)
 *   decision  c     victim way          eligible-way mask      ranks (3 bits/way) | fill << 24   victim line
 *   fill      c     way                 fill | evicted << 1    set                         line
 *   inval     c     way                 reason                 set                         line
 *   l1        c     0 = L1-I, 1 = L1-D  l1 flags               L2 set of the line          line
 *   roi       0     1 = begin, 0 = end  0                      0                           0
 *   end       0     0                   0                      0                           records before it
 *
 * line = address >> log2(line bytes) = Sniper's tag with address_hash = mask (set = line mod sets).
 * state: 0 I, 1 S, 2 E, 3 M (Sniper SHARED_UPGRADING is recorded as S and is not eligible; OWNED as M).
 * ranks: Sniper's getRecencyBits(way), 0 = most recently used. hint: the delivered sharer hint (0 = none).
 * l1_mask: the L2 block's record of which previous-level caches hold the line (Sniper m_cached_locs).
 * fill: 0 demand, 1 upgrade refill (CacheSetEvent::fill_t). reason: CacheSetEvent::invalidation_t
 * (0 unclassified, 1 coherence, 2 back invalidation, 3 upgrade).
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rltrace
{

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::uint64_t kMagic = 0x3145434152544c52ULL; // "RLTRACE1" as little-endian bytes
inline constexpr std::uint32_t kMaxWays = 8;                    // ranks are packed 3 bits per way
inline constexpr std::uint32_t kMaxSets = 65536;                // set fits in 16 bits of a way record
inline constexpr std::uint8_t kNoWay = 0xff;

enum class Type : std::uint8_t { header = 0, access = 1, way = 2, decision = 3, fill = 4, inval = 5, l1 = 6, roi = 7, end = 8 };
enum class State : std::uint8_t { I = 0, S = 1, E = 2, M = 3 };

// access flags (field a)
inline constexpr std::uint8_t kAccHit = 1u << 0;      // counted hit (Sniper cache_hit at the counter update)
inline constexpr std::uint8_t kAccStore = 1u << 1;    // Core::WRITE (else a read)
inline constexpr unsigned kAccPreStateShift = 2;      // 2 bits: the line's state before the access (I if absent)
inline constexpr std::uint8_t kAccFromL1I = 1u << 4;  // requested by the L1-I
inline constexpr std::uint8_t kAccPrefetch = 1u << 5; // a prefetch request (not counted in the demand statistics)
inline constexpr std::uint8_t kAccReadEx = 1u << 6;   // Core::READ_EX: a read that takes exclusive ownership
inline constexpr std::uint8_t kAccOverlap = 1u << 7;  // the line's earlier fill was still in flight (MSHR): Sniper's
                                                      // statistics count such a store hit as a store miss

// l1 flags (field b)
inline constexpr std::uint8_t kL1Store = 1u << 0;       // a data store or an exclusive read (takes ownership)
inline constexpr std::uint8_t kL1EpochFirst = 1u << 1;  // first L1 access of this core to the line since the last
                                                        // access or fill of this core in the line's L2 set
inline constexpr std::uint8_t kL1StoreFilter = 1u << 2; // a store recorded by the coherence filter (see L1Filter)

struct Record
{
  std::uint8_t type;
  std::uint8_t core;
  std::uint8_t a;
  std::uint8_t b;
  std::uint32_t aux;
  std::uint64_t tag;
};
static_assert(sizeof(Record) == 16, "trace records are 16 bytes");

inline Record make(Type t, std::uint32_t core, std::uint32_t a, std::uint32_t b, std::uint32_t aux, std::uint64_t tag)
{
  return Record{static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(core), static_cast<std::uint8_t>(a),
                static_cast<std::uint8_t>(b), aux, tag};
}

// One way of a set as the trace sees it
struct WayState
{
  std::uint64_t tag = ~0ULL;
  bool valid = false;
  State state = State::I;
  std::uint8_t hint = 0;
  std::uint8_t l1_mask = 0;
  std::uint8_t rank = 0;    // recency, 0 = MRU (not part of the change test: it changes at every access)
  bool eligible = true;     // may be chosen as a victim (not SHARED_UPGRADING)

  bool same_content(const WayState &o) const
  {
    return tag == o.tag && valid == o.valid && state == o.state && hint == o.hint && l1_mask == o.l1_mask;
  }
};

inline Record way_record(std::uint32_t core, std::uint32_t set, std::uint32_t way, const WayState &w)
{
  return make(Type::way, core, way, static_cast<std::uint32_t>(w.state) | (w.valid ? 4u : 0u),
              static_cast<std::uint32_t>(w.hint) | (static_cast<std::uint32_t>(w.l1_mask) << 8) | (set << 16), w.tag);
}

inline Record access_record(std::uint32_t core, std::uint32_t set, std::uint64_t line, bool store, bool read_ex, bool hit, State pre,
                            bool from_l1i, bool prefetch, std::uint32_t way, bool overlap = false)
{
  const std::uint32_t flags = (hit ? kAccHit : 0u) | (store ? kAccStore : 0u) | (static_cast<std::uint32_t>(pre) << kAccPreStateShift) |
                              (from_l1i ? kAccFromL1I : 0u) | (prefetch ? kAccPrefetch : 0u) | (read_ex ? kAccReadEx : 0u) |
                              (overlap ? kAccOverlap : 0u);
  return make(Type::access, core, flags, hit ? way : kNoWay, set, line);
}

// ranks: 3 bits per way; eligible: bit per way
inline Record decision_record(std::uint32_t core, std::uint32_t victim_way, std::uint32_t eligible_mask, std::uint32_t ranks,
                              std::uint32_t fill, std::uint64_t victim_line)
{
  return make(Type::decision, core, victim_way, eligible_mask, ranks | (fill << 24), victim_line);
}

inline Record fill_record(std::uint32_t core, std::uint32_t set, std::uint32_t way, std::uint64_t line, std::uint32_t fill, bool evicted)
{
  return make(Type::fill, core, way, fill | (evicted ? 2u : 0u), set, line);
}

inline Record inval_record(std::uint32_t core, std::uint32_t set, std::uint32_t way, std::uint64_t line, std::uint32_t reason)
{
  return make(Type::inval, core, way, reason, set, line);
}

inline Record l1_record(std::uint32_t core, bool l1d, std::uint8_t flags, std::uint32_t set, std::uint64_t line)
{
  return make(Type::l1, core, l1d ? 1u : 0u, flags, set, line);
}

// Pack the decision-time recency and eligibility of `ways` ways
inline void pack_ways(const WayState *w, std::uint32_t ways, std::uint32_t &ranks, std::uint32_t &eligible)
{
  ranks = 0;
  eligible = 0;
  for (std::uint32_t i = 0; i < ways; ++i)
  {
    ranks |= (static_cast<std::uint32_t>(w[i].rank) & 7u) << (3 * i);
    if (w[i].eligible)
      eligible |= 1u << i;
  }
}

// Set sampling: set s is traced iff s mod modulus == residue (deterministic, the same sets in every cache)
struct Sampling
{
  std::uint32_t modulus = 8;
  std::uint32_t residue = 0;
  bool sampled(std::uint32_t set) const { return set % modulus == residue; }
};

/*
 * The last way contents written for each (core, set, way): a way record is written only when a way's content (tag,
 * valid, state, hint, L1 mask) differs from what was last written, immediately before the record that needs it. The
 * offline mirror therefore knows every traced way's content at every access, decision and invalidation of its set.
 */
class WayMirror
{
public:
  WayMirror(std::uint32_t cores, std::uint32_t sets, std::uint32_t ways) : sets_(sets), ways_(ways), v_(std::size_t(cores) * sets * ways) {}

  template <typename Emit>
  std::uint32_t sync(std::uint32_t core, std::uint32_t set, const WayState *now, Emit emit)
  {
    std::uint32_t written = 0;
    WayState *last = &v_[(std::size_t(core) * sets_ + set) * ways_];
    for (std::uint32_t w = 0; w < ways_; ++w)
      if (!last[w].same_content(now[w]))
      {
        last[w] = now[w];
        emit(way_record(core, set, w, now[w]));
        ++written;
      }
    return written;
  }

private:
  std::uint32_t sets_, ways_;
  std::vector<WayState> v_; // initially invalid ways, as a cache starts
};

/*
 * Which L1 accesses to record (the L1 sees far more accesses than the L2; most are not needed offline).
 *
 * Epoch rule (L1 inclusion): an epoch of (core c, L2 set s) ends at every recorded access and fill of c in s. The
 * first L1 access of c to a line of s in each epoch is recorded (kL1EpochFirst). A shadow cache decides only at an
 * access or a fill, so the first L1 use of any line it evicts is always in the trace. The access or fill of a line
 * itself counts as its first use in the new epoch (the L1 access that completes that miss is not recorded again).
 *
 * Store rule (coherence): a store by core j to line x is recorded unless the previous recorded store to x was also by
 * j and no access or L1 record of x (by any core) came after it (kL1StoreFilter). A shadow copy of x can only appear
 * at an access or an L1 record, so the first store by any other core after it is always in the trace; the stores left
 * out repeat a recorded store with nothing in between that they could affect.
 *
 * Memory: one entry per (core, line) and per line ever recorded in the sampled sets (bounded by the sampled
 * footprint, not by the run length).
 */
class L1Filter
{
public:
  L1Filter(std::uint32_t cores, std::uint32_t sets) : sets_(sets), epochs_(std::size_t(cores) * sets, 0) {}

  // An access or a fill of `core` to `line` in `set`, recorded as record number `index`. Accesses also count as an
  // event of the line for the store rule (fills always follow an access of that core, or an upgrade).
  void on_l2(std::uint32_t core, std::uint32_t set, std::uint64_t line, std::uint64_t index, bool is_access)
  {
    const std::uint32_t e = ++epochs_[std::size_t(core) * sets_ + set];
    last_epoch_[key(core, line)] = e;
    if (is_access)
      lines_[line].last_event = index + 1; // stored +1 so that 0 means "never"
  }

  // An L1 access; returns the l1 flags to record, or 0 when the access is not recorded. `index` is the record number
  // the l1 record would get.
  std::uint8_t on_l1(std::uint32_t core, std::uint32_t set, std::uint64_t line, bool store, std::uint64_t index)
  {
    std::uint8_t flags = 0;
    const std::uint32_t e = epochs_[std::size_t(core) * sets_ + set];
    auto it = last_epoch_.find(key(core, line));
    if (it == last_epoch_.end() || it->second != e)
    {
      flags |= kL1EpochFirst;
      last_epoch_[key(core, line)] = e;
    }
    if (store)
    {
      const Line &l = lines_[line];
      if (l.last_store == 0 || l.last_store_core != core || l.last_event > l.last_store)
        flags |= kL1StoreFilter;
    }
    if (!flags)
      return 0;
    if (store)
      flags |= kL1Store;
    Line &l = lines_[line];
    l.last_event = index + 1;
    if (store)
    {
      l.last_store = index + 1;
      l.last_store_core = core;
    }
    return flags;
  }

  std::size_t entries() const { return last_epoch_.size() + lines_.size(); }

private:
  struct Line
  {
    std::uint64_t last_event = 0; // record number + 1 of the last access or l1 record of the line
    std::uint64_t last_store = 0; // record number + 1 of the last recorded store
    std::uint32_t last_store_core = 0;
  };
  static std::uint64_t key(std::uint32_t core, std::uint64_t line) { return (line << 8) | (core & 0xffu); }

  std::uint32_t sets_;
  std::vector<std::uint32_t> epochs_;
  std::unordered_map<std::uint64_t, std::uint32_t> last_epoch_;
  std::unordered_map<std::uint64_t, Line> lines_;
};

// Buffered writer: records are appended to a buffer and handed to `sink(data, bytes)` in blocks
template <typename Sink>
class Writer
{
public:
  explicit Writer(Sink sink, std::size_t block_records = 65536) : sink_(sink), block_(block_records) { buf_.reserve(block_); }

  std::uint64_t append(const Record &r)
  {
    buf_.push_back(r);
    ++counts_[r.type < kTypes ? r.type : kTypes - 1];
    if (buf_.size() >= block_)
      flush();
    return records_++;
  }
  void flush()
  {
    if (!buf_.empty())
      sink_(reinterpret_cast<const void *>(buf_.data()), buf_.size() * sizeof(Record));
    buf_.clear();
  }
  std::uint64_t records() const { return records_; }       // records appended so far = index of the next one
  std::uint64_t count(Type t) const { return counts_[static_cast<std::size_t>(t)]; }

  static constexpr std::size_t kTypes = 9;

private:
  Sink sink_;
  std::size_t block_;
  std::vector<Record> buf_;
  std::uint64_t records_ = 0;
  std::uint64_t counts_[kTypes] = {};
};

} // namespace rltrace

#endif /* RL_TRACE_CORE_H */
