#pragma once

// Debug-only coherence trace of one cache line (observation only: changes no
// state or timing). Enabled by setting SNIPER_TRACE_VADDR to a virtual address
// in hex: the first data access to that line by any core records its physical
// line; from then on every core access to it prints a snapshot of the line in
// every L1-D, L2 and L3 and in its home directory, and every insert, update,
// invalidation, eviction and directory message for it is printed as it happens.
// Output goes to stderr, prefixed with [LINETRACE]. Assumes 64-byte lines.

#include "fixed_types.h"

namespace ParametricDramDirectoryMSI
{
   namespace LineTracer
   {
      extern bool g_enabled;
      extern IntPtr g_vline; // virtual line (address >> 6) to trace
      extern IntPtr g_pline; // its physical line, once seen; 0 before

      inline bool active() { return g_enabled; }
      inline bool matchesVirtual(IntPtr vaddr) { return g_enabled && (vaddr >> 6) == g_vline; }
      inline bool matchesPhysical(IntPtr paddr) { return g_enabled && g_pline != 0 && (paddr >> 6) == g_pline; }
      void notePhysical(IntPtr paddr);
      void print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
      const char *msgTypeName(UInt32 msg_type);
      const char *reasonName(UInt32 reason);
   }
}
