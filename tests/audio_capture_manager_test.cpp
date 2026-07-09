#include "utils/audio_capture_manager.h"

#include <cassert>
#include <cstdint>
#include <vector>

void test_dispatcher_fans_out_and_unregisters_consumers() {
  AudioFrameDispatcher dispatcher;
  std::vector<uint64_t> first;
  std::vector<uint64_t> second;

  const size_t first_id = dispatcher.register_consumer(
      [&first](const AudioFrame &frame) { first.push_back(frame.seq); });
  const size_t second_id = dispatcher.register_consumer(
      [&second](const AudioFrame &frame) { second.push_back(frame.seq); });

  assert(first_id != 0);
  assert(second_id != 0);
  assert(first_id != second_id);
  assert(dispatcher.consumer_count() == 2);

  const uint8_t sample[] = {1, 2, 3, 4};
  AudioFrame frame;
  frame.data = sample;
  frame.size = sizeof(sample);
  frame.seq = 7;
  dispatcher.dispatch(frame);

  assert(first.size() == 1);
  assert(first[0] == 7);
  assert(second.size() == 1);
  assert(second[0] == 7);

  dispatcher.unregister_consumer(first_id);
  assert(dispatcher.consumer_count() == 1);

  frame.seq = 8;
  dispatcher.dispatch(frame);

  assert(first.size() == 1);
  assert(second.size() == 2);
  assert(second[1] == 8);

  dispatcher.unregister_consumer(second_id);
  assert(dispatcher.consumer_count() == 0);
}

void test_audio_timestamp_rebaser_starts_each_stream_at_zero() {
  AudioTimestampRebaser rebaser;

  assert(rebaser.rebase(95'000'000'000ULL, 20'000'000ULL) == 0);
  assert(rebaser.rebase(95'020'000'000ULL, 20'000'000ULL) == 20'000'000ULL);
  assert(rebaser.rebase(95'010'000'000ULL, 20'000'000ULL) == 40'000'000ULL);

  rebaser.reset();
  assert(rebaser.rebase(180'000'000'000ULL, 20'000'000ULL) == 0);
}

int main() {
  test_dispatcher_fans_out_and_unregisters_consumers();
  test_audio_timestamp_rebaser_starts_each_stream_at_zero();
  return 0;
}
