#include "line_tracer.h"
#include "shmem_msg.h"
#include "cache_cntlr.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace ParametricDramDirectoryMSI
{
   namespace LineTracer
   {
      static IntPtr parseTarget()
      {
         const char *s = getenv("SNIPER_TRACE_VADDR");
         return s ? strtoull(s, NULL, 16) >> 6 : 0;
      }

      IntPtr g_vline = parseTarget();
      bool g_enabled = getenv("SNIPER_TRACE_VADDR") != NULL;
      IntPtr g_pline = 0;
      static Lock s_print_lock;

      void notePhysical(IntPtr paddr)
      {
         if (g_pline == 0)
         {
            g_pline = paddr >> 6;
            print("tracing virtual line %#lx = physical line %#lx", (unsigned long)(g_vline << 6), (unsigned long)(g_pline << 6));
         }
      }

      void print(const char *fmt, ...)
      {
         ScopedLock sl(s_print_lock);
         va_list ap;
         va_start(ap, fmt);
         fputs("[LINETRACE] ", stderr);
         vfprintf(stderr, fmt, ap);
         fputc('\n', stderr);
         va_end(ap);
      }

      const char *msgTypeName(UInt32 msg_type)
      {
         using PrL1PrL2DramDirectoryMSI::ShmemMsg;
         switch (msg_type)
         {
            case ShmemMsg::EX_REQ: return "EX_REQ";
            case ShmemMsg::SH_REQ: return "SH_REQ";
            case ShmemMsg::UPGRADE_REQ: return "UPGRADE_REQ";
            case ShmemMsg::INV_REQ: return "INV_REQ";
            case ShmemMsg::FLUSH_REQ: return "FLUSH_REQ";
            case ShmemMsg::WB_REQ: return "WB_REQ";
            case ShmemMsg::EX_REP: return "EX_REP";
            case ShmemMsg::SH_REP: return "SH_REP";
            case ShmemMsg::UPGRADE_REP: return "UPGRADE_REP";
            case ShmemMsg::INV_REP: return "INV_REP";
            case ShmemMsg::FLUSH_REP: return "FLUSH_REP";
            case ShmemMsg::WB_REP: return "WB_REP";
            case ShmemMsg::NULLIFY_REQ: return "NULLIFY_REQ";
            case ShmemMsg::DRAM_READ_REQ: return "DRAM_READ_REQ";
            case ShmemMsg::DRAM_WRITE_REQ: return "DRAM_WRITE_REQ";
            case ShmemMsg::DRAM_READ_REP: return "DRAM_READ_REP";
            default: return "?";
         }
      }

      const char *reasonName(UInt32 reason)
      {
         static const char *names[] = {"CORE_RD", "CORE_WR", "CORE_RDEX", "UPGRADE", "EVICT", "BACK_INVAL", "COHERENCY"};
         return reason < Transition::NUM_REASONS ? names[reason] : "?";
      }
   }
}
