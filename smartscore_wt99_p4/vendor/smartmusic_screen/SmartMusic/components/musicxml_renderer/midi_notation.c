#include "midi_notation.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTATION_OCTAVE_MIN       (-2)
#define NOTATION_OCTAVE_SLOTS     16
#define NOTATION_VOICE_SLOTS      16
#define NOTATION_ALTER_UNKNOWN    INT8_MAX
#define NOTATION_ALTER_NO_UPDATE  (INT8_MAX - 1)

typedef struct {
    uint8_t step;
    int8_t alter;
    int8_t octave;
    int preference_cost;
} notation_spelling_t;

typedef struct {
    uint32_t cost;
    uint16_t printed;
    int8_t fifths;
} key_candidate_t;

typedef struct {
    uint8_t staff;
    uint8_t voice;
    uint8_t midi_note;
    uint32_t onset;
    uint32_t duration;
    uint8_t tie_flags;
    uint8_t slur_start;
    uint8_t slur_stop;
    uint8_t gliss_start;
    uint8_t gliss_stop;
    size_t source_begin;
    size_t source_end;
    notation_spelling_t spelling;
    music_accidental_t accidental;
    music_stem_direction_t stem;
    bool chord;
} notation_group_item_t;

typedef struct {
    uint8_t staff_index;
    uint8_t step;
    uint8_t octave_slot;
    int8_t alter;
    bool has_state;
} notation_analysis_mark_t;

static uint8_t normalized_staff(const midi_note_t *note)
{
    return note && note->staff == 2 ? 2 : 1;
}

static uint8_t normalized_voice(const midi_note_t *note)
{
    return note && note->voice ? note->voice : 1;
}

static int compare_source_notes(const midi_data_t *midi, uint16_t a,
                                uint16_t b)
{
    const midi_note_t *left = &midi->notes[a];
    const midi_note_t *right = &midi->notes[b];
#define CMP_FIELD(field)                         \
    do {                                         \
        if (left->field < right->field) return -1; \
        if (left->field > right->field) return 1;  \
    } while (0)
    CMP_FIELD(start_tick);
    uint8_t left_staff = normalized_staff(left);
    uint8_t right_staff = normalized_staff(right);
    if (left_staff != right_staff) return left_staff < right_staff ? -1 : 1;
    uint8_t left_voice = normalized_voice(left);
    uint8_t right_voice = normalized_voice(right);
    if (left_voice != right_voice) return left_voice < right_voice ? -1 : 1;
    CMP_FIELD(note);
    CMP_FIELD(duration);
    CMP_FIELD(dots);
    CMP_FIELD(tie_flags);
    CMP_FIELD(slur_start);
    CMP_FIELD(slur_stop);
    CMP_FIELD(gliss_start);
    CMP_FIELD(gliss_stop);
#undef CMP_FIELD
    return a < b ? -1 : a > b ? 1 : 0;
}

static uint16_t *make_source_order(const midi_data_t *midi)
{
    size_t count = (size_t)midi->note_count;
    if (!count || count > MAX_NOTES) return NULL;
    uint16_t *allocation = malloc(count * 2U * sizeof(*allocation));
    if (!allocation) return NULL;
    uint16_t *order = allocation;
    uint16_t *scratch = allocation + count;
    for (size_t i = 0; i < count; ++i) order[i] = (uint16_t)i;

    uint16_t *source = order;
    uint16_t *destination = scratch;
    for (size_t width = 1; width < count; width *= 2U) {
        for (size_t begin = 0; begin < count; begin += width * 2U) {
            size_t middle = begin + width;
            size_t end = begin + width * 2U;
            if (middle > count) middle = count;
            if (end > count) end = count;
            size_t left = begin;
            size_t right = middle;
            size_t out = begin;
            while (left < middle || right < end) {
                if (right == end ||
                    (left < middle && compare_source_notes(
                        midi, source[left], source[right]) <= 0)) {
                    destination[out++] = source[left++];
                } else {
                    destination[out++] = source[right++];
                }
            }
        }
        uint16_t *swap = source;
        source = destination;
        destination = swap;
        if (width > count / 2U) break;
    }
    if (source != order) memcpy(order, source, count * sizeof(*order));
    return allocation;
}

static bool meter_values(const midi_data_t *midi, int *numerator,
                         int *denominator, uint32_t *measure_ticks,
                         uint32_t *beat_ticks)
{
    if (!midi || midi->ticks_per_quarter <= 0) return false;
    int beats = midi->time_sig_num > 0 ? midi->time_sig_num : 4;
    if (beats > UINT8_MAX) return false;
    int denominator_power = midi->time_sig_den;
    if (denominator_power < 0 || denominator_power > 6)
        denominator_power = 2;
    int beat_type = 1 << denominator_power;
    uint64_t measure = (uint64_t)(uint32_t)midi->ticks_per_quarter *
                       (uint32_t)beats * 4U / (uint32_t)beat_type;
    uint64_t beat = (uint64_t)(uint32_t)midi->ticks_per_quarter * 4U /
                    (uint32_t)beat_type;
    if (!measure || measure > INT32_MAX || !beat || beat > INT32_MAX)
        return false;
    if (numerator) *numerator = beats;
    if (denominator) *denominator = beat_type;
    if (measure_ticks) *measure_ticks = (uint32_t)measure;
    if (beat_ticks) *beat_ticks = (uint32_t)beat;
    return true;
}

static void key_alterations(int fifths, int8_t alterations[7])
{
    static const uint8_t sharp_order[7] = {3, 0, 4, 1, 5, 2, 6};
    static const uint8_t flat_order[7] = {6, 2, 5, 1, 4, 0, 3};
    memset(alterations, 0, 7U * sizeof(*alterations));
    int count = fifths < 0 ? -fifths : fifths;
    if (count > 7) count = 7;
    for (int i = 0; i < count; ++i) {
        uint8_t step = fifths < 0 ? flat_order[i] : sharp_order[i];
        alterations[step] = fifths < 0 ? -1 : 1;
    }
}

static int normalized_pitch_class(int value)
{
    int pitch_class = value % 12;
    return pitch_class < 0 ? pitch_class + 12 : pitch_class;
}

static int abs_int(int value)
{
    return value < 0 ? -value : value;
}

static int expected_diatonic_distance(int semitones)
{
    semitones = abs_int(semitones);
    int octaves = semitones / 12;
    int remainder = semitones % 12;
    static const uint8_t within_octave[12] =
        {0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6};
    return octaves * 7 + within_octave[remainder];
}

static notation_spelling_t choose_spelling(
    uint8_t midi_note, int fifths,
    const notation_spelling_t *melody_previous, int melody_previous_midi,
    const notation_spelling_t *chord_previous, int chord_previous_midi)
{
    static const int natural_pitch_class[7] = {0, 2, 4, 5, 7, 9, 11};
    int8_t key_alter[7];
    key_alterations(fifths, key_alter);
    notation_spelling_t best = {0, 0,
                                (int8_t)((int)(midi_note / 12U) - 1),
                                INT_MAX};

    for (uint8_t step = 0; step < 7; ++step) {
        for (int alter = -1; alter <= 1; ++alter) {
            if (normalized_pitch_class(natural_pitch_class[step] + alter) !=
                midi_note % 12U)
                continue;
            int octave_numerator = (int)midi_note -
                                   natural_pitch_class[step] - alter;
            if (octave_numerator % 12 != 0) continue;
            int octave = octave_numerator / 12 - 1;
            int cost = alter == key_alter[step] ? 0 : 100;
            if (fifths > 0 && alter < 0) cost += 18;
            if (fifths < 0 && alter > 0) cost += 18;
            if (fifths == 0 && alter < 0) cost += 1;
            bool uncommon_single = (step == 2 && alter == 1) ||
                                   (step == 6 && alter == 1) ||
                                   (step == 0 && alter == -1) ||
                                   (step == 3 && alter == -1);
            if (uncommon_single && alter != key_alter[step]) cost += 24;

            int diatonic = octave * 7 + step;
            if (melody_previous) {
                int previous_diatonic = melody_previous->octave * 7 +
                                         melody_previous->step;
                int actual = diatonic - previous_diatonic;
                int semitones = (int)midi_note - melody_previous_midi;
                int wanted = expected_diatonic_distance(semitones);
                if (semitones < 0) wanted = -wanted;
                cost += abs_int(actual - wanted) * 3;
                if ((semitones > 0 && actual <= 0) ||
                    (semitones < 0 && actual >= 0))
                    cost += 20;
            }
            if (chord_previous) {
                int previous_diatonic = chord_previous->octave * 7 +
                                         chord_previous->step;
                int actual = diatonic - previous_diatonic;
                int semitones = (int)midi_note - chord_previous_midi;
                int wanted = expected_diatonic_distance(semitones);
                cost += abs_int(actual - wanted) * 8;
                if (actual <= 0) cost += 24;
            }

            if (cost < best.preference_cost ||
                (cost == best.preference_cost &&
                 (abs_int(alter) < abs_int(best.alter) ||
                  (abs_int(alter) == abs_int(best.alter) &&
                   (step < best.step ||
                    (step == best.step && octave < best.octave)))))) {
                best.step = step;
                best.alter = (int8_t)alter;
                best.octave = (int8_t)octave;
                best.preference_cost = cost;
            }
        }
    }
    return best;
}

static int octave_slot(int octave)
{
    int slot = octave - NOTATION_OCTAVE_MIN;
    return slot >= 0 && slot < NOTATION_OCTAVE_SLOTS ? slot : -1;
}

static void reset_accidental_state(int8_t state[2][7][NOTATION_OCTAVE_SLOTS],
                                   int fifths)
{
    int8_t key_alter[7];
    key_alterations(fifths, key_alter);
    for (int staff = 0; staff < 2; ++staff)
        for (int step = 0; step < 7; ++step)
            for (int octave = 0; octave < NOTATION_OCTAVE_SLOTS; ++octave)
                state[staff][step][octave] = key_alter[step];
}

static music_accidental_t accidental_for_alter(int8_t alter)
{
    if (alter > 0) return MUSIC_ACCIDENTAL_SHARP;
    if (alter < 0) return MUSIC_ACCIDENTAL_FLAT;
    return MUSIC_ACCIDENTAL_NATURAL;
}

static bool same_duplicate_identity(const midi_note_t *left,
                                    const midi_note_t *right)
{
    return left->start_tick == right->start_tick &&
           normalized_staff(left) == normalized_staff(right) &&
           normalized_voice(left) == normalized_voice(right) &&
           left->note == right->note &&
           left->duration == right->duration &&
           left->dots == right->dots &&
           left->tie_flags == right->tie_flags &&
           left->slur_start == right->slur_start &&
           left->slur_stop == right->slur_stop &&
           left->gliss_start == right->gliss_start &&
           left->gliss_stop == right->gliss_stop;
}

static uint32_t weighted_note_cost(const midi_note_t *note,
                                   uint32_t within_measure,
                                   uint32_t beat_ticks, int tpq)
{
    uint32_t cost = 100;
    if (beat_ticks && within_measure % beat_ticks == 0) cost += 45;
    uint64_t duration_units = tpq > 0 ?
        (uint64_t)note->duration * 20U / (uint32_t)tpq : 0;
    if (duration_units > 80U) duration_units = 80U;
    return cost + (uint32_t)duration_units;
}

static key_candidate_t score_key_candidate(
    const midi_data_t *midi, const uint16_t *order,
    notation_analysis_mark_t *marks, int fifths,
    uint32_t measure_ticks, uint32_t beat_ticks)
{
    key_candidate_t candidate = {.cost = 0, .printed = 0,
                                 .fifths = (int8_t)fifths};
    int8_t state[2][7][NOTATION_OCTAVE_SLOTS];
    reset_accidental_state(state, fifths);
    uint32_t current_measure = UINT32_MAX;
    size_t count = (size_t)midi->note_count;
    size_t position = 0;
    uint16_t onset_groups = 0;
    while (position < count) {
        uint32_t onset = midi->notes[order[position]].start_tick;
        uint32_t measure = onset / measure_ticks;
        if (measure != current_measure) {
            reset_accidental_state(state, fifths);
            current_measure = measure;
        }
        size_t group_end = position + 1U;
        while (group_end < count &&
               midi->notes[order[group_end]].start_tick == onset)
            group_end++;
        onset_groups++;

        size_t mark_count = 0;
        size_t cursor = position;
        while (cursor < group_end) {
            const midi_note_t *source = &midi->notes[order[cursor]];
            size_t duplicate_end = cursor + 1U;
            while (duplicate_end < group_end && same_duplicate_identity(
                       source, &midi->notes[order[duplicate_end]]))
                duplicate_end++;

            notation_spelling_t spelling = choose_spelling(
                source->note, fifths, NULL, 0, NULL, 0);
            uint8_t staff_index = (uint8_t)(normalized_staff(source) - 1U);
            int slot = octave_slot(spelling.octave);
            bool needs_accidental = slot < 0 ||
                state[staff_index][spelling.step][slot] != spelling.alter;
            const bool tied_continuation =
                (source->tie_flags & MIDI_NOTE_TIE_STOP) != 0;
            if (tied_continuation)
                needs_accidental = false;
            uint32_t note_cost = weighted_note_cost(
                source, onset % measure_ticks, beat_ticks,
                midi->ticks_per_quarter);
            if (needs_accidental) {
                candidate.printed++;
                candidate.cost += note_cost;
            }
            int8_t key_alter[7];
            key_alterations(fifths, key_alter);
            if (spelling.alter != key_alter[spelling.step]) {
                candidate.cost += 25;
                if (source->duration >= (uint32_t)midi->ticks_per_quarter)
                    candidate.cost += 15;
                if (beat_ticks && onset % beat_ticks == 0)
                    candidate.cost += 20;
            }
            candidate.cost += (uint32_t)(spelling.preference_cost % 100);
            marks[mark_count++] = (notation_analysis_mark_t){
                .staff_index = staff_index,
                .step = spelling.step,
                .octave_slot = slot >= 0 ? (uint8_t)slot : 0,
                .alter = spelling.alter,
                /* A barline tie carries its spelling only to the tied note;
                 * it does not establish an accidental for later free notes
                 * in the continuation measure. */
                .has_state = slot >= 0 && !tied_continuation,
            };
            cursor = duplicate_end;
        }

        int8_t updates[2][7][NOTATION_OCTAVE_SLOTS];
        memset(updates, NOTATION_ALTER_NO_UPDATE, sizeof(updates));
        for (size_t i = 0; i < mark_count; ++i) {
            notation_analysis_mark_t *mark = &marks[i];
            if (!mark->has_state) continue;
            int8_t *update = &updates[mark->staff_index][mark->step]
                                     [mark->octave_slot];
            if (*update == NOTATION_ALTER_NO_UPDATE)
                *update = mark->alter;
            else if (*update != mark->alter)
                *update = NOTATION_ALTER_UNKNOWN;
        }
        for (int staff = 0; staff < 2; ++staff)
            for (int step = 0; step < 7; ++step)
                for (int octave = 0; octave < NOTATION_OCTAVE_SLOTS; ++octave)
                    if (updates[staff][step][octave] !=
                        NOTATION_ALTER_NO_UPDATE)
                        state[staff][step][octave] =
                            updates[staff][step][octave];
        position = group_end;
    }
    candidate.cost += (uint32_t)abs_int(fifths) *
                      (30U + (uint32_t)onset_groups / 4U);
    return candidate;
}

static bool candidate_better(const key_candidate_t *left,
                             const key_candidate_t *right)
{
    if (left->cost != right->cost) return left->cost < right->cost;
    if (left->printed != right->printed)
        return left->printed < right->printed;
    int left_abs = abs_int(left->fifths);
    int right_abs = abs_int(right->fifths);
    if (left_abs != right_abs) return left_abs < right_abs;
    return left->fifths < right->fifths;
}

static bool analysis_minor_mode(const midi_data_t *midi,
                                const uint16_t *order, int fifths,
                                uint32_t measure_ticks, uint32_t beat_ticks)
{
    int major_tonic = normalized_pitch_class(fifths * 7);
    int minor_tonic = normalized_pitch_class(major_tonic + 9);
    uint32_t major_support = 0;
    uint32_t minor_support = 0;
    uint32_t last_onset = midi->notes[order[midi->note_count - 1]].start_tick;
    const midi_note_t *previous = NULL;
    for (int i = 0; i < midi->note_count; ++i) {
        const midi_note_t *note = &midi->notes[order[i]];
        if (previous && same_duplicate_identity(previous, note)) continue;
        previous = note;
        uint32_t weight = 100;
        uint64_t duration = (uint64_t)note->duration * 25U /
                            (uint32_t)midi->ticks_per_quarter;
        if (duration > 100U) duration = 100U;
        weight += (uint32_t)duration;
        if (beat_ticks && note->start_tick % beat_ticks == 0) weight += 60;
        if (note->start_tick == last_onset) weight += 220;
        int pitch_class = note->note % 12U;
        if (pitch_class == major_tonic) major_support += weight;
        if (pitch_class == minor_tonic) minor_support += weight;
        if (pitch_class == normalized_pitch_class(major_tonic + 7))
            major_support += weight / 5U;
        if (pitch_class == normalized_pitch_class(minor_tonic + 7))
            minor_support += weight / 5U;
    }
    (void)measure_ticks;
    uint32_t margin = major_support / 10U + 80U;
    return minor_support > major_support + margin;
}

bool midi_notation_analyze_key(const midi_data_t *midi,
                               midi_notation_key_analysis_t *analysis)
{
    if (!analysis) return false;
    memset(analysis, 0, sizeof(*analysis));
    if (!midi || midi->note_count <= 0 || midi->note_count > MAX_NOTES ||
        midi->ticks_per_quarter <= 0)
        return false;
    for (int i = 0; i < midi->note_count; ++i)
        if (midi->notes[i].note > 127 || !midi->notes[i].duration ||
            midi->notes[i].duration > INT32_MAX || midi->notes[i].dots > 3)
            return false;
    if (midi->tonality_forced &&
        (midi->tonality_sf < -7 || midi->tonality_sf > 7))
        return false;
    analysis->note_count = (uint16_t)midi->note_count;
    analysis->fifths = (int8_t)midi->tonality_sf;
    analysis->minor = midi->tonality_minor;
    if (midi->tonality_forced) {
        analysis->skipped_forced = true;
        analysis->confidence_percent = 100;
        return true;
    }

    uint32_t measure_ticks = 0;
    uint32_t beat_ticks = 0;
    if (!meter_values(midi, NULL, NULL, &measure_ticks, &beat_ticks))
        return false;
    uint16_t *order = make_source_order(midi);
    notation_analysis_mark_t *marks = calloc(
        (size_t)midi->note_count, sizeof(*marks));
    if (!order || !marks) {
        free(order);
        free(marks);
        return false;
    }

    uint64_t last_end = 0;
    uint16_t onset_groups = 0;
    uint32_t previous_onset = UINT32_MAX;
    for (int i = 0; i < midi->note_count; ++i) {
        const midi_note_t *note = &midi->notes[order[i]];
        uint64_t end = (uint64_t)note->start_tick + note->duration;
        if (end > last_end) last_end = end;
        if (note->start_tick != previous_onset) {
            onset_groups++;
            previous_onset = note->start_tick;
        }
    }
    uint64_t completed = last_end / measure_ticks;
    if (completed > UINT16_MAX) completed = UINT16_MAX;
    analysis->completed_measure_count = (uint16_t)completed;
    analysis->onset_group_count = onset_groups;

    key_candidate_t candidates[15];
    for (int fifths = -7; fifths <= 7; ++fifths)
        candidates[fifths + 7] = score_key_candidate(
            midi, order, marks, fifths, measure_ticks, beat_ticks);
    key_candidate_t best = candidates[0];
    key_candidate_t runner = {.cost = UINT32_MAX, .printed = UINT16_MAX,
                              .fifths = 0};
    for (int i = 1; i < 15; ++i)
        if (candidate_better(&candidates[i], &best)) best = candidates[i];
    for (int i = 0; i < 15; ++i) {
        if (candidates[i].fifths == best.fifths) continue;
        if (runner.cost == UINT32_MAX ||
            candidate_better(&candidates[i], &runner))
            runner = candidates[i];
    }
    key_candidate_t c_major = candidates[7];
    analysis->c_printed_accidentals = c_major.printed;
    analysis->best_printed_accidentals = best.printed;
    analysis->c_notation_cost = c_major.cost;
    analysis->best_notation_cost = best.cost;
    analysis->runner_up_notation_cost = runner.cost;
    analysis->fifths = best.fifths;
    analysis->minor = analysis_minor_mode(midi, order, best.fifths,
                                          measure_ticks, beat_ticks);

    uint32_t printed_reduction = c_major.printed > best.printed ?
        (uint32_t)c_major.printed - best.printed : 0;
    uint32_t reduction_percent = c_major.printed ?
        printed_reduction * 100U / c_major.printed : 0;
    if (reduction_percent > 100) reduction_percent = 100;
    analysis->accidental_reduction_percent = (uint8_t)reduction_percent;
    uint32_t runner_margin = runner.cost > best.cost && runner.cost ?
        (runner.cost - best.cost) * 100U / runner.cost : 0;
    if (runner_margin > 100) runner_margin = 100;
    analysis->runner_up_margin_percent = (uint8_t)runner_margin;
    uint32_t cost_reduction = c_major.cost > best.cost && c_major.cost ?
        (c_major.cost - best.cost) * 100U / c_major.cost : 0;

    analysis->accepted = completed >= 2U && onset_groups >= 8U &&
        best.fifths != 0 && printed_reduction >= 3U &&
        cost_reduction >= 35U && runner_margin >= 15U;
    uint32_t confidence = (cost_reduction * 2U + runner_margin) / 3U;
    if (!analysis->accepted && confidence > 49U) confidence = 49U;
    if (confidence > 100U) confidence = 100U;
    analysis->confidence_percent = (uint8_t)confidence;

    free(marks);
    free(order);
    return true;
}

void midi_notation_build_options_from_midi(
    const midi_data_t *midi, midi_notation_build_options_t *options)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    if (!midi) return;
    options->tonality_forced = midi->tonality_forced;
    options->key_fifths = midi->tonality_sf >= -7 && midi->tonality_sf <= 7 ?
        (int8_t)midi->tonality_sf : 0;
    options->key_minor = midi->tonality_minor;
}

static music_duration_kind_t duration_kind(uint32_t ticks, int tpq,
                                           uint8_t *dots)
{
    static const music_duration_kind_t kinds[6] = {
        MUSIC_DURATION_WHOLE, MUSIC_DURATION_HALF, MUSIC_DURATION_QUARTER,
        MUSIC_DURATION_EIGHTH, MUSIC_DURATION_16TH, MUSIC_DURATION_32ND,
    };
    static const uint8_t eighth_units[6] = {32, 16, 8, 4, 2, 1};
    static const uint8_t dot_numerators[4] = {8, 12, 14, 15};
    uint64_t actual = (uint64_t)ticks * 64U;
    uint64_t best_error = UINT64_MAX;
    music_duration_kind_t best = MUSIC_DURATION_QUARTER;
    uint8_t best_dots = 0;
    for (int i = 0; i < 6; ++i) {
        for (uint8_t dotted = 0; dotted < 4; ++dotted) {
            uint64_t candidate = (uint64_t)(uint32_t)tpq *
                                 eighth_units[i] *
                                 dot_numerators[dotted];
            uint64_t error = actual > candidate ? actual - candidate :
                                                    candidate - actual;
            if (error < best_error) {
                best_error = error;
                best = kinds[i];
                best_dots = dotted;
            }
        }
    }
    if (dots) *dots = best_dots;
    return best;
}

static uint8_t beam_level(const music_note_t *note)
{
    if (!note || note->chord) return 0;
    switch (note->type) {
    case MUSIC_DURATION_EIGHTH: return 1;
    case MUSIC_DURATION_16TH: return 2;
    case MUSIC_DURATION_32ND: return 3;
    default: return 0;
    }
}

static void mark_beam_segment(music_score_t *score, const uint16_t *indices,
                              size_t count, uint8_t level)
{
    if (!score || !indices || count < 2U || level >= MUSIC_MAX_BEAM_LEVELS)
        return;
    for (size_t i = 0; i < count; ++i) {
        score->events[indices[i]].data.note.beams[level] =
            i == 0 ? MUSIC_BEAM_BEGIN :
            i + 1U == count ? MUSIC_BEAM_END : MUSIC_BEAM_CONTINUE;
    }
}

static void mark_beam_run(music_score_t *score, const uint16_t *indices,
                          size_t count)
{
    if (count < 2U) return;
    mark_beam_segment(score, indices, count, 0);
    for (uint8_t level = 1; level < MUSIC_MAX_BEAM_LEVELS; ++level) {
        size_t position = 0;
        while (position < count) {
            while (position < count &&
                   beam_level(&score->events[indices[position]].data.note) <=
                       level)
                position++;
            size_t begin = position;
            while (position < count &&
                   beam_level(&score->events[indices[position]].data.note) >
                       level)
                position++;
            size_t segment_count = position - begin;
            if (segment_count >= 2U) {
                mark_beam_segment(score, indices + begin, segment_count,
                                  level);
            } else if (segment_count == 1U) {
                music_note_t *note =
                    &score->events[indices[begin]].data.note;
                note->beams[level] = begin + 1U == count ?
                    MUSIC_BEAM_BACKWARD_HOOK : MUSIC_BEAM_FORWARD_HOOK;
            }
        }
    }
}

static void auto_beam_measure(music_score_t *score, music_measure_t *measure,
                              int tpq, int numerator, int denominator,
                              uint8_t staff_count)
{
    if (!score || !measure || tpq <= 0 || denominator <= 0) return;
    int64_t beat_value = (int64_t)tpq * 4 / denominator;
    if (denominator == 8 && numerator > 3 && numerator % 3 == 0)
        beat_value = (int64_t)tpq * 3 / 2;
    if (beat_value <= 0 || beat_value > INT32_MAX) beat_value = tpq;
    int beat = (int)beat_value;
    uint16_t run[MAX_NOTES];
    for (uint8_t staff = 1; staff <= staff_count; ++staff) {
        bool used_voices[UINT8_MAX + 1U] = {false};
        for (uint16_t offset = 0; offset < measure->event_count; ++offset) {
            music_event_t *event =
                &score->events[measure->event_start + offset];
            if (event->kind == MUSIC_EVENT_NOTE &&
                event->data.note.staff == staff &&
                !event->data.note.chord)
                used_voices[event->data.note.voice] = true;
        }
        for (uint16_t voice = 1; voice <= UINT8_MAX; ++voice) {
            if (!used_voices[voice]) continue;
            size_t run_count = 0;
            int64_t previous_end = -1;
            int run_beat = -1;
            for (uint16_t offset = 0; offset < measure->event_count; ++offset) {
                uint16_t event_index = measure->event_start + offset;
                music_event_t *event = &score->events[event_index];
                if (event->kind != MUSIC_EVENT_NOTE) continue;
                music_note_t *note = &event->data.note;
                if (note->staff != staff || note->voice != voice || note->chord)
                    continue;
                uint8_t level = beam_level(note);
                int current_beat = event->onset_divisions / beat;
                bool contiguous = level && run_count &&
                    current_beat == run_beat &&
                    event->onset_divisions == previous_end;
                if (!contiguous && run_count) {
                    mark_beam_run(score, run, run_count);
                    run_count = 0;
                }
                if (!level) {
                    previous_end = -1;
                    run_beat = -1;
                    continue;
                }
                if (!run_count) run_beat = current_beat;
                if (run_count < MAX_NOTES) run[run_count++] = event_index;
                previous_end = (int64_t)event->onset_divisions +
                               note->duration_divisions;
            }
            if (run_count) mark_beam_run(score, run, run_count);
        }
    }
}

static uint8_t score_staff_count(const midi_data_t *midi)
{
    for (int i = 0; i < midi->note_count; ++i)
        if (midi->notes[i].staff == 2) return 2;
    return 1;
}

static void initialize_score(music_score_t *score, const midi_data_t *midi,
                             int fifths, bool minor, int numerator,
                             int denominator, uint32_t measure_ticks,
                             uint16_t measure_count, uint8_t staff_count)
{
    music_score_init(score);
    const char *title = midi->title[0] ? midi->title : "Piano score";
    size_t title_length = strlen(title);
    if (title_length >= sizeof(score->title))
        title_length = sizeof(score->title) - 1U;
    memcpy(score->title, title, title_length);
    score->title[title_length] = '\0';
    score->part_count = 1;
    score->measure_count = measure_count;
    music_part_t *part = &score->parts[0];
    memcpy(part->id, "P1", 3);
    memcpy(part->name, "Piano", 6);
    part->measure_start = 0;
    part->measure_count = measure_count;
    part->staff_count = staff_count;
    for (uint16_t i = 0; i < measure_count; ++i) {
        music_measure_t *measure = &score->measures[i];
        unsigned number = (unsigned)i + 1U;
        snprintf(measure->number, sizeof(measure->number), "%u", number);
        measure->divisions = midi->ticks_per_quarter;
        measure->duration_divisions = (int32_t)measure_ticks;
        measure->staff_count = staff_count;
        measure->clefs[0] = (music_clef_t){MUSIC_CLEF_TREBLE, 2, 0};
        if (staff_count == 2)
            measure->clefs[1] = (music_clef_t){MUSIC_CLEF_BASS, 4, 0};
        measure->key.fifths = (int8_t)fifths;
        measure->key.minor = minor;
        measure->time = (music_time_signature_t){
            .beats = (uint8_t)numerator,
            .beat_type = (uint8_t)denominator,
            .symbol = MUSIC_TIME_NUMERIC,
        };
        measure->right_barline = i + 1U == measure_count ?
            MUSIC_BARLINE_FINAL : MUSIC_BARLINE_SINGLE;
    }
}

static size_t collect_group_items(
    const midi_data_t *midi, const uint16_t *order, size_t begin, size_t end,
    notation_group_item_t *items, int fifths,
    notation_spelling_t previous[2][NOTATION_VOICE_SLOTS],
    bool previous_valid[2][NOTATION_VOICE_SLOTS],
    int previous_midi[2][NOTATION_VOICE_SLOTS],
    int8_t accidental_state[2][7][NOTATION_OCTAVE_SLOTS],
    uint16_t *deduplicated)
{
    size_t item_count = 0;
    size_t cursor = begin;
    while (cursor < end) {
        const midi_note_t *source = &midi->notes[order[cursor]];
        size_t duplicate_end = cursor + 1U;
        while (duplicate_end < end && same_duplicate_identity(
                   source, &midi->notes[order[duplicate_end]]))
            duplicate_end++;
        notation_group_item_t *item = &items[item_count++];
        memset(item, 0, sizeof(*item));
        item->staff = normalized_staff(source);
        item->voice = normalized_voice(source);
        item->midi_note = source->note;
        item->onset = source->start_tick;
        item->duration = source->duration;
        item->source_begin = cursor;
        item->source_end = duplicate_end;
        for (size_t i = cursor; i < duplicate_end; ++i) {
            const midi_note_t *duplicate = &midi->notes[order[i]];
            if (duplicate->duration > item->duration)
                item->duration = duplicate->duration;
            item->tie_flags |= duplicate->tie_flags;
            item->slur_start |= duplicate->slur_start;
            item->slur_stop |= duplicate->slur_stop;
            item->gliss_start |= duplicate->gliss_start;
            item->gliss_stop |= duplicate->gliss_stop;
        }
        if (deduplicated)
            *deduplicated += (uint16_t)(duplicate_end - cursor - 1U);

        uint8_t staff_index = item->staff - 1U;
        int voice_slot = item->voice <= NOTATION_VOICE_SLOTS ?
            item->voice - 1 : -1;
        const notation_spelling_t *melody =
            voice_slot >= 0 && previous_valid[staff_index][voice_slot] ?
            &previous[staff_index][voice_slot] : NULL;
        int melody_midi = voice_slot >= 0 ?
            previous_midi[staff_index][voice_slot] : 0;
        const notation_spelling_t *chord = NULL;
        int chord_midi = 0;
        if (item_count > 1U) {
            notation_group_item_t *prior = &items[item_count - 2U];
            if (prior->staff == item->staff && prior->voice == item->voice) {
                chord = &prior->spelling;
                chord_midi = prior->midi_note;
                item->chord = true;
            }
        }
        item->spelling = choose_spelling(item->midi_note, fifths, melody,
                                         melody_midi, chord, chord_midi);
        int slot = octave_slot(item->spelling.octave);
        bool required = slot < 0 ||
            accidental_state[staff_index][item->spelling.step][slot] !=
                item->spelling.alter;
        if (item->tie_flags & MIDI_NOTE_TIE_STOP) required = false;
        item->accidental = required ?
            accidental_for_alter(item->spelling.alter) : MUSIC_ACCIDENTAL_NONE;
        cursor = duplicate_end;
    }

    /* MIDI Note-Off times inside a played chord routinely differ by a few
     * keys. Written notes sharing one onset, staff, voice and stem must share
     * one rhythmic value, so choose the deterministic modal duration (longer
     * wins a tie) without changing the source performance data. */
    for (size_t group_begin = 0; group_begin < item_count;) {
        size_t group_end = group_begin + 1U;
        while (group_end < item_count &&
               items[group_end].staff == items[group_begin].staff &&
               items[group_end].voice == items[group_begin].voice)
            group_end++;
        bool has_tie = false;
        for (size_t i = group_begin; i < group_end; ++i)
            if (items[i].tie_flags) has_tie = true;
        /* A tied segment has an exact temporal endpoint. Use that endpoint
         * as the written rhythm for its played chord, rather than shortening
         * the tie to the modal Note-Off of neighbouring keys. Creator splits
         * all barline ties at the same boundary, so tied members agree. */
        if (has_tie) {
            uint32_t tied_duration = 0;
            for (size_t i = group_begin; i < group_end; ++i)
                if (items[i].tie_flags &&
                    items[i].duration > tied_duration)
                    tied_duration = items[i].duration;
            for (size_t i = group_begin; i < group_end; ++i)
                items[i].duration = tied_duration;
            group_begin = group_end;
            continue;
        }
        uint32_t selected_duration = items[group_begin].duration;
        size_t selected_count = 0;
        for (size_t candidate = group_begin; candidate < group_end;
             ++candidate) {
            size_t count = 0;
            for (size_t probe = group_begin; probe < group_end; ++probe)
                if (items[probe].duration == items[candidate].duration)
                    count++;
            if (count > selected_count ||
                (count == selected_count &&
                 items[candidate].duration > selected_duration)) {
                selected_count = count;
                selected_duration = items[candidate].duration;
            }
        }
        for (size_t i = group_begin; i < group_end; ++i)
            items[i].duration = selected_duration;
        group_begin = group_end;
    }

    for (uint8_t staff = 1; staff <= 2; ++staff) {
        uint8_t first_voice = 0;
        uint8_t voice_count = 0;
        for (size_t i = 0; i < item_count; ++i) {
            if (items[i].staff != staff ||
                (i > 0 && items[i - 1].staff == staff &&
                 items[i - 1].voice == items[i].voice))
                continue;
            if (!first_voice) first_voice = items[i].voice;
            voice_count++;
        }
        for (size_t begin_chord = 0; begin_chord < item_count;) {
            if (items[begin_chord].staff != staff) {
                begin_chord++;
                continue;
            }
            size_t end_chord = begin_chord + 1U;
            while (end_chord < item_count &&
                   items[end_chord].staff == staff &&
                   items[end_chord].voice == items[begin_chord].voice)
                end_chord++;
            music_stem_direction_t stem;
            if (voice_count > 1U) {
                stem = items[begin_chord].voice == first_voice ?
                    MUSIC_STEM_UP : MUSIC_STEM_DOWN;
            } else {
                int low = items[begin_chord].midi_note;
                int high = items[end_chord - 1U].midi_note;
                int middle = staff == 1 ? 71 : 50; /* B4 / D3 middle line. */
                stem = low + high < middle * 2 ?
                    MUSIC_STEM_UP : MUSIC_STEM_DOWN;
            }
            for (size_t i = begin_chord; i < end_chord; ++i)
                items[i].stem = stem;
            begin_chord = end_chord;
        }
    }

    int8_t updates[2][7][NOTATION_OCTAVE_SLOTS];
    memset(updates, NOTATION_ALTER_NO_UPDATE, sizeof(updates));
    for (size_t i = 0; i < item_count; ++i) {
        notation_group_item_t *item = &items[i];
        uint8_t staff_index = item->staff - 1U;
        int slot = octave_slot(item->spelling.octave);
        if (slot >= 0 && !(item->tie_flags & MIDI_NOTE_TIE_STOP)) {
            int8_t *update = &updates[staff_index][item->spelling.step][slot];
            if (*update == NOTATION_ALTER_NO_UPDATE)
                *update = item->spelling.alter;
            else if (*update != item->spelling.alter)
                *update = NOTATION_ALTER_UNKNOWN;
        }
        int voice_slot = item->voice <= NOTATION_VOICE_SLOTS ?
            item->voice - 1 : -1;
        if (voice_slot >= 0) {
            previous[staff_index][voice_slot] = item->spelling;
            previous_midi[staff_index][voice_slot] = item->midi_note;
            previous_valid[staff_index][voice_slot] = true;
        }
    }
    for (int staff = 0; staff < 2; ++staff)
        for (int step = 0; step < 7; ++step)
            for (int octave = 0; octave < NOTATION_OCTAVE_SLOTS; ++octave)
                if (updates[staff][step][octave] !=
                    NOTATION_ALTER_NO_UPDATE)
                    accidental_state[staff][step][octave] =
                        updates[staff][step][octave];
    return item_count;
}

midi_notation_status_t midi_notation_build_score(
    const midi_data_t *midi,
    const midi_notation_build_options_t *options,
    music_score_t *score,
    uint16_t *midi_to_event,
    size_t midi_to_event_capacity,
    midi_notation_build_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));
    if (!midi || !score || midi->note_count < 0 ||
        midi->note_count > MAX_NOTES || midi->ticks_per_quarter <= 0)
        return MIDI_NOTATION_INVALID_ARGUMENT;
    if (!midi->note_count) return MIDI_NOTATION_EMPTY;
    for (int i = 0; i < midi->note_count; ++i) {
        if (midi->notes[i].note > 127 || !midi->notes[i].duration ||
            midi->notes[i].dots > 3)
            return MIDI_NOTATION_INVALID_ARGUMENT;
        if (midi->notes[i].duration > INT32_MAX)
            return MIDI_NOTATION_CAPACITY;
    }
    if ((midi_to_event && midi_to_event_capacity < (size_t)midi->note_count) ||
        (!midi_to_event && midi_to_event_capacity))
        return MIDI_NOTATION_CAPACITY;

    midi_notation_build_options_t selected;
    if (options) selected = *options;
    else midi_notation_build_options_from_midi(midi, &selected);
    if (selected.key_fifths < -7 || selected.key_fifths > 7)
        return MIDI_NOTATION_INVALID_ARGUMENT;

    int numerator = 0;
    int denominator = 0;
    uint32_t measure_ticks = 0;
    if (!meter_values(midi, &numerator, &denominator, &measure_ticks, NULL))
        return MIDI_NOTATION_INVALID_ARGUMENT;
    midi_notation_key_analysis_t analysis = {0};
    if (selected.infer_key && !selected.tonality_forced &&
        !midi->tonality_forced) {
        if (!midi_notation_analyze_key(midi, &analysis))
            return MIDI_NOTATION_OUT_OF_MEMORY;
        if (analysis.accepted) {
            selected.key_fifths = analysis.fifths;
            selected.key_minor = analysis.minor;
        }
    }

    uint64_t last_end = 0;
    for (int i = 0; i < midi->note_count; ++i) {
        uint64_t end = (uint64_t)midi->notes[i].start_tick +
                       midi->notes[i].duration;
        if (end > last_end) last_end = end;
    }
    uint64_t measure_count_64 =
        (last_end + measure_ticks - 1U) / measure_ticks;
    if (!measure_count_64) measure_count_64 = 1;
    if (measure_count_64 > MUSIC_MAX_MEASURES ||
        midi->note_count > MUSIC_MAX_EVENTS)
        return MIDI_NOTATION_CAPACITY;
    uint16_t measure_count = (uint16_t)measure_count_64;

    uint16_t *order = make_source_order(midi);
    notation_group_item_t *items = calloc((size_t)midi->note_count,
                                          sizeof(*items));
    if (!order || !items) {
        free(order);
        free(items);
        return MIDI_NOTATION_OUT_OF_MEMORY;
    }

    if (midi_to_event)
        for (int i = 0; i < midi->note_count; ++i)
            midi_to_event[i] = MIDI_NOTATION_NO_EVENT;
    uint8_t staff_count = score_staff_count(midi);
    initialize_score(score, midi, selected.key_fifths, selected.key_minor,
                     numerator, denominator, measure_ticks, measure_count,
                     staff_count);

    notation_spelling_t previous[2][NOTATION_VOICE_SLOTS] = {{{0}}};
    bool previous_valid[2][NOTATION_VOICE_SLOTS] = {{false}};
    int previous_midi[2][NOTATION_VOICE_SLOTS] = {{0}};
    int8_t accidental_state[2][7][NOTATION_OCTAVE_SLOTS];
    reset_accidental_state(accidental_state, selected.key_fifths);
    uint32_t active_measure = UINT32_MAX;
    uint16_t deduplicated = 0;
    size_t item_total = 0;
    size_t position = 0;
    while (position < (size_t)midi->note_count) {
        uint32_t onset = midi->notes[order[position]].start_tick;
        uint32_t measure_index = onset / measure_ticks;
        if (measure_index != active_measure) {
            reset_accidental_state(accidental_state, selected.key_fifths);
            active_measure = measure_index;
        }
        size_t group_end = position + 1U;
        while (group_end < (size_t)midi->note_count &&
               midi->notes[order[group_end]].start_tick == onset)
            group_end++;
        size_t item_count = collect_group_items(
            midi, order, position, group_end, items + item_total,
            selected.key_fifths,
            previous, previous_valid, previous_midi, accidental_state,
            &deduplicated);
        if (item_count > (size_t)midi->note_count - item_total) {
            free(items);
            free(order);
            return MIDI_NOTATION_CAPACITY;
        }
        item_total += item_count;
        position = group_end;
    }

    /* A MIDI note may legally sustain across one or more barlines.  Written
     * notation represents that sound as one note segment per measure joined
     * by ties.  Creator input is already split at barlines, so its one-segment
     * path is unchanged; imported/saved preview data is normalized here. */
    size_t required_events = 0;
    for (size_t i = 0; i < item_total; ++i) {
        const notation_group_item_t *item = &items[i];
        const uint64_t end = (uint64_t)item->onset + item->duration;
        const uint64_t first_measure = item->onset / measure_ticks;
        const uint64_t last_measure = (end - 1U) / measure_ticks;
        const uint64_t segment_count = last_measure - first_measure + 1U;
        if (segment_count > MUSIC_MAX_EVENTS - required_events) {
            free(items);
            free(order);
            return MIDI_NOTATION_CAPACITY;
        }
        required_events += (size_t)segment_count;
    }

    for (uint16_t measure_index = 0; measure_index < measure_count;
         ++measure_index) {
        music_measure_t *measure = &score->measures[measure_index];
        measure->event_start = score->event_count;
        const uint64_t measure_start =
            (uint64_t)measure_index * measure_ticks;
        const uint64_t measure_end = measure_start + measure_ticks;
        for (size_t i = 0; i < item_total; ++i) {
            notation_group_item_t *item = &items[i];
            const uint64_t item_start = item->onset;
            const uint64_t item_end = item_start + item->duration;
            if (item_start >= measure_end || item_end <= measure_start)
                continue;

            const uint64_t segment_start = item_start > measure_start ?
                item_start : measure_start;
            const uint64_t segment_end = item_end < measure_end ?
                item_end : measure_end;
            const bool has_previous_segment = segment_start > item_start;
            const bool has_next_segment = segment_end < item_end;
            uint16_t event_index = score->event_count++;
            music_event_t *event = &score->events[event_index];
            event->kind = MUSIC_EVENT_NOTE;
            event->part_index = 0;
            event->measure_index = measure_index;
            event->onset_divisions =
                (int32_t)(segment_start - measure_start);
            music_note_t *note = &event->data.note;
            note->pitch.step = item->spelling.step;
            note->pitch.alter = item->spelling.alter;
            note->pitch.octave = item->spelling.octave;
            const uint32_t segment_duration =
                (uint32_t)(segment_end - segment_start);
            note->duration_divisions = (int32_t)segment_duration;
            note->type = duration_kind(segment_duration,
                                       midi->ticks_per_quarter, &note->dots);
            note->accidental = has_previous_segment ?
                MUSIC_ACCIDENTAL_NONE : item->accidental;
            note->stem = item->stem;
            note->voice = item->voice;
            note->staff = item->staff;
            note->tie_flags = (uint8_t)(
                (has_previous_segment ? MIDI_NOTE_TIE_STOP :
                 (item->tie_flags & MIDI_NOTE_TIE_STOP)) |
                (has_next_segment ? MIDI_NOTE_TIE_START :
                 (item->tie_flags & MIDI_NOTE_TIE_START)));
            /* Connection endpoints belong to the source onset.  Internal
             * barline continuation segments only carry the generated tie. */
            note->slur_start = has_previous_segment ? 0 : item->slur_start;
            note->slur_stop = has_previous_segment ? 0 : item->slur_stop;
            note->gliss_start = has_previous_segment ? 0 : item->gliss_start;
            note->gliss_stop = has_previous_segment ? 0 : item->gliss_stop;
            note->chord = item->chord;
            measure->event_count++;
            if (midi_to_event && !has_previous_segment)
                for (size_t source_position = item->source_begin;
                     source_position < item->source_end; ++source_position)
                    midi_to_event[order[source_position]] = event_index;
        }
    }

    for (uint16_t i = 0; i < measure_count; ++i) {
        music_measure_t *measure = &score->measures[i];
        auto_beam_measure(score, measure, midi->ticks_per_quarter,
                          numerator, denominator, staff_count);
    }
    if (result) {
        result->applied_fifths = selected.key_fifths;
        result->applied_minor = selected.key_minor;
        result->score_event_count = score->event_count;
        result->mapped_note_count = (uint16_t)midi->note_count;
        result->deduplicated_note_count = deduplicated;
        result->key_analysis = analysis;
    }
    free(items);
    free(order);
    return MIDI_NOTATION_OK;
}

const char *midi_notation_status_name(midi_notation_status_t status)
{
    switch (status) {
    case MIDI_NOTATION_OK: return "ok";
    case MIDI_NOTATION_INVALID_ARGUMENT: return "invalid_argument";
    case MIDI_NOTATION_EMPTY: return "empty";
    case MIDI_NOTATION_CAPACITY: return "capacity";
    case MIDI_NOTATION_OUT_OF_MEMORY: return "out_of_memory";
    default: return "unknown";
    }
}
