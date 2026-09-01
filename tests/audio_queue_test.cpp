#include "../src/audio_block_queue.h"

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

static int invariants(const AudioBlockQueue& q) {
  unsigned ready = 0, playing = 0, filling = 0;
  for (uint8_t i = 0; i < q.capacity; ++i) {
    ready += q.state[i] == AudioBlockQueue::READY;
    playing += q.state[i] == AudioBlockQueue::PLAYING;
    filling += q.state[i] == AudioBlockQueue::FILLING;
  }
  CHECK(ready == q.ready_count);
  CHECK(playing <= 1 && filling <= 1);
  CHECK((q.playing >= 0) == (playing == 1));
  if (q.playing >= 0) CHECK(q.state[q.playing] == AudioBlockQueue::PLAYING);
  return 0;
}

extern "C" int run_audio_queue_tests() {
  AudioBlockQueue q;
  q.reset();
  CHECK(q.start_next() == -1);
  CHECK(!q.publish(0));
  for (uint8_t i = 0; i < q.capacity; ++i) {
    CHECK(q.reserve() == i);
    CHECK(q.reserve() == -1);
    CHECK(q.publish(i));
    CHECK(!q.publish(i));
    if (i + 1 < q.capacity) CHECK(q.start_next() == -1);
  }
  CHECK(q.start_next() == 0);
  CHECK(q.reserve() == -1); // No overwrite of playing or queued audio.
  CHECK(q.start_next() == -1);
  q.complete();
  CHECK(q.reserve() == 0);
  CHECK(q.start_next() == 1);
  q.complete();
  CHECK(q.start_next() == 2);
  q.complete();
  CHECK(q.start_next() == -1); // Slot 0 is still being converted.
  CHECK(q.state[0] == AudioBlockQueue::FILLING);
  CHECK(q.publish(0));
  CHECK(q.start_next() == 0); // Recovery without stale block replay.
  q.complete();
  CHECK(q.start_next() == -1);
  q.reset();
  q.complete();
  CHECK(invariants(q) == 0);
  CHECK(!q.primed && q.playing == -1);

  uint32_t random = 0xC01DC0DEu;
  uint32_t sequence[q.capacity];
  uint32_t produced = 0, expected = 0;
  int filling = -1;
  for (unsigned step = 0; step < 50000; ++step) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    const unsigned event = random & 7u;
    if (event < 3 && filling < 0) {
      filling = q.reserve();
      if (filling >= 0) sequence[filling] = produced++;
    } else if (event == 3 && filling >= 0) {
      CHECK(q.publish((uint8_t)filling));
      filling = -1;
    } else if (event >= 4 && event < 7) {
      q.complete(); // Includes long producer stalls and empty queues.
    } else if (event == 7 && step % 701u == 0) {
      q.reset();
      filling = -1;
      expected = produced;
    }
    const int started = q.start_next();
    if (started >= 0) CHECK(sequence[started] == expected++);
    CHECK(invariants(q) == 0);
    if (filling >= 0) CHECK(q.state[filling] == AudioBlockQueue::FILLING);
  }
  CHECK(produced > 1000);
  return 0;
}
