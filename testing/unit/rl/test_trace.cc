// Headroom-diagnostic trace (rl_trace_core.h, docs/rl/HEADROOM.md): the record layout the offline reader depends on,
// the way-change records, the L1 epoch and store filters, set sampling and the buffered writer. The recorder is
// observation only; these tests fix what it writes.

#include <cstring>
#include <vector>

#include "rl_test.h"
#include "rl_trace_core.h"

using namespace rltrace;

namespace
{
std::vector<unsigned char> bytes_of(const Record &r)
{
  std::vector<unsigned char> b(sizeof r);
  std::memcpy(b.data(), &r, sizeof r);
  return b;
}

WayState way(std::uint64_t tag, State s, std::uint8_t hint = 0, std::uint8_t l1 = 0, std::uint8_t rank = 0, bool eligible = true)
{
  WayState w;
  w.tag = tag;
  w.valid = true;
  w.state = s;
  w.hint = hint;
  w.l1_mask = l1;
  w.rank = rank;
  w.eligible = eligible;
  return w;
}
} // namespace

TEST_CASE("trace: records are 16 little-endian bytes: type, core, a, b, aux, tag")
{
  REQUIRE(sizeof(Record) == 16);
  const Record r = make(Type::access, 3, 0x12, 0x34, 0x01020304u, 0x1122334455667788ULL);
  const std::vector<unsigned char> b = bytes_of(r);
  const unsigned char want[16] = {1, 3, 0x12, 0x34, 0x04, 0x03, 0x02, 0x01, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  for (int i = 0; i < 16; ++i)
    CHECK(b[i] == want[i]);
  CHECK(kMagic == 0x3145434152544c52ULL);
  const char magic[9] = "RLTRACE1";
  CHECK(std::memcmp(&kMagic, magic, 8) == 0);
}

TEST_CASE("trace: access record flags (hit, store, pre-state, L1-I, prefetch, exclusive read) and way")
{
  const Record hit = access_record(1, 77, 0xabc, true, false, true, State::M, false, false, 5);
  CHECK(hit.type == (std::uint8_t)Type::access);
  CHECK(hit.core == 1);
  CHECK(hit.a == (kAccHit | kAccStore | (3u << kAccPreStateShift)));
  CHECK(hit.b == 5);
  CHECK(hit.aux == 77);
  CHECK(hit.tag == 0xabc);
  // a miss never records a way; a write to S is an (upgrade) miss with pre-state S
  const Record up = access_record(0, 2, 9, true, false, false, State::S, false, false, 3);
  CHECK(up.a == (kAccStore | (1u << kAccPreStateShift)));
  CHECK(up.b == kNoWay);
  const Record ifetch = access_record(0, 2, 9, false, false, false, State::I, true, false, 0);
  CHECK(ifetch.a == kAccFromL1I);
  const Record rx = access_record(0, 2, 9, false, true, false, State::E, false, true, 0);
  CHECK(rx.a == (kAccReadEx | kAccPrefetch | (2u << kAccPreStateShift)));
  // a store hit on a line whose fill is still in flight: Sniper's statistics count it as a store miss
  const Record ov = access_record(0, 2, 9, true, false, true, State::M, false, false, 1, true);
  CHECK(ov.a == (kAccHit | kAccStore | kAccOverlap | (3u << kAccPreStateShift)));
  CHECK(ov.b == 1);
}

TEST_CASE("trace: way, decision, fill, invalidation and l1 record fields")
{
  const Record w = way_record(2, 300, 7, way(0x1234, State::E, 3, 0x2));
  CHECK(w.type == (std::uint8_t)Type::way);
  CHECK(w.core == 2);
  CHECK(w.a == 7);
  CHECK(w.b == ((std::uint32_t)State::E | 4u));
  CHECK(w.aux == (3u | (0x2u << 8) | (300u << 16)));
  CHECK(w.tag == 0x1234);
  const Record inv_way = way_record(0, 5, 1, WayState());
  CHECK(inv_way.b == 0); // not valid, state I
  CHECK(inv_way.tag == ~0ULL);

  const Record d = decision_record(1, 6, 0xbf, 0x00fac688u, 1, 0x99);
  CHECK(d.type == (std::uint8_t)Type::decision);
  CHECK(d.a == 6);
  CHECK(d.b == 0xbf);
  CHECK(d.aux == (0x00fac688u | (1u << 24)));
  CHECK(d.tag == 0x99);

  const Record f = fill_record(0, 12, 4, 0x55, 1, true);
  CHECK(f.a == 4);
  CHECK(f.b == (1u | 2u));
  CHECK(f.aux == 12);
  const Record i = inval_record(3, 12, 2, 0x55, 2);
  CHECK(i.type == (std::uint8_t)Type::inval);
  CHECK(i.b == 2);
  const Record l = l1_record(1, true, kL1Store | kL1StoreFilter, 40, 0x77);
  CHECK(l.a == 1);
  CHECK(l.b == (kL1Store | kL1StoreFilter));
  CHECK(l.aux == 40);
}

TEST_CASE("trace: decision ranks are 3 bits per way, eligibility one bit per way (SHARED_UPGRADING not eligible)")
{
  WayState w[8];
  for (std::uint32_t i = 0; i < 8; ++i)
    w[i] = way(100 + i, State::S, 0, 0, (std::uint8_t)(7 - i));
  w[6].eligible = false; // a way in SHARED_UPGRADING
  std::uint32_t ranks = 0, eligible = 0;
  pack_ways(w, 8, ranks, eligible);
  for (std::uint32_t i = 0; i < 8; ++i)
    CHECK(((ranks >> (3 * i)) & 7u) == 7 - i);
  CHECK(eligible == 0xbfu);
  CHECK(ranks < (1u << 24)); // leaves the fill kind in bits 24-31
}

TEST_CASE("trace: a way record is written only when the way's content changes, not its recency")
{
  WayMirror m(2, 4, 2);
  std::vector<Record> out;
  auto emit = [&](const Record &r) { out.push_back(r); };
  WayState now[2] = {WayState(), WayState()};
  CHECK(m.sync(1, 3, now, emit) == 0); // a cache starts with invalid ways
  now[0] = way(0x10, State::E, 1, 0, 0);
  CHECK(m.sync(1, 3, now, emit) == 1);
  REQUIRE(out.size() == 1);
  CHECK(out[0].a == 0);
  CHECK(out[0].aux >> 16 == 3);
  now[0].rank = 1; // recency alone: nothing
  CHECK(m.sync(1, 3, now, emit) == 0);
  now[0].state = State::S; // a downgrade
  now[1] = way(0x20, State::M, 1, 0x1, 0);
  CHECK(m.sync(1, 3, now, emit) == 2);
  now[1].l1_mask = 0; // the L1 dropped its copy
  CHECK(m.sync(1, 3, now, emit) == 1);
  CHECK(m.sync(0, 3, now, emit) == 2); // other core, same set: tracked separately
  now[0] = WayState(); // invalidated
  CHECK(m.sync(1, 3, now, emit) == 1);
  CHECK((out.back().b & 4u) == 0);
}

TEST_CASE("trace: epoch rule records the first L1 access to a line after each access or fill in its set")
{
  L1Filter f(2, 8);
  std::uint64_t idx = 0;
  // core 0 accesses line 0x13 (set 3); the L1 access that completes it is not recorded again
  f.on_l2(0, 3, 0x13, idx++, true);
  CHECK(f.on_l1(0, 3, 0x13, false, idx) == 0);
  // another line of set 3 already in the L1: its first use in this epoch is recorded, the second is not
  CHECK(f.on_l1(0, 3, 0x23, false, idx++) == kL1EpochFirst);
  CHECK(f.on_l1(0, 3, 0x23, false, idx) == 0);
  // a fill in set 3 starts a new epoch: recorded again
  f.on_l2(0, 3, 0x33, idx++, false);
  CHECK(f.on_l1(0, 3, 0x23, false, idx++) == kL1EpochFirst);
  // events of another set or another core do not end core 0's epoch in set 3
  f.on_l2(0, 4, 0x14, idx++, true);
  f.on_l2(1, 3, 0x43, idx++, true);
  CHECK(f.on_l1(0, 3, 0x23, false, idx) == 0);
  // the other core has its own epochs
  CHECK(f.on_l1(1, 3, 0x23, false, idx++) == kL1EpochFirst);
}

TEST_CASE("trace: store rule records a store unless the same core's previous recorded store had nothing after it")
{
  L1Filter f(3, 8);
  std::uint64_t idx = 10;
  const std::uint64_t x = 0x15; // set 5
  f.on_l2(1, 5, x, idx++, true); // core 1 accesses x (its first L1 store is in the same epoch)
  CHECK(f.on_l1(1, 5, x, true, idx++) == (kL1Store | kL1StoreFilter));
  CHECK(f.on_l1(1, 5, x, true, idx) == 0);   // repeated store by the same core: left out
  CHECK(f.on_l1(2, 5, x, true, idx++) == (kL1Store | kL1StoreFilter | kL1EpochFirst)); // another core
  CHECK(f.on_l1(1, 5, x, true, idx++) == (kL1Store | kL1StoreFilter)); // back to core 1
  CHECK(f.on_l1(1, 5, x, true, idx) == 0);
  f.on_l2(0, 5, x, idx++, true); // any core's access to x after it: core 1's next store is recorded
  CHECK(f.on_l1(1, 5, x, true, idx++) == (kL1Store | kL1StoreFilter));
  // a load is recorded by the epoch rule only, and makes the next store of the line recordable
  CHECK(f.on_l1(0, 5, x, false, idx++) == 0);  // core 0's own access started this epoch
  f.on_l2(0, 5, 0x25, idx++, false);
  CHECK(f.on_l1(0, 5, x, false, idx++) == kL1EpochFirst);
  CHECK(f.on_l1(1, 5, x, true, idx++) == (kL1Store | kL1StoreFilter));
  // a store recorded by the epoch rule alone (nothing since core 1's last recorded store) still counts as the line's
  // last recorded store
  f.on_l2(1, 5, 0x35, idx++, false);
  CHECK(f.on_l1(1, 5, x, true, idx++) == (kL1Store | kL1EpochFirst));
  CHECK(f.on_l1(1, 5, x, true, idx) == 0);
}

TEST_CASE("trace: set sampling is a fixed residue class, the same in every cache")
{
  Sampling s;
  CHECK(s.modulus == 8);
  CHECK(s.residue == 0);
  CHECK(s.sampled(0));
  CHECK(s.sampled(504));
  CHECK_FALSE(s.sampled(505));
  s.modulus = 1;
  for (std::uint32_t i = 0; i < 512; ++i)
    CHECK(s.sampled(i));
  s.modulus = 16;
  s.residue = 5;
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < 512; ++i)
    n += s.sampled(i);
  CHECK(n == 32);
  CHECK(s.sampled(21));
}

TEST_CASE("trace: the writer hands out records in order, in blocks, and counts them by type")
{
  std::vector<unsigned char> sink;
  auto put = [&](const void *d, std::size_t n) {
    const unsigned char *p = static_cast<const unsigned char *>(d);
    sink.insert(sink.end(), p, p + n);
  };
  Writer<decltype(put)> w(put, 3);
  CHECK(w.append(make(Type::header, 0, 1, 8, kSchemaVersion, kMagic)) == 0);
  CHECK(w.append(access_record(0, 1, 2, false, false, false, State::I, false, false, 0)) == 1);
  CHECK(sink.empty());
  CHECK(w.append(make(Type::roi, 0, 1, 0, 0, 0)) == 2); // third record fills the block
  CHECK(sink.size() == 48);
  w.append(make(Type::end, 0, 0, 0, 0, 3));
  CHECK(sink.size() == 48);
  w.flush();
  REQUIRE(sink.size() == 64);
  CHECK(w.records() == 4);
  CHECK(w.count(Type::access) == 1);
  CHECK(w.count(Type::header) == 1);
  CHECK(sink[16] == (unsigned char)Type::access);
  CHECK(sink[48] == (unsigned char)Type::end);
}
