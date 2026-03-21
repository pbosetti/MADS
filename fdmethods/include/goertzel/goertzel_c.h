#ifndef GOERTZEL_GOERTZEL_C_H
#define GOERTZEL_GOERTZEL_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goertzel_analyzer_t goertzel_analyzer_t;

goertzel_analyzer_t *goertzel_analyzer_create(const double *alpha, size_t alpha_count);
void goertzel_analyzer_destroy(goertzel_analyzer_t *analyzer);
size_t goertzel_analyzer_size(const goertzel_analyzer_t *analyzer);
void goertzel_analyzer_reset(goertzel_analyzer_t *analyzer);
int goertzel_analyzer_process_buffer(goertzel_analyzer_t *analyzer,
                                     const double *samples,
                                     size_t sample_count);
int goertzel_analyzer_state_vector(const goertzel_analyzer_t *analyzer,
                                   double *states_out,
                                   size_t state_count);
int goertzel_analyzer_dft_terms(const goertzel_analyzer_t *analyzer,
                                const double *omega,
                                size_t omega_count,
                                double *real_out,
                                double *imag_out);

double goertzel_alpha_from_bin(double bin, size_t sample_count);
double goertzel_omega_from_bin(double bin, size_t sample_count);
double goertzel_alpha_from_frequency(double frequency_hz, double sample_rate_hz);
double goertzel_omega_from_frequency(double frequency_hz, double sample_rate_hz);

#ifdef __cplusplus
}
#endif

#endif  // GOERTZEL_GOERTZEL_C_H
