#include "utils/audio_timestamp_rebaser.h"

#include <cassert>

int main() {
  AudioTimestampRebaser rebaser;

  assert(rebaser.rebase(95'000'000'000ULL, 20'000'000ULL) == 0);
  assert(rebaser.rebase(95'020'000'000ULL, 20'000'000ULL) == 20'000'000ULL);
  assert(rebaser.rebase(95'010'000'000ULL, 20'000'000ULL) == 40'000'000ULL);

  rebaser.reset();
  assert(rebaser.rebase(180'000'000'000ULL, 20'000'000ULL) == 0);
  return 0;
}