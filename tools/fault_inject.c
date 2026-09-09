/* Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
 *
 * Makes one I/O call fail, so that the engine's error paths can be run rather
 * than reasoned about.
 *
 * Every durability argument has the shape "if this call fails at this instant,
 * then afterwards ...".  Reading the code establishes what the branch does;
 * only running it establishes what the database looks like afterwards.  This
 * library intercepts fsync and rename, counts the calls whose path matches a
 * pattern, and returns EIO from the one the caller names.
 *
 * It found a real defect.  Compaction used to delete its output files when the
 * manifest install failed -- but the edit is appended before it is synced, so a
 * failing fsync leaves a record naming files that were then deleted, and the
 * database refuses to open ever again with all of its data still on disk.
 * Sweeping every manifest fsync in a workload left 17 of 85 injection points
 * permanently unopenable.  After the fix, 51 of 51 were clean.
 *
 * Build and use:
 *
 *     gcc -shared -fPIC -o /tmp/fault_inject.so tools/fault_inject.c -ldl
 *     FAULT_MODE=fsync FAULT_MATCH=MANIFEST FAULT_N=3 \
 *       LD_PRELOAD=/tmp/fault_inject.so ./your_program
 *
 * FAULT_MODE   fsync | rename
 * FAULT_MATCH  substring a path must contain to be counted
 * FAULT_N      which matching call fails, counting from zero
 *
 * tools/fault_sweep.sh drives it across every injection point in a workload.
 *
 * Linux and other systems with LD_PRELOAD only.  It is a test harness, not
 * part of the engine, and nothing in src/ knows it exists.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int (*real_fsync)(int);
static int (*real_rename)(const char*, const char*);

static const char* match;
static const char* mode;
static long trigger = -1;
static long counter = 0;

__attribute__((constructor)) static void init(void) {
  real_fsync = dlsym(RTLD_NEXT, "fsync");
  real_rename = dlsym(RTLD_NEXT, "rename");
  match = getenv("FAULT_MATCH");
  mode = getenv("FAULT_MODE");
  const char* n = getenv("FAULT_N");
  trigger = n ? atol(n) : -1;
}

/* The path behind a descriptor, so a call can be matched by the file it is
   about rather than by the order it happens to occur in. */
static int path_of_fd(int fd, char* buf, size_t n) {
  char link[64];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  const ssize_t len = readlink(link, buf, n - 1);
  if (len < 0) return -1;
  buf[len] = '\0';
  return 0;
}

int fsync(int fd) {
  if (mode && strcmp(mode, "fsync") == 0 && match) {
    char path[4096];
    if (path_of_fd(fd, path, sizeof(path)) == 0 && strstr(path, match)) {
      const long n = counter++;
      if (n == trigger) {
        fprintf(stderr, "[fault] fsync #%ld on %s -> EIO\n", n, path);
        errno = EIO;
        return -1;
      }
    }
  }
  return real_fsync(fd);
}

int rename(const char* from, const char* to) {
  if (mode && strcmp(mode, "rename") == 0 && match &&
      (strstr(from, match) || strstr(to, match))) {
    const long n = counter++;
    if (n == trigger) {
      fprintf(stderr, "[fault] rename #%ld %s -> %s -> EIO\n", n, from, to);
      errno = EIO;
      return -1;
    }
  }
  return real_rename(from, to);
}
