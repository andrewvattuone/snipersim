#pragma once

// Test instrumentation: an ordered per-line event record for a range of cache lines (observation only: changes no state,
// message or timing). Used by experiments/sharer-counts/run_regress.sh to check coherence_regress line by line
// instead of through whole-run miss totals (see COHERENCE_VALIDATION.md, "Per-line check").
//
// Enabled by SNIPER_WATCH_VADDR (virtual address in hex) and SNIPER_WATCH_LINES (number of 64-byte lines,
// default 1). The memory manager maps each watched virtual line to its physical line before the first data access
// to it; from then on every insert (demand fill / upgrade refill, with the state granted), eviction, invalidation
// (by reason) and downgrade (M/E -> S) of that line is recorded, per cache level and core, with a global sequence
// number taken under one lock, i.e. in the order the controllers performed them. At the end of the simulation the
// events are written to linewatch.csv in the output directory (one row per event). Assumes 64-byte lines.

#include "fixed_types.h"
#include "mem_component.h"

namespace ParametricDramDirectoryMSI
{
   namespace LineWatch
   {
      enum event_t
      {
         FILL_DEMAND,
         FILL_UPGRADE_REFILL,
         INV_COHERENCE,
         INV_BACK_INVALIDATION,
         INV_UPGRADE,
         INV_OTHER,
         EVICTION,
         DOWNGRADE,
         NUM_EVENTS
      };

      extern bool g_enabled;

      inline bool active() { return g_enabled; }
      // A data access to virtual address vaddr at physical address paddr is about to be performed
      void noteAccess(IntPtr vaddr, IntPtr paddr);
      // An event of a cache (level, core) on the line containing physical address paddr; state = the state the line
      // gets (fills, downgrades) or had (invalidations, evictions), as CStateString's character
      void note(MemComponent::component_t component, core_id_t core, IntPtr paddr, event_t event, char state);
      // Registers the end-of-simulation dump (once)
      void init();
   }
}
