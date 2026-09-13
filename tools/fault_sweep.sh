#!/bin/bash
#
# Fails one I/O call at a time, everywhere it can happen, and checks that the
# database still opens and still holds everything it acknowledged.
#
# The sweep matters more than any single injection.  A crash test kills the
# process at a random instant and mostly lands somewhere ordinary; this walks
# every fsync of the manifest in turn, so the rare instant that breaks the
# database is visited deliberately rather than waited for.
#
# tests/test_faults.cpp does the same for every kind of call, on every
# platform, on the simulated disk, and cuts the power afterwards too.  This
# is kept because it goes through the real system calls.
#
# Usage:  tools/fault_sweep.sh [build-dir] [seeds]
#
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$REPO/build}"
SEEDS="${2:-3}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

LIB="$WORK/fault_inject.so"
gcc -shared -fPIC -o "$LIB" "$REPO/tools/fault_inject.c" -ldl || exit 1

# A workload built for this: keys drawn from a small range so level 0 files
# overlap and real compactions run, and a memtable small enough that several
# flushes and installs happen per phase.
cat > "$WORK/workload.cpp" <<'CPP'
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include "ambar/db.hpp"
using namespace ambar;
static std::string K(int i){char b[32];snprintf(b,sizeof b,"k%06d",i);return b;}
int main(int argc, char** argv) {
  const std::string dir = argv[1], phase = argv[2];
  const int n = atoi(argv[3]), gen = atoi(argv[4]);
  Options o; o.create_if_missing = true;
  o.write_buffer_size = 64 << 10; o.max_file_size = 1 << 20;
  std::unique_ptr<DB> db; Status s = DB::open(o, dir, &db);
  if (!s.is_ok()) { std::cout << "open: " << s.to_string() << "\n"; return 10; }
  if (phase == "verify") {
    std::map<std::string,std::string> model;
    std::ifstream f(dir + ".jnl"); std::string k, v;
    while (f >> k >> v) model[k] = v;
    int lost = 0, wrong = 0; std::string got;
    for (const auto& kv : model) {
      Status g = db->get(ReadOptions(), kv.first, &got);
      if (!g.is_ok()) ++lost; else if (got != kv.second) ++wrong;
    }
    std::cout << "lost=" << lost << " wrong=" << wrong
              << " acknowledged=" << model.size() << "\n";
    return (lost || wrong) ? 1 : 0;
  }
  std::ofstream j(dir + ".jnl", std::ios::app);
  std::mt19937 r(999 + gen);
  for (int i = 0; i < n; ++i) {
    const std::string key = K(static_cast<int>(r() % 4000));
    char vb[40]; snprintf(vb, sizeof vb, "g%02d_i%07d_", gen, i);
    std::string v = vb; v.append(150, 'q');
    // Journalled only after the write returns, so the journal never claims
    // more than the engine acknowledged.
    if (db->put(WriteOptions(), key, v).is_ok()) { j << key << " " << v << "\n"; j.flush(); }
  }
  return 0;
}
CPP

g++ -O1 -std=c++17 -I"$REPO/include" -I"$REPO/src" "$WORK/workload.cpp" \
    "$BUILD/libambar.a" -pthread -o "$WORK/workload" || exit 1

# How many manifest fsyncs one phase performs, so every one can be visited.
rm -rf "$WORK/count" "$WORK/count.jnl"
"$WORK/workload" "$WORK/count" write 4000 1 >/dev/null 2>&1
POINTS=$(FAULT_MODE=fsync FAULT_MATCH=MANIFEST FAULT_N=-1 LD_PRELOAD="$LIB" \
         "$WORK/workload" "$WORK/count" write 4000 2 2>&1 >/dev/null | grep -c "fsync #")
[ "$POINTS" -eq 0 ] && POINTS=20
echo "sweeping $POINTS manifest fsync points x $SEEDS seeds"

clean=0; unopenable=0; lost=0; landed=0; total=0
for seed in $(seq 1 "$SEEDS"); do
  for n in $(seq 0 $((POINTS - 1))); do
    total=$((total + 1))
    D="$WORK/db_${seed}_$n"
    "$WORK/workload" "$D" write 4000 "$seed" >/dev/null 2>&1
    err=$(FAULT_MODE=fsync FAULT_MATCH=MANIFEST FAULT_N=$n LD_PRELOAD="$LIB" \
          "$WORK/workload" "$D" write 4000 $((seed + 10)) 2>&1 >/dev/null)
    echo "$err" | grep -q EIO && landed=$((landed + 1))

    out=$("$WORK/workload" "$D" verify 0 0 2>&1)
    if echo "$out" | grep -q "^open:"; then
      unopenable=$((unopenable + 1))
      [ "$unopenable" -le 3 ] && echo "  UNOPENABLE seed=$seed n=$n: $out"
    elif echo "$out" | grep -qE "lost=[1-9]|wrong=[1-9]"; then
      lost=$((lost + 1))
      [ "$lost" -le 3 ] && echo "  DATA LOSS seed=$seed n=$n: $out"
    else
      clean=$((clean + 1))
    fi
    rm -rf "$D" "$D.jnl"
  done
done

echo
echo "$total runs, $landed faults landed: clean=$clean unopenable=$unopenable data_loss=$lost"
[ "$unopenable" -eq 0 ] && [ "$lost" -eq 0 ]
