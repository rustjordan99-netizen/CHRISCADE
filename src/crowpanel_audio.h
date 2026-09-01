#pragma once

#include <stddef.h>
#include <stdint.h>

void crowpanel_audio_init();
void crowpanel_audio_submit(const int16_t* stereo_samples, size_t stereo_bytes);
void crowpanel_audio_shutdown();

// When the physical volume pot is at its minimum, stop the APU mix and PWM
// DMA work entirely. Turning it back up resumes audio automatically.
bool crowpanel_audio_performance_muted();
// Preserve the configured DMA channel/APU state across the SD screenshot pause.
void crowpanel_audio_screenshot_pause();
void crowpanel_audio_screenshot_resume(bool saved, int16_t* stereo_scratch);
