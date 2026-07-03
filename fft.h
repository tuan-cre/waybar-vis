#ifndef WAYBAR_VIS_FFT_H
#define WAYBAR_VIS_FFT_H

#include <stddef.h>

void calculate_spectrum(const float *pcm, size_t n, float *freq_out,
                        int sample_rate);

#endif
