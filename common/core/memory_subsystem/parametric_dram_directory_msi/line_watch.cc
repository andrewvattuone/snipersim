#include "line_watch.h"
#include "config.h"
#include "hooks_manager.h"
#include "lock.h"
#include "simulator.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>

namespace ParametricDramDirectoryMSI
{
   namespace LineWatch
   {
      namespace
      {
         IntPtr parseStart()
         {
            const char *s = getenv("SNIPER_WATCH_VADDR");
            return s ? strtoull(s, NULL, 16) >> 6 : 0;
         }
         UInt64 parseLines()
         {
            const char *s = getenv("SNIPER_WATCH_LINES");
            return s ? strtoull(s, NULL, 10) : 1;
         }

         const IntPtr s_vstart = parseStart();
         const UInt64 s_lines = parseLines();
         Lock s_lock;
         std::unordered_map<IntPtr, UInt64> s_index;  // physical line -> watched line index
         std::map<UInt64, IntPtr> s_vline;            // watched line index -> physical line
         struct Event
         {
            UInt64 seq;
            UInt64 line;
            int component;
            core_id_t core;
            event_t event;
            char state;
         };
         std::vector<Event> s_events;
         bool s_initialized = false;

         const char *s_event_names[NUM_EVENTS] = {"fill_demand", "fill_upgrade_refill", "inv_coherence", "inv_back_invalidation",
                                                  "inv_upgrade", "inv_other", "eviction", "downgrade"};

         SInt64 dump(UInt64, UInt64)
         {
            ScopedLock sl(s_lock);
            const String path = Sim()->getConfig()->formatOutputFileName("linewatch.csv");
            FILE *f = fopen(path.c_str(), "w");
            if (!f)
               return 0;
            fprintf(f, "# lines=%lu vaddr=%#lx mapped=%lu\nseq,line,component,core,event,state\n", (unsigned long)s_lines,
                    (unsigned long)(s_vstart << 6), (unsigned long)s_vline.size());
            for (const Event &e : s_events)
               fprintf(f, "%lu,%lu,%s,%d,%s,%c\n", (unsigned long)e.seq, (unsigned long)e.line,
                       MemComponentString((MemComponent::component_t)e.component), e.core, s_event_names[e.event], e.state);
            fclose(f);
            return 0;
         }
      }

      bool g_enabled = getenv("SNIPER_WATCH_VADDR") != NULL;

      void init()
      {
         if (!g_enabled)
            return;
         ScopedLock sl(s_lock);
         if (s_initialized)
            return;
         s_initialized = true;
         Sim()->getHooksManager()->registerHook(HookType::HOOK_SIM_END, dump, 0);
      }

      void noteAccess(IntPtr vaddr, IntPtr paddr)
      {
         const IntPtr vline = vaddr >> 6;
         if (vline < s_vstart || vline >= s_vstart + s_lines)
            return;
         ScopedLock sl(s_lock);
         const UInt64 index = vline - s_vstart;
         if (s_vline.count(index) == 0)
         {
            s_vline[index] = paddr >> 6;
            s_index[paddr >> 6] = index;
         }
      }

      void note(MemComponent::component_t component, core_id_t core, IntPtr paddr, event_t event, char state)
      {
         ScopedLock sl(s_lock);
         auto it = s_index.find(paddr >> 6);
         if (it == s_index.end())
            return;
         s_events.push_back({s_events.size(), it->second, (int)component, core, event, state});
      }
   }
}
