#include <stdint.h>
#include <stddef.h>
#include "../lib/minigb_apu/minigb_apu.h"

void reference_audio_init();
void reference_audio_write(uint16_t, uint8_t);
uint8_t reference_audio_read(uint16_t);
void reference_audio_callback(void*, int16_t*, size_t);
extern "C" unsigned fixture_state(uint8_t*);
extern "C" unsigned reference_fixture_state(uint8_t*);
extern "C" void fixture_boundary(unsigned);
extern "C" void reference_fixture_boundary(unsigned);

extern "C" void* memset(void* ptr, int value, size_t count) {
    uint8_t* p = static_cast<uint8_t*>(ptr);
    while (count--) *p++ = static_cast<uint8_t>(value);
    return ptr;
}

static uint32_t rng;
static uint32_t next_random() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void write_both(uint16_t address, uint8_t value) {
    audio_write(address, value);
    reference_audio_write(address, value);
}

static int16_t reference_pcm[AUDIO_SAMPLES * 2];
static int16_t actual_pcm[AUDIO_SAMPLES * 2];
static uint8_t reference_state[1024], actual_state[1024];

extern "C" int run_apu_case(unsigned test) {
    rng = 0x12345678u ^ (test * 0x9e3779b9u);
    // Explicitly reset power before init so cases do not inherit register state.
    write_both(0xFF26, 0);
    audio_init();
    reference_audio_init();
    write_both(0xFF26, 0x80);
    write_both(0xFF24, test & 1 ? 0x77 : next_random());
    write_both(0xFF25, test & 1 ? 0xff : next_random());
    for (unsigned i = 0; i < 16; ++i) write_both(0xFF30 + i, next_random());
    const uint16_t frequencies[] = {0, 1, 31, 512, 1024, 1792, 2000, 2047};
    for (unsigned ch = 0; ch < 4; ++ch) {
        const uint16_t base = 0xFF10 + ch * 5;
        const uint16_t freq = frequencies[(test + ch) & 7];
        if (ch == 0) write_both(base, test % 3 ? 0 : next_random() & 0x7f);
        if (ch == 2) write_both(base, 0x80);
        write_both(base + 1, (test & 3) << 6 | (next_random() & 63));
        write_both(base + 2, ch == 2 ? ((test >> 2) & 3) << 5 : 0xF0 | (test & 15));
        write_both(base + 3, ch == 3 ? test & 255 : freq & 255);
        write_both(base + 4, 0x80 | (test & 16 ? 0x40 : 0) | (freq >> 8));
    }
    fixture_boundary(test);
    reference_fixture_boundary(test);

    for (unsigned frame = 0; frame < 8; ++frame) {
        if (frame >= 2) {
            // Frequency/route/envelope/wave RAM updates between callbacks,
            // including retrigger, power off/on and disabled channels.
            for (unsigned change = 0; change < 8; ++change) {
                uint16_t addr = 0xFF10 + next_random() % 48;
                write_both(addr, next_random());
            }
        }
        reference_audio_callback(nullptr, reference_pcm, sizeof(reference_pcm));
        audio_callback(nullptr, actual_pcm, sizeof(actual_pcm));
        for (unsigned i = 0; i < AUDIO_SAMPLES * 2; ++i)
            if (reference_pcm[i] != actual_pcm[i]) return 10000 + frame * 1000 + i;
        const unsigned size = reference_fixture_state(reference_state);
        if (fixture_state(actual_state) != size) return 20000;
        for (unsigned i = 0; i < size; ++i)
            if (reference_state[i] != actual_state[i]) return 30000 + frame * 1000 + i;
        for (unsigned addr = 0xFF10; addr <= 0xFF3F; ++addr)
            if (audio_read(addr) != reference_audio_read(addr)) return 40000 + addr;
    }
    return 0;
}

// Independent identical starting points for guest instruction-count comparison.
extern "C" void prepare_apu_benchmark() {
    (void)run_apu_case(1);
    write_both(0xFF26, 0);
    audio_init();
    reference_audio_init();
    write_both(0xFF26, 0x80);
    write_both(0xFF24, 0x77);
    write_both(0xFF25, 0xFF);
    write_both(0xFF10, 0);
    for (unsigned ch = 0; ch < 4; ++ch) {
        const unsigned base = 0xFF10 + ch * 5;
        if (ch == 2) write_both(base, 0x80);
        write_both(base + 1, 0x80);
        write_both(base + 2, ch == 2 ? 0x20 : 0xF0);
        write_both(base + 3, ch == 3 ? 0x21 : 0x40);
        write_both(base + 4, 0x86);
    }
}
extern "C" void benchmark_reference() {
    reference_audio_callback(nullptr, reference_pcm, sizeof(reference_pcm));
}
extern "C" void benchmark_actual() {
    audio_callback(nullptr, actual_pcm, sizeof(actual_pcm));
}
