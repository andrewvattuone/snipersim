// Controlled sharing microbenchmark (reads, writes, migratory sharing) for the
// sharer-count instrumentation in experiments/sharer-counts.
// Usage: sharetest MODE K   (MODE = r | w | p | u, 1 <= K <= 4)
// 4 threads; threads 0..K-1 take part in the sharing phase on a 128 KB shared
// buffer, then all threads stream a private 1 MB buffer twice so the shared
// lines get evicted from L1/L2 and (with a small LLC) from L3.
//   r: K threads read every shared line twice (read sharing, E->S downgrades);
//      identical to the original single-mode sharetest.
//   w: K threads read every line; thread 0 then writes every line (S->M upgrade,
//      invalidating the other K-1 copies); threads 1..K-1 re-read the first half
//      (M->S downgrade of thread 0, re-fills by the readers). Expected sharers at
//      L3 eviction: first half K, second half 1.
//   p: migratory: for 4 rounds, each of threads 0..K-1 in turn writes every
//      line (ownership moves, invalidating the previous writer). Expected: 1.
//   u: upgrade by the last reader: threads 0..K-1 read every line one after the
//      other (barrier between them), then thread K-1 writes every line, then
//      threads 0..K-2 re-read it. Under MESI the write invalidates the K-1 other
//      copies and the re-reads miss. Checks whether Sniper's shared-L3 model,
//      where a read fill inherits the L3's E/M state, still invalidates them.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 4
#define SHARED_BYTES (128 * 1024)
#define PRIVATE_BYTES (1024 * 1024)
#define LINE 64

static volatile char shared_buf[SHARED_BYTES] __attribute__((aligned(4096)));
static char *private_buf[NTHREADS];
static int K;
static char MODE;
static pthread_barrier_t barrier;
static volatile long sink[NTHREADS * 16];

static long read_range(long from, long to)
{
   long sum = 0;
   for (long i = from; i < to; i += LINE)
      sum += shared_buf[i];
   return sum;
}

static void write_range(long from, long to, long val)
{
   for (long i = from; i < to; i += LINE)
      shared_buf[i] = (char)(i + val);
}

static void *worker(void *arg)
{
   long id = (long)arg;
   long sum = 0;
   pthread_barrier_wait(&barrier);
   if (MODE == 'r')
   {
      if (id < K)
         for (int rep = 0; rep < 2; rep++)
            sum += read_range(0, SHARED_BYTES);
   }
   else if (MODE == 'w')
   {
      if (id < K)
         sum += read_range(0, SHARED_BYTES);
      pthread_barrier_wait(&barrier);
      if (id == 0)
         write_range(0, SHARED_BYTES, 1);
      pthread_barrier_wait(&barrier);
      if (id > 0 && id < K)
         sum += read_range(0, SHARED_BYTES / 2);
   }
   else if (MODE == 'u')
   {
      for (int t = 0; t < K; t++)
      {
         if (id == t)
            sum += read_range(0, SHARED_BYTES);
         pthread_barrier_wait(&barrier);
      }
      if (id == K - 1)
         write_range(0, SHARED_BYTES, 1);
      pthread_barrier_wait(&barrier);
      if (id < K - 1)
         sum += read_range(0, SHARED_BYTES);
   }
   else // 'p'
   {
      for (int round = 0; round < 4; round++)
         for (int t = 0; t < K; t++)
         {
            if (id == t)
               write_range(0, SHARED_BYTES, round * K + t);
            pthread_barrier_wait(&barrier);
         }
   }
   pthread_barrier_wait(&barrier);
   for (int rep = 0; rep < 2; rep++)
      for (long i = 0; i < PRIVATE_BYTES; i += LINE)
         sum += private_buf[id][i]++;
   sink[id * 16] = sum;
   return NULL;
}

int main(int argc, char **argv)
{
   MODE = argc > 1 ? argv[1][0] : 'r';
   K = argc > 2 ? atoi(argv[2]) : 1;
   if (K < 1 || K > NTHREADS || (MODE != 'r' && MODE != 'w' && MODE != 'p' && MODE != 'u'))
   {
      fprintf(stderr, "usage: sharetest r|w|p|u K(1..4)\n");
      return 1;
   }
   for (long i = 0; i < SHARED_BYTES; i += LINE) shared_buf[i] = (char)i;
   for (int t = 0; t < NTHREADS; t++)
      if (posix_memalign((void **)&private_buf[t], 4096, PRIVATE_BYTES)) return 1;
   for (int t = 0; t < NTHREADS; t++)
      for (long i = 0; i < PRIVATE_BYTES; i += 4096) private_buf[t][i] = 0;
   pthread_barrier_init(&barrier, NULL, NTHREADS);
   pthread_t th[NTHREADS];
   for (long t = 1; t < NTHREADS; t++) pthread_create(&th[t], NULL, worker, (void *)t);
   worker((void *)0);
   for (long t = 1; t < NTHREADS; t++) pthread_join(th[t], NULL);
   printf("done MODE=%c K=%d\n", MODE, K);
   return 0;
}
