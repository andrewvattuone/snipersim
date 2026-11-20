// Deterministic coherence regression test (2 threads, barrier-synchronized).
//
// Usage: coherence_regress {clean|dirty} {write|read} [NLINES]
//   step 0 (dirty only): thread 0 writes every line, so the shared cache holds it dirty
//   step 1: thread 0 reads every line
//   step 2: thread 1 reads every line                  (expected MESI: both S)
//   step 3: thread 1 writes every line   ("write")     (expected: T1 M, T0 invalidated)
//           thread 1 reads it instead    ("read")      (control: no coherence action)
//   step 4: thread 0 reads every line                  (expected after write: miss, T1 M->S)
// The values returned by every read are checked (exit status 1 on a mismatch).
// NLINES (default 1) lines of 64 bytes are used, all in one static array that is
// initialized at compile time, so no core touches them before step 0/1. With
// NLINES = 1 the line address printed below can be traced with SNIPER_TRACE_VADDR
// (build with -no-pie so that the address is the same under the simulator).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLINES 256
#define WORDS_PER_LINE 8

static volatile long regress_lines[MAXLINES * WORDS_PER_LINE] __attribute__((aligned(64))) = {7};
static int nlines = 1, dirty = 0, do_write = 1;
static long seen1[MAXLINES], seen2[MAXLINES], seen4[MAXLINES];
static pthread_barrier_t barrier;

static long expected_initial(int i) { return dirty ? 500 + i : (i == 0 ? 7 : 0); }

static void *thread1(void *arg)
{
   (void)arg;
   pthread_barrier_wait(&barrier); // after step 0
   pthread_barrier_wait(&barrier); // after step 1
   for (int i = 0; i < nlines; i++) // step 2
      seen2[i] = regress_lines[i * WORDS_PER_LINE];
   pthread_barrier_wait(&barrier);
   for (int i = 0; i < nlines; i++) // step 3
      if (do_write)
         regress_lines[i * WORDS_PER_LINE] = 1000 + i;
      else
         seen2[i] += regress_lines[i * WORDS_PER_LINE] - seen2[i];
   pthread_barrier_wait(&barrier);
   pthread_barrier_wait(&barrier); // after step 4
   return NULL;
}

int main(int argc, char **argv)
{
   if (argc < 3 || (strcmp(argv[1], "clean") && strcmp(argv[1], "dirty")) || (strcmp(argv[2], "write") && strcmp(argv[2], "read")))
   {
      fprintf(stderr, "usage: coherence_regress {clean|dirty} {write|read} [NLINES <= %d]\n", MAXLINES);
      return 2;
   }
   dirty = !strcmp(argv[1], "dirty");
   do_write = !strcmp(argv[2], "write");
   nlines = argc > 3 ? atoi(argv[3]) : 1;
   if (nlines < 1 || nlines > MAXLINES)
      return 2;
   printf("regress_lines at %p, %d line(s), %s, step 3 %s\n", (void *)regress_lines, nlines, argv[1], argv[2]);
   fflush(stdout);

   pthread_barrier_init(&barrier, NULL, 2);
   pthread_t t1;
   pthread_create(&t1, NULL, thread1, NULL);

   if (dirty) // step 0
      for (int i = 0; i < nlines; i++)
         regress_lines[i * WORDS_PER_LINE] = 500 + i;
   pthread_barrier_wait(&barrier);
   for (int i = 0; i < nlines; i++) // step 1
      seen1[i] = regress_lines[i * WORDS_PER_LINE];
   pthread_barrier_wait(&barrier);
   pthread_barrier_wait(&barrier); // after step 2
   pthread_barrier_wait(&barrier); // after step 3
   for (int i = 0; i < nlines; i++) // step 4
      seen4[i] = regress_lines[i * WORDS_PER_LINE];
   pthread_barrier_wait(&barrier);
   pthread_join(t1, NULL);

   int errors = 0;
   for (int i = 0; i < nlines; i++)
   {
      long init = expected_initial(i), after = do_write ? 1000 + i : init;
      if (seen1[i] != init || seen2[i] != init || seen4[i] != after)
      {
         if (errors++ < 5)
            printf("line %d: step1 %ld step2 %ld step4 %ld, expected %ld %ld %ld\n", i, seen1[i], seen2[i], seen4[i], init, init, after);
      }
   }
   printf("data check: %s (%d of %d lines wrong)\n", errors ? "FAIL" : "OK", errors, nlines);
   return errors ? 1 : 0;
}
