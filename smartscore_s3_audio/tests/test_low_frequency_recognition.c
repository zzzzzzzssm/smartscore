#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chord_detector.h"
#include "low_frequency_analyzer.h"
#include "music_detector_config.h"
#include "note_utils.h"

#define TEST_PI 3.14159265358979323846f
#define TEST_BLOCKS 14
#define TEST_MAX_OSCILLATORS 3

typedef enum {
    TEST_WAVE_SINE = 0,
    TEST_WAVE_PIANO_LIKE,
    TEST_WAVE_OCTAVE_TRAP,
    TEST_WAVE_NOISE,
} test_waveform_t;

typedef struct {
    int midi;
    float amplitude;
} test_oscillator_t;

typedef struct {
    test_waveform_t waveform;
    test_oscillator_t oscillators[TEST_MAX_OSCILLATORS];
    int oscillator_count;
} synthetic_case_t;

typedef struct {
    bool saw_single;
    bool saw_interval;
    bool saw_chord;
    bool saw_wrong_octave;
    int single_midi;
    int interval_notes[2];
    int chord_notes[3];
    low_frequency_result_t last;
} observed_result_t;

static int s_failures;
static uint32_t s_noise_state = 0x53434F52U;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n",                \
                    __FILE__, __LINE__, #condition);                        \
            ++s_failures;                                                   \
        }                                                                   \
    } while (0)

static float test_noise(void)
{
    s_noise_state = s_noise_state * 1664525U + 1013904223U;
    return ((float)((s_noise_state >> 8) & 0x00FFFFFFU) /
            8388607.5f) - 1.0f;
}

static bool same_notes(const int *actual, int actual_count,
                       const int *expected, int expected_count)
{
    if (actual_count != expected_count) return false;
    bool used[3] = {false};
    for (int expected_index = 0; expected_index < expected_count;
         ++expected_index) {
        bool found = false;
        for (int actual_index = 0; actual_index < actual_count;
             ++actual_index) {
            if (!used[actual_index] &&
                actual[actual_index] == expected[expected_index]) {
                used[actual_index] = true;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static float envelope_at(uint64_t sample_index)
{
    const float seconds = (float)sample_index / MUSIC_SAMPLE_RATE_HZ;
    const float attack = fminf(1.0f, seconds / 0.018f);
    return attack * expf(-0.28f * seconds);
}

static float render_oscillator(const synthetic_case_t *test_case,
                               const test_oscillator_t *oscillator,
                               uint64_t sample_index)
{
    const float frequency = note_midi_to_frequency(oscillator->midi);
    const float phase = 2.0f * TEST_PI * frequency *
                        (float)sample_index / MUSIC_SAMPLE_RATE_HZ;
    if (test_case->waveform == TEST_WAVE_PIANO_LIKE) {
        static const float harmonics[] = {1.0f, 0.52f, 0.29f, 0.17f, 0.10f};
        float sample = 0.0f;
        for (size_t harmonic = 0;
             harmonic < sizeof(harmonics) / sizeof(harmonics[0]);
             ++harmonic) {
            sample += harmonics[harmonic] *
                      sinf(phase * (float)(harmonic + 1U));
        }
        return oscillator->amplitude * 0.48f * sample;
    }
    if (test_case->waveform == TEST_WAVE_OCTAVE_TRAP) {
        return oscillator->amplitude *
            (0.09f * sinf(phase) + sinf(phase * 2.0f) +
             0.22f * sinf(phase * 3.0f));
    }
    return oscillator->amplitude * sinf(phase);
}

static void render_block(const synthetic_case_t *test_case,
                         uint64_t first_sample, float *output)
{
    for (size_t index = 0; index < MUSIC_CAPTURE_FRAMES; ++index) {
        const uint64_t sample_index = first_sample + index;
        if (test_case->waveform == TEST_WAVE_NOISE) {
            output[index] = 0.004f * test_noise();
            continue;
        }
        float sample = 0.0f;
        for (int oscillator = 0;
             oscillator < test_case->oscillator_count; ++oscillator) {
            sample += render_oscillator(test_case,
                                        &test_case->oscillators[oscillator],
                                        sample_index);
        }
        output[index] = envelope_at(sample_index) * sample;
    }
}

static void observe(const low_frequency_result_t *result,
                    int expected_single_midi, observed_result_t *observed)
{
    observed->last = *result;
    if (result->final_kind == LOW_FREQUENCY_RESULT_SINGLE) {
        observed->saw_single = true;
        observed->single_midi = result->midi_notes[0];
        if (expected_single_midi >= MUSIC_LOW_MIDI_MIN &&
            result->midi_notes[0] == expected_single_midi + 12) {
            observed->saw_wrong_octave = true;
        }
    } else if (result->final_kind == LOW_FREQUENCY_RESULT_INTERVAL &&
               result->note_count == 2) {
        observed->saw_interval = true;
        memcpy(observed->interval_notes, result->midi_notes,
               sizeof(observed->interval_notes));
    } else if (result->final_kind == LOW_FREQUENCY_RESULT_CHORD &&
               result->note_count == 3) {
        observed->saw_chord = true;
        memcpy(observed->chord_notes, result->midi_notes,
               sizeof(observed->chord_notes));
    }
}

static observed_result_t run_synthetic(const synthetic_case_t *test_case,
                                       int expected_single_midi,
                                       float low_snr_db,
                                       bool low_band_above_gate)
{
    observed_result_t observed = {0};
    float block[MUSIC_CAPTURE_FRAMES];
    const float snr[AUDIO_PREPROCESS_BAND_COUNT] = {
        low_snr_db, 24.0f, 22.0f,
    };
    low_frequency_analyzer_reset();
    for (int block_index = 0; block_index < TEST_BLOCKS; ++block_index) {
        render_block(test_case,
                     (uint64_t)block_index * MUSIC_CAPTURE_FRAMES, block);
        low_frequency_analyzer_push(block, MUSIC_CAPTURE_FRAMES, 1);
        low_frequency_result_t result;
        low_frequency_analyzer_analyze(
            true, (block_index & 1) != 0, low_band_above_gate, false,
            block_index == 0, 0.0001f, snr, &result);
        observe(&result, expected_single_midi, &observed);
    }
    return observed;
}

static void test_c2_to_g4_pure_sine_sweep(void)
{
    for (int midi = 36; midi <= 67; ++midi) {
        const synthetic_case_t test_case = {
            .waveform = TEST_WAVE_SINE,
            .oscillators = {{midi, 0.12f}},
            .oscillator_count = 1,
        };
        const observed_result_t observed =
            run_synthetic(&test_case, midi, 30.0f, true);
        if (!observed.saw_single || observed.single_midi != midi ||
            observed.saw_wrong_octave) {
            fprintf(stderr,
                    "pure sine MIDI %d: single=%d midi=%d wrong_octave=%d "
                    "reject=%s\n",
                    midi, observed.saw_single, observed.single_midi,
                    observed.saw_wrong_octave,
                    low_frequency_reject_reason_name(
                        observed.last.reject_reason));
            ++s_failures;
        }
    }
}

static void test_piano_like_single_sweep(void)
{
    for (int midi = 36; midi <= 67; ++midi) {
        const synthetic_case_t test_case = {
            .waveform = TEST_WAVE_PIANO_LIKE,
            .oscillators = {{midi, 0.10f}},
            .oscillator_count = 1,
        };
        const observed_result_t observed =
            run_synthetic(&test_case, midi, 28.0f, true);
        CHECK(observed.saw_single);
        CHECK(observed.single_midi == midi);
        CHECK(!observed.saw_wrong_octave);
    }
}

static void test_intervals(void)
{
    static const struct {
        int notes[2];
        float amplitudes[2];
    } cases[] = {
        {{36, 37}, {0.09f, 0.09f}}, /* adjacent semitone */
        {{36, 40}, {0.09f, 0.09f}}, /* major third */
        {{36, 43}, {0.09f, 0.09f}}, /* fifth */
        {{36, 48}, {0.09f, 0.09f}}, /* independently struck octave */
        {{38, 57}, {0.09f, 0.045f}}, /* low plus high, 6 dB */
        {{43, 50}, {0.09f, 0.0225f}}, /* fifth, 12 dB */
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const synthetic_case_t test_case = {
            .waveform = TEST_WAVE_PIANO_LIKE,
            .oscillators = {
                {cases[index].notes[0], cases[index].amplitudes[0]},
                {cases[index].notes[1], cases[index].amplitudes[1]},
            },
            .oscillator_count = 2,
        };
        const observed_result_t observed =
            run_synthetic(&test_case, -1, 30.0f, true);
        if (!observed.saw_interval ||
            !same_notes(observed.interval_notes, 2, cases[index].notes, 2)) {
            fprintf(stderr, "interval %d+%d not recovered; reject=%s\n",
                    cases[index].notes[0], cases[index].notes[1],
                    low_frequency_reject_reason_name(
                        observed.last.reject_reason));
            ++s_failures;
        }
    }
}

static void test_major_and_minor_triads(void)
{
    static const int cases[][3] = {
        {36, 40, 43}, /* C major */
        {38, 41, 45}, /* D minor */
        {43, 47, 50}, /* G major */
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const synthetic_case_t test_case = {
            .waveform = TEST_WAVE_PIANO_LIKE,
            .oscillators = {
                {cases[index][0], 0.065f},
                {cases[index][1], 0.065f},
                {cases[index][2], 0.065f},
            },
            .oscillator_count = 3,
        };
        const observed_result_t observed =
            run_synthetic(&test_case, -1, 30.0f, true);
        if (!observed.saw_chord ||
            !same_notes(observed.chord_notes, 3, cases[index], 3)) {
            fprintf(stderr, "triad %d+%d+%d not recovered; reject=%s\n",
                    cases[index][0], cases[index][1], cases[index][2],
                    low_frequency_reject_reason_name(
                        observed.last.reject_reason));
            ++s_failures;
        }
    }
}

static void test_band_gate_and_noise_rejection(void)
{
    const synthetic_case_t noise = {
        .waveform = TEST_WAVE_NOISE,
    };
    observed_result_t observed = run_synthetic(&noise, -1, 3.0f, true);
    CHECK(!observed.saw_single);
    CHECK(!observed.saw_interval);
    CHECK(!observed.saw_chord);
    CHECK(observed.last.reject_reason == LOW_FREQUENCY_REJECT_LOW_BAND_SNR);

    observed = run_synthetic(&noise, -1, 30.0f, false);
    CHECK(observed.last.reject_reason ==
          LOW_FREQUENCY_REJECT_BELOW_BAND_GATE);
}

static float preprocess_tone(float frequency_hz, float amplitude,
                             audio_preprocess_state_t *state,
                             audio_frame_metrics_t *out_metrics)
{
    int16_t input[MUSIC_CAPTURE_FRAMES] = {0};
    float output[MUSIC_CAPTURE_FRAMES];
    audio_preprocess_init(state);
    audio_preprocess_frame(state, input, output, MUSIC_CAPTURE_FRAMES,
                           out_metrics);
    audio_preprocess_finish_calibration(state);
    uint64_t first_sample = 0;
    for (int block_index = 0; block_index < 8; ++block_index) {
        for (size_t index = 0; index < MUSIC_CAPTURE_FRAMES; ++index) {
            const float phase = 2.0f * TEST_PI * frequency_hz *
                (float)(first_sample + index) / MUSIC_SAMPLE_RATE_HZ;
            input[index] = (int16_t)lrintf(
                32767.0f * amplitude * sinf(phase));
        }
        audio_preprocess_frame(state, input, output, MUSIC_CAPTURE_FRAMES,
                               out_metrics);
        first_sample += MUSIC_CAPTURE_FRAMES;
    }
    return out_metrics->rms;
}

static void test_preprocess_high_pass_and_band_gate(void)
{
    audio_preprocess_state_t c2_state;
    audio_preprocess_state_t rumble_state;
    audio_frame_metrics_t c2_metrics;
    audio_frame_metrics_t rumble_metrics;
    const float c2_rms = preprocess_tone(
        note_midi_to_frequency(36), 0.02f, &c2_state, &c2_metrics);
    const float rumble_rms = preprocess_tone(
        10.0f, 0.02f, &rumble_state, &rumble_metrics);
    CHECK(c2_rms > rumble_rms * 4.0f);
    CHECK(audio_preprocess_band_above_gate(
        &c2_state, &c2_metrics, AUDIO_PREPROCESS_BAND_LOW));
    CHECK(audio_preprocess_band_snr_db(
        &c2_state, &c2_metrics, AUDIO_PREPROCESS_BAND_LOW) >=
        MUSIC_LOW_BAND_MIN_SNR_DB);
}

static void test_second_harmonic_octave_trap(void)
{
    static const int fundamentals[] = {36, 40, 43, 47};
    for (size_t index = 0;
         index < sizeof(fundamentals) / sizeof(fundamentals[0]); ++index) {
        const synthetic_case_t test_case = {
            .waveform = TEST_WAVE_OCTAVE_TRAP,
            .oscillators = {{fundamentals[index], 0.11f}},
            .oscillator_count = 1,
        };
        const observed_result_t observed = run_synthetic(
            &test_case, fundamentals[index], 30.0f, true);
        CHECK(observed.saw_single);
        CHECK(observed.single_midi == fundamentals[index]);
        CHECK(!observed.saw_wrong_octave);
    }
}

static uint16_t read_u16_le(const uint8_t bytes[2])
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32_le(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Manifest rows are: wav_path,expected_midi,onset_start_ms,onset_end_ms.
 * WAV fixtures must be PCM16, 24 kHz, mono or stereo. Paths may be absolute
 * or relative to the test process working directory. */
static bool test_real_piano_wav(const char *path, int expected_midi,
                                uint32_t onset_start_ms,
                                uint32_t onset_end_ms)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "real piano fixture cannot be opened: %s\n", path);
        return false;
    }
    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(file);
        fprintf(stderr, "not a RIFF/WAVE fixture: %s\n", path);
        return false;
    }
    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t sample_rate = 0;
    long data_offset = -1;
    uint32_t data_size = 0;
    uint8_t chunk_header[8];
    while (fread(chunk_header, 1, sizeof(chunk_header), file) ==
           sizeof(chunk_header)) {
        const uint32_t chunk_size = read_u32_le(chunk_header + 4);
        if (memcmp(chunk_header, "fmt ", 4) == 0 && chunk_size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) break;
            format = read_u16_le(fmt);
            channels = read_u16_le(fmt + 2);
            sample_rate = read_u32_le(fmt + 4);
            bits = read_u16_le(fmt + 14);
            if (chunk_size > sizeof(fmt)) {
                fseek(file, (long)(chunk_size - sizeof(fmt)), SEEK_CUR);
            }
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_offset = ftell(file);
            data_size = chunk_size;
            fseek(file, (long)chunk_size, SEEK_CUR);
        } else {
            fseek(file, (long)chunk_size, SEEK_CUR);
        }
        if ((chunk_size & 1U) != 0U) fseek(file, 1, SEEK_CUR);
        if (format != 0 && data_offset >= 0) break;
    }
    if (format != 1 || (channels != 1 && channels != 2) || bits != 16 ||
        sample_rate != MUSIC_SAMPLE_RATE_HZ || data_offset < 0 ||
        data_size < channels * sizeof(int16_t)) {
        fclose(file);
        fprintf(stderr,
                "fixture must be PCM16/%d Hz mono or stereo: %s\n",
                MUSIC_SAMPLE_RATE_HZ, path);
        return false;
    }
    if (fseek(file, data_offset, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    low_frequency_analyzer_reset();
    bool matched = false;
    uint64_t sample_index = 0;
    float block[MUSIC_CAPTURE_FRAMES];
    int16_t pcm[MUSIC_CAPTURE_FRAMES * 2];
    int16_t mono[MUSIC_CAPTURE_FRAMES];
    audio_preprocess_state_t preprocess;
    audio_frame_metrics_t metrics = {0};
    audio_preprocess_init(&preprocess);
    memset(mono, 0, sizeof(mono));
    audio_preprocess_frame(&preprocess, mono, block, MUSIC_CAPTURE_FRAMES,
                           &metrics);
    uint32_t remaining_frames = data_size /
        (channels * (uint32_t)sizeof(int16_t));
    int block_index = 0;
    while (remaining_frames > 0) {
        const size_t frames = remaining_frames < MUSIC_CAPTURE_FRAMES
            ? remaining_frames : MUSIC_CAPTURE_FRAMES;
        const size_t samples = frames * channels;
        if (fread(pcm, sizeof(int16_t), samples, file) != samples) break;
        for (size_t frame = 0; frame < MUSIC_CAPTURE_FRAMES; ++frame) {
            if (frame >= frames) {
                mono[frame] = 0;
            } else if (channels == 1) {
                mono[frame] = pcm[frame];
            } else {
                mono[frame] = (int16_t)(((int32_t)pcm[2 * frame] +
                                         (int32_t)pcm[2 * frame + 1]) / 2);
            }
        }
        const uint32_t block_ms = (uint32_t)(sample_index * 1000U /
                                             MUSIC_SAMPLE_RATE_HZ);
        if (!preprocess.calibrated && block_ms >= onset_start_ms) {
            audio_preprocess_finish_calibration(&preprocess);
        }
        audio_preprocess_frame(&preprocess, mono, block, MUSIC_CAPTURE_FRAMES,
                               &metrics);
        float snr[AUDIO_PREPROCESS_BAND_COUNT];
        for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
            snr[band] = audio_preprocess_band_snr_db(
                &preprocess, &metrics, (audio_preprocess_band_t)band);
        }
        const bool onset = block_ms >= onset_start_ms &&
                           block_ms <= onset_end_ms;
        const bool low_gate = audio_preprocess_band_above_gate(
            &preprocess, &metrics, AUDIO_PREPROCESS_BAND_LOW);
        low_frequency_analyzer_push(block, MUSIC_CAPTURE_FRAMES, 1);
        low_frequency_result_t result;
        low_frequency_analyzer_analyze(
            preprocess.calibrated && low_gate,
            (block_index & 1) != 0, low_gate, metrics.clipped, onset,
            preprocess.band_noise_floor[AUDIO_PREPROCESS_BAND_LOW],
            snr, &result);
        if (result.final_kind == LOW_FREQUENCY_RESULT_SINGLE &&
            result.midi_notes[0] == expected_midi) {
            matched = true;
        }
        sample_index += frames;
        remaining_frames -= (uint32_t)frames;
        ++block_index;
    }
    fclose(file);
    if (!matched) {
        fprintf(stderr, "real piano fixture did not recover MIDI %d: %s\n",
                expected_midi, path);
    }
    return matched;
}

static int run_real_piano_manifest(void)
{
    const char *manifest_path = getenv("SMARTSCORE_REAL_PIANO_MANIFEST");
    if (manifest_path == NULL || manifest_path[0] == '\0') {
        fprintf(stderr,
                "REAL_PIANO_FIXTURES_MISSING: set "
                "SMARTSCORE_REAL_PIANO_MANIFEST; real-piano regression is "
                "incomplete and must not be reported as passed.\n");
        return -1;
    }
    FILE *manifest = fopen(manifest_path, "r");
    if (manifest == NULL) {
        fprintf(stderr, "cannot open real-piano manifest: %s\n",
                manifest_path);
        return -1;
    }
    int count = 0;
    char row[768];
    while (fgets(row, sizeof(row), manifest) != NULL) {
        if (row[0] == '#' || row[0] == '\r' || row[0] == '\n') continue;
        char path[512];
        int midi = -1;
        unsigned onset_start_ms = 0;
        unsigned onset_end_ms = 0;
        if (sscanf(row, "%511[^,],%d,%u,%u", path, &midi,
                   &onset_start_ms, &onset_end_ms) != 4 ||
            midi < 36 || midi > 67 || onset_end_ms < onset_start_ms) {
            fprintf(stderr, "invalid real-piano manifest row: %s", row);
            ++s_failures;
            continue;
        }
        ++count;
        if (!test_real_piano_wav(path, midi, onset_start_ms, onset_end_ms)) {
            ++s_failures;
        }
    }
    fclose(manifest);
    if (count == 0) {
        fprintf(stderr, "real-piano manifest contains no fixtures: %s\n",
                manifest_path);
        return -1;
    }
    return count;
}

int main(void)
{
    low_frequency_analyzer_init();
    if (chord_detector_init() != 0) {
        fprintf(stderr, "low-frequency FFT backend initialization failed\n");
        return 1;
    }

    test_c2_to_g4_pure_sine_sweep();
    test_piano_like_single_sweep();
    test_intervals();
    test_major_and_minor_triads();
    test_band_gate_and_noise_rejection();
    test_preprocess_high_pass_and_band_gate();
    test_second_harmonic_octave_trap();
    const int real_fixture_count = run_real_piano_manifest();

    if (s_failures != 0) {
        fprintf(stderr, "%d low-frequency recognition test(s) failed\n",
                s_failures);
        return 1;
    }
    if (real_fixture_count < 0) return 2;
    printf("low-frequency recognition tests passed with %d real fixture(s)\n",
           real_fixture_count);
    return 0;
}
