#ifdef APU_REFERENCE
#define audio_init reference_audio_init
#define audio_read reference_audio_read
#define audio_write reference_audio_write
#define audio_callback reference_audio_callback
#define fixture_state reference_fixture_state
#define fixture_boundary reference_fixture_boundary
#include "apu_reference.cpp"
#else
#include "../lib/minigb_apu/minigb_apu.cpp"
#endif

// Compare every state byte, not just PCM or a possibly colliding checksum.
extern "C" unsigned fixture_state(uint8_t* out) {
    unsigned n = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(chans);
    for (unsigned i = 0; i < sizeof(chans); ++i) out[n++] = p[i];
    for (unsigned i = 0; i < sizeof(audio_mem); ++i) out[n++] = audio_mem[i];
    p = reinterpret_cast<const uint8_t*>(&vol_l);
    for (unsigned i = 0; i < sizeof(vol_l); ++i) out[n++] = p[i];
    p = reinterpret_cast<const uint8_t*>(&vol_r);
    for (unsigned i = 0; i < sizeof(vol_r); ++i) out[n++] = p[i];
    return n;
}

extern "C" void fixture_boundary(unsigned mode) {
    for (unsigned i = 0; i < 4; ++i) {
        // Legal counter extrema include an exact reference-period boundary.
        chans[i].freq_counter = mode % 3 == 0 ? 0 : FREQ_INC_REF - (mode % 3 == 1);
        chans[i].muted = (mode >> (i + 2)) & 1u;
    }
}
