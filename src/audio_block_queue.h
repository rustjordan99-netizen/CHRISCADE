#pragma once

#include <stdint.h>

// Single producer and completion-IRQ consumer, both on core 0. The driver
// must disable interrupts around all queue operations. Sample conversion is
// outside that critical section and only ever touches a FILLING block.
struct AudioBlockQueue {
  static constexpr uint8_t capacity = 3;
  enum State : uint8_t { FREE, FILLING, READY, PLAYING };
  State state[capacity];
  uint8_t writer;
  uint8_t reader;
  uint8_t ready_count;
  int8_t playing;
  bool primed;

  void reset() {
    for (uint8_t i = 0; i < capacity; ++i) state[i] = FREE;
    writer = reader = ready_count = 0;
    playing = -1;
    primed = false;
  }

  int reserve() {
    if (state[writer] != FREE) return -1;
    state[writer] = FILLING;
    return writer;
  }

  bool publish(uint8_t slot) {
    if (slot != writer || state[slot] != FILLING) return false;
    state[slot] = READY;
    ++ready_count;
    if (++writer == capacity) writer = 0;
    return true;
  }

  int start_next() {
    if (playing >= 0) return -1;
    if (!primed) {
      if (ready_count != capacity) return -1;
      primed = true;
    }
    if (state[reader] != READY) return -1;
    const uint8_t slot = reader;
    state[slot] = PLAYING;
    playing = slot;
    --ready_count;
    if (++reader == capacity) reader = 0;
    return slot;
  }

  void complete() {
    if (playing < 0) return;
    state[playing] = FREE;
    playing = -1;
  }
};
