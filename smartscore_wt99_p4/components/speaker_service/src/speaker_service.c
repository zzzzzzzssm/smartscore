#include "speaker_service.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "board_sdcard.h"
#include "board_wt99_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "wav_stream.h"

#define SPEAKER_QUEUE_LENGTH 10
#define SPEAKER_CHUNK_SAMPLES 512
#define SPEAKER_TASK_STACK_BYTES 5120
#define SPEAKER_TASK_PRIORITY 12
#define SPEAKER_OUTPUT_DRAIN_MS 30
#define SPEAKER_STREAM_OUTPUT_DRAIN_MS 80
#define SPEAKER_STREAM_BUFFER_BYTES (256U * 1024U)
#define SPEAKER_STREAM_READ_WAIT_MS 10U
/* Qwen normally delivers one 19.2 KiB PCM delta at a time.  Release playback
 * on the first delta instead of imposing the old four-second fixed delay.
 * If the upstream rate later falls below 48 kB/s, adapt only the rebuffer
 * threshold for this stream: 64 KiB after the first underrun and 96 KiB after
 * repeated underruns.  Producers can always keep filling the 256 KiB PSRAM
 * ring while I2S consumption is paused. */
#define SPEAKER_STREAM_PREBUFFER_BYTES (48U * 1024U)
#define SPEAKER_STREAM_FIRST_REBUFFER_BYTES (48U * 1024U)
#define SPEAKER_STREAM_REPEAT_REBUFFER_BYTES (64U * 1024U)
#define SPEAKER_STREAM_LOW_WATER_TICKS 2U
#define SPEAKER_STREAM_BACKPRESSURE_LOG_INTERVAL 50U
#define METRONOME_QUEUE_LENGTH 6
#define METRONOME_TASK_STACK_BYTES 3072
#define METRONOME_TASK_PRIORITY 7
#define METRONOME_WARMUP_MS 80
#define METRONOME_ACCENT_FREQUENCY_HZ 1400.0f
#define METRONOME_CLICK_FREQUENCY_HZ 900.0f
#define METRONOME_ACCENT_DURATION_MS 90
#define METRONOME_CLICK_DURATION_MS 70
#define METRONOME_ACCENT_GAIN 0.18f
#define METRONOME_CLICK_GAIN 0.12f

typedef enum {
    SPEAKER_COMMAND_TONE = 0,
    SPEAKER_COMMAND_STOP,
    SPEAKER_COMMAND_VOLUME,
    SPEAKER_COMMAND_MUTE,
    SPEAKER_COMMAND_METRONOME_BEGIN,
    SPEAKER_COMMAND_METRONOME_CLICK,
    SPEAKER_COMMAND_METRONOME_END,
    SPEAKER_COMMAND_FILE_PLAY,
    SPEAKER_COMMAND_FILE_PAUSE,
    SPEAKER_COMMAND_STREAM_START,
    SPEAKER_COMMAND_STREAM_RELEASE,
    SPEAKER_COMMAND_STREAM_FINISH,
    SPEAKER_COMMAND_STREAM_ABORT,
} speaker_command_type_t;

typedef struct {
    speaker_command_type_t type;
    union {
        struct {
            float frequency_hz;
            uint32_t duration_ms;
            float gain;
        } tone;
        struct {
            uint8_t beat;
            float frequency_hz;
            uint32_t duration_ms;
            float gain;
        } click;
        char file_name[SPEAKER_FILE_NAME_MAX];
        struct {
            uint32_t sample_rate_hz;
            bool held;
        } stream;
        uint8_t volume;
        bool muted;
    } data;
} speaker_command_t;

typedef enum {
    METRONOME_COMMAND_START = 0,
    METRONOME_COMMAND_PAUSE,
    METRONOME_COMMAND_STOP,
} metronome_command_type_t;

typedef struct {
    metronome_command_type_t type;
    uint32_t generation;
    uint16_t bpm;
    uint8_t beats;
    uint8_t unit;
} metronome_command_t;

static const char *TAG = "SPEAKER";
static QueueHandle_t s_queue;
static QueueHandle_t s_metronome_queue;
static StreamBufferHandle_t s_stream_buffer;
static TaskHandle_t s_task;
static TaskHandle_t s_metronome_task;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_metronome_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_stream_lock = portMUX_INITIALIZER_UNLOCKED;
static speaker_status_t s_status;
static speaker_metronome_status_t s_metronome_status;
static uint32_t s_metronome_generation;
static bool s_control_only_ready;
static esp_timer_handle_t s_control_metronome_timer;
static esp_timer_handle_t s_control_tone_timer;
static wav_stream_file_t s_file;
static bool s_stream_finish_requested;
static bool s_stream_held;
static bool s_stream_output_started;
static bool s_stream_resume_file;
static bool s_stream_rebuffering;
static uint32_t s_stream_low_water_ticks;
static uint32_t s_stream_underruns;
static size_t s_stream_max_buffered_bytes;
static uint64_t s_stream_received_bytes;
static uint64_t s_stream_played_bytes;
static uint32_t s_stream_backpressure_events;
/* Static double buffer: 2 KiB total, never allocated on the audio task stack. */
static int16_t s_pcm[2][SPEAKER_CHUNK_SAMPLES];

static size_t stream_rebuffer_threshold_bytes(void)
{
    return s_stream_underruns <= 1U
               ? SPEAKER_STREAM_FIRST_REBUFFER_BYTES
               : SPEAKER_STREAM_REPEAT_REBUFFER_BYTES;
}

static void initialize_status_defaults(bool hardware_output_enabled)
{
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_metronome_status, 0, sizeof(s_metronome_status));
    s_status.state = SPEAKER_STATE_STOPPED;
    s_status.hardware_output_enabled = hardware_output_enabled;
    s_status.hardware.volume_percent =
        BOARD_WT99_AUDIO_INITIAL_VOLUME_PERCENT;
    s_status.last_error = ESP_OK;
    s_metronome_status.bpm = 90;
    s_metronome_status.beats_per_measure = 4;
    s_metronome_status.beat_unit = 4;
}

static void control_tone_timeout_cb(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_status_lock);
    if (s_status.state == SPEAKER_STATE_TONE) {
        s_status.state = SPEAKER_STATE_STOPPED;
        s_status.frequency_hz = 0.0f;
        s_status.duration_ms = 0;
        s_status.elapsed_ms = 0;
        s_status.last_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_status_lock);
}

static void control_metronome_tick_cb(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_metronome_lock);
    if (s_metronome_status.state == SPEAKER_METRONOME_RUNNING) {
        uint8_t beats = s_metronome_status.beats_per_measure;
        if (beats == 0) beats = 1;
        s_metronome_status.beat_index =
            (uint8_t)(s_metronome_status.beat_index % beats + 1);
    }
    portEXIT_CRITICAL(&s_metronome_lock);
}

static void stop_control_timer(esp_timer_handle_t timer)
{
    if (timer != NULL) {
        esp_timer_stop(timer);
    }
}

static void control_stop_all(void)
{
    stop_control_timer(s_control_tone_timer);
    stop_control_timer(s_control_metronome_timer);
    portENTER_CRITICAL(&s_metronome_lock);
    s_metronome_status.state = SPEAKER_METRONOME_STOPPED;
    s_metronome_status.beat_index = 0;
    portEXIT_CRITICAL(&s_metronome_lock);
    portENTER_CRITICAL(&s_status_lock);
    s_status.state = SPEAKER_STATE_STOPPED;
    s_status.frequency_hz = 0.0f;
    s_status.duration_ms = 0;
    s_status.elapsed_ms = 0;
    s_status.file_name[0] = '\0';
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t send_speaker_command(const speaker_command_t *command,
                                      bool urgent)
{
    if (s_queue == NULL || command == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t sent = urgent
                          ? xQueueSendToFront(s_queue, command, pdMS_TO_TICKS(50))
                          : xQueueSend(s_queue, command, pdMS_TO_TICKS(50));
    return sent == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void set_last_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_metronome_state(speaker_metronome_state_t state,
                                uint8_t beat)
{
    portENTER_CRITICAL(&s_metronome_lock);
    s_metronome_status.state = state;
    s_metronome_status.beat_index = beat;
    if (s_metronome_task != NULL) {
        s_metronome_status.task_stack_min_words =
            uxTaskGetStackHighWaterMark(s_metronome_task);
    }
    portEXIT_CRITICAL(&s_metronome_lock);
}

static void record_metronome_queue_error(void)
{
    portENTER_CRITICAL(&s_metronome_lock);
    ++s_metronome_status.queue_errors;
    portEXIT_CRITICAL(&s_metronome_lock);
}

static void cancel_metronome_for_new_source(void)
{
    if (s_metronome_queue == NULL) {
        return;
    }
    uint32_t generation;
    portENTER_CRITICAL(&s_metronome_lock);
    generation = ++s_metronome_generation;
    s_metronome_status.state = SPEAKER_METRONOME_STOPPED;
    s_metronome_status.beat_index = 0;
    portEXIT_CRITICAL(&s_metronome_lock);
    /* Remove a START that has not yet been consumed, then wake the task. */
    xQueueReset(s_metronome_queue);
    metronome_command_t command = {
        .type = METRONOME_COMMAND_STOP,
        .generation = generation,
    };
    xQueueSendToFront(s_metronome_queue, &command, 0);
}

static void cancel_running_metronome_for_stream(void)
{
    portENTER_CRITICAL(&s_metronome_lock);
    bool running =
        s_metronome_status.state == SPEAKER_METRONOME_RUNNING;
    portEXIT_CRITICAL(&s_metronome_lock);
    if (running) cancel_metronome_for_new_source();
}

static void stop_output(esp_err_t cause)
{
    uint32_t file_rate = s_file.sample_rate_hz;
    speaker_state_t previous_state;
    portENTER_CRITICAL(&s_status_lock);
    previous_state = s_status.state;
    portEXIT_CRITICAL(&s_status_lock);
    esp_err_t error = board_audio_end_output();
    if (s_file.file != NULL) {
        wav_stream_close(&s_file);
    }
    if (file_rate != 0 &&
        file_rate != BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ) {
        esp_err_t rate_error = board_audio_set_sample_rate(
            BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ);
        if (error == ESP_OK) {
            error = rate_error;
        }
    }
    if (previous_state == SPEAKER_STATE_STREAM) {
        esp_err_t rate_error = board_audio_set_sample_rate(
            BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ);
        if (error == ESP_OK) error = rate_error;
        s_stream_finish_requested = false;
        s_stream_held = false;
        s_stream_output_started = false;
        s_stream_rebuffering = false;
        s_stream_resume_file = false;
        s_stream_low_water_ticks = 0;
    }
    if (cause != ESP_OK) {
        error = cause;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.state = error == ESP_OK ? SPEAKER_STATE_STOPPED
                                    : SPEAKER_STATE_ERROR;
    s_status.frequency_hz = 0.0f;
    s_status.duration_ms = 0;
    s_status.elapsed_ms = 0;
    s_status.file_name[0] = '\0';
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_status_lock);
}

static void generate_sine(int16_t *output,
                          float *phase,
                          float frequency_hz,
                          float gain,
                          uint32_t sample_rate_hz,
                          size_t sample_count)
{
    const float two_pi = 6.28318530718f;
    const float step = two_pi * frequency_hz / (float)sample_rate_hz;
    const float amplitude = gain * 32767.0f;
    for (size_t index = 0; index < sample_count; ++index) {
        output[index] = (int16_t)(sinf(*phase) * amplitude);
        *phase += step;
        if (*phase >= two_pi) {
            *phase -= two_pi;
        }
    }
}

static esp_err_t write_silence_ms(uint32_t duration_ms)
{
    uint64_t remaining =
        (uint64_t)duration_ms * BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ /
        1000ULL;
    memset(s_pcm[0], 0, sizeof(s_pcm[0]));
    while (remaining > 0) {
        size_t count = remaining > SPEAKER_CHUNK_SAMPLES
                           ? SPEAKER_CHUNK_SAMPLES
                           : (size_t)remaining;
        esp_err_t error = board_audio_write(s_pcm[0], count);
        if (error != ESP_OK) {
            return error;
        }
        remaining -= count;
    }
    return ESP_OK;
}

static esp_err_t write_metronome_click(const speaker_command_t *command)
{
    float phase = 0.0f;
    uint64_t remaining =
        (uint64_t)command->data.click.duration_ms *
        BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ / 1000ULL;
    unsigned buffer_index = 0;
    while (remaining > 0) {
        size_t count = remaining > SPEAKER_CHUNK_SAMPLES
                           ? SPEAKER_CHUNK_SAMPLES
                           : (size_t)remaining;
        generate_sine(s_pcm[buffer_index],
                      &phase,
                      command->data.click.frequency_hz,
                      command->data.click.gain,
                      BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ,
                      count);
        esp_err_t error = board_audio_write(s_pcm[buffer_index], count);
        if (error != ESP_OK) {
            return error;
        }
        buffer_index ^= 1U;
        remaining -= count;
    }
    return ESP_OK;
}

static void handle_file_pause(void)
{
    speaker_state_t state;
    portENTER_CRITICAL(&s_status_lock);
    state = s_status.state;
    portEXIT_CRITICAL(&s_status_lock);

    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (state == SPEAKER_STATE_FILE) {
        error = board_audio_end_output();
        if (error == ESP_OK) {
            portENTER_CRITICAL(&s_status_lock);
            s_status.state = SPEAKER_STATE_FILE_PAUSED;
            portEXIT_CRITICAL(&s_status_lock);
        }
    } else if (state == SPEAKER_STATE_FILE_PAUSED) {
        error = board_audio_begin_output();
        if (error == ESP_OK) {
            portENTER_CRITICAL(&s_status_lock);
            s_status.state = SPEAKER_STATE_FILE;
            portEXIT_CRITICAL(&s_status_lock);
        }
    }
    set_last_error(error);
}

static void handle_metronome_begin(void)
{
    stop_output(ESP_OK);
    esp_err_t error = board_audio_set_sample_rate(
        BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ);
    if (error == ESP_OK) {
        error = board_audio_begin_output();
    }
    if (error == ESP_OK) {
        error = write_silence_ms(METRONOME_WARMUP_MS);
    }
    if (error != ESP_OK) {
        stop_output(error);
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.state = SPEAKER_STATE_METRONOME;
    s_status.frequency_hz = 0.0f;
    s_status.duration_ms = 0;
    s_status.elapsed_ms = 0;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "metronome output session started; PA remains enabled");
}

static void handle_metronome_click(const speaker_command_t *command)
{
    speaker_state_t state;
    portENTER_CRITICAL(&s_status_lock);
    state = s_status.state;
    portEXIT_CRITICAL(&s_status_lock);
    if (state != SPEAKER_STATE_METRONOME) {
        return;
    }

    esp_err_t error = write_metronome_click(command);
    if (error != ESP_OK) {
        stop_output(error);
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status.frequency_hz = command->data.click.frequency_hz;
    s_status.elapsed_ms += command->data.click.duration_ms;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
}

static void handle_file_play(const char *file_name,
                             uint64_t *played_samples,
                             unsigned *buffer_index)
{
    stop_output(ESP_OK);
    *played_samples = 0;
    *buffer_index = 0;

    esp_err_t error = wav_stream_open(file_name, &s_file);
    if (error == ESP_OK) {
        error = board_audio_set_sample_rate(s_file.sample_rate_hz);
    }
    if (error == ESP_OK) {
        error = board_audio_begin_output();
    }
    if (error != ESP_OK) {
        if (s_file.file != NULL) {
            wav_stream_close(&s_file);
        }
        stop_output(error);
        ESP_LOGW(TAG, "WAV rejected: %s (%s)", file_name,
                 esp_err_to_name(error));
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_status.state = SPEAKER_STATE_FILE;
    s_status.frequency_hz = 0.0f;
    s_status.duration_ms = (uint32_t)((uint64_t)s_file.data_bytes * 1000ULL /
                           (2ULL * s_file.sample_rate_hz));
    s_status.elapsed_ms = 0;
    strlcpy(s_status.file_name, s_file.name, sizeof(s_status.file_name));
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "WAV stream started: %s, %" PRIu32 " Hz",
             s_file.name, s_file.sample_rate_hz);
}

static void handle_tone(const speaker_command_t *command,
                        float *frequency_hz,
                        uint32_t *duration_ms,
                        float *gain,
                        float *phase,
                        uint64_t *generated_samples)
{
    stop_output(ESP_OK);
    *frequency_hz = command->data.tone.frequency_hz;
    *duration_ms = command->data.tone.duration_ms;
    *gain = command->data.tone.gain;
    *phase = 0.0f;
    *generated_samples = 0;

    esp_err_t error = board_audio_set_sample_rate(
        BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ);
    if (error == ESP_OK) {
        error = board_audio_begin_output();
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status.state = error == ESP_OK ? SPEAKER_STATE_TONE
                                    : SPEAKER_STATE_ERROR;
    s_status.frequency_hz = *frequency_hz;
    s_status.duration_ms = *duration_ms;
    s_status.elapsed_ms = 0;
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_status_lock);
    if (error != ESP_OK) {
        stop_output(error);
    } else {
        ESP_LOGI(TAG, "tone started: %.2f Hz, %" PRIu32 " ms",
                 *frequency_hz, *duration_ms);
    }
}

static void handle_stream_start(uint32_t sample_rate_hz, bool held)
{
    if (s_stream_buffer != NULL) xStreamBufferReset(s_stream_buffer);
    portENTER_CRITICAL(&s_stream_lock);
    s_stream_max_buffered_bytes = 0;
    s_stream_received_bytes = 0;
    s_stream_played_bytes = 0;
    s_stream_backpressure_events = 0;
    s_stream_underruns = 0;
    portEXIT_CRITICAL(&s_stream_lock);
    speaker_state_t previous_state;
    portENTER_CRITICAL(&s_status_lock);
    previous_state = s_status.state;
    portEXIT_CRITICAL(&s_status_lock);
    s_stream_resume_file = previous_state == SPEAKER_STATE_FILE_PAUSED &&
                           s_file.file != NULL;
    if (!s_stream_resume_file) {
        stop_output(ESP_OK);
    }
    esp_err_t error = board_audio_set_sample_rate(sample_rate_hz);
    portENTER_CRITICAL(&s_status_lock);
    s_status.state = error == ESP_OK ? SPEAKER_STATE_STREAM
                                    : SPEAKER_STATE_ERROR;
    s_status.frequency_hz = 0.0f;
    s_status.duration_ms = 0;
    s_status.elapsed_ms = 0;
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_status_lock);
    s_stream_finish_requested = false;
    s_stream_held = held;
    s_stream_output_started = false;
    s_stream_rebuffering = false;
    s_stream_low_water_ticks = 0;
    if (error != ESP_OK) {
        stop_output(error);
    } else {
        ESP_LOGI(TAG,
                 "PCM stream prepared: %" PRIu32
                 " Hz, ring=%u prebuffer=%u rebuffer=%u/%u bytes, held=%s resume-file=%s",
                 sample_rate_hz, (unsigned)SPEAKER_STREAM_BUFFER_BYTES,
                 (unsigned)SPEAKER_STREAM_PREBUFFER_BYTES,
                 (unsigned)SPEAKER_STREAM_FIRST_REBUFFER_BYTES,
                 (unsigned)SPEAKER_STREAM_REPEAT_REBUFFER_BYTES,
                 held ? "yes" : "no",
                 s_stream_resume_file ? "yes" : "no");
    }
}

static void finish_stream_output(esp_err_t cause)
{
    esp_err_t error = ESP_OK;
    if (s_stream_output_started) error = board_audio_end_output();
    s_stream_output_started = false;
    s_stream_finish_requested = false;
    s_stream_held = false;
    s_stream_rebuffering = false;
    s_stream_low_water_ticks = 0;

    portENTER_CRITICAL(&s_stream_lock);
    const size_t max_buffered = s_stream_max_buffered_bytes;
    const uint64_t received_bytes = s_stream_received_bytes;
    const uint64_t played_bytes = s_stream_played_bytes;
    const uint32_t backpressure_events =
        s_stream_backpressure_events;
    portEXIT_CRITICAL(&s_stream_lock);
    ESP_LOGI(TAG,
             "PCM stream summary: ring=%u buffered=%u max=%u underruns=%" PRIu32
             " backpressure=%" PRIu32 " received=%" PRIu64
             " played=%" PRIu64 " bytes",
             (unsigned)SPEAKER_STREAM_BUFFER_BYTES,
             s_stream_buffer != NULL
                 ? (unsigned)xStreamBufferBytesAvailable(s_stream_buffer)
                 : 0U,
             (unsigned)max_buffered, s_stream_underruns,
             backpressure_events, received_bytes, played_bytes);

    if (cause == ESP_OK && s_stream_resume_file && s_file.file != NULL) {
        esp_err_t rate_error = board_audio_set_sample_rate(
            s_file.sample_rate_hz);
        if (error == ESP_OK) error = rate_error;
        if (error != ESP_OK) {
            s_stream_resume_file = false;
            stop_output(error);
            return;
        }
        portENTER_CRITICAL(&s_status_lock);
        s_status.state = SPEAKER_STATE_FILE_PAUSED;
        s_status.frequency_hz = 0.0f;
        s_status.duration_ms = 0;
        s_status.last_error = error;
        portEXIT_CRITICAL(&s_status_lock);
        s_stream_resume_file = false;
        ESP_LOGI(TAG, "PCM stream released; prior file remains paused");
        return;
    }
    s_stream_resume_file = false;
    stop_output(cause != ESP_OK ? cause : error);
}

static void speaker_task(void *context)
{
    (void)context;
    speaker_command_t command;
    float phase = 0.0f;
    float frequency_hz = 0.0f;
    float gain = 0.0f;
    uint32_t duration_ms = 0;
    uint64_t generated_samples = 0;
    uint64_t file_samples = 0;
    unsigned buffer_index = 0;

    while (true) {
        speaker_state_t state;
        portENTER_CRITICAL(&s_status_lock);
        state = s_status.state;
        portEXIT_CRITICAL(&s_status_lock);

        TickType_t wait = (state == SPEAKER_STATE_TONE ||
                           state == SPEAKER_STATE_FILE ||
                           state == SPEAKER_STATE_STREAM)
                              ? 0
                              : portMAX_DELAY;
        if (xQueueReceive(s_queue, &command, wait) == pdTRUE) {
            if (command.type == SPEAKER_COMMAND_STOP) {
                if (s_stream_buffer != NULL) xStreamBufferReset(s_stream_buffer);
                stop_output(ESP_OK);
            } else if (command.type == SPEAKER_COMMAND_VOLUME) {
                set_last_error(board_audio_set_volume(command.data.volume));
            } else if (command.type == SPEAKER_COMMAND_MUTE) {
                set_last_error(board_audio_set_mute(command.data.muted));
            } else if (command.type == SPEAKER_COMMAND_FILE_PAUSE) {
                handle_file_pause();
            } else if (command.type == SPEAKER_COMMAND_STREAM_START) {
                handle_stream_start(command.data.stream.sample_rate_hz,
                                    command.data.stream.held);
            } else if (command.type == SPEAKER_COMMAND_STREAM_RELEASE) {
                portENTER_CRITICAL(&s_stream_lock);
                s_stream_held = false;
                portEXIT_CRITICAL(&s_stream_lock);
                ESP_LOGI(TAG, "held PCM stream released");
            } else if (command.type == SPEAKER_COMMAND_STREAM_FINISH) {
                portENTER_CRITICAL(&s_status_lock);
                bool active = s_status.state == SPEAKER_STATE_STREAM;
                portEXIT_CRITICAL(&s_status_lock);
                if (active) s_stream_finish_requested = true;
            } else if (command.type == SPEAKER_COMMAND_STREAM_ABORT) {
                portENTER_CRITICAL(&s_status_lock);
                bool active = s_status.state == SPEAKER_STATE_STREAM;
                portEXIT_CRITICAL(&s_status_lock);
                if (active) {
                    if (s_stream_buffer != NULL) {
                        xStreamBufferReset(s_stream_buffer);
                    }
                    finish_stream_output(ESP_OK);
                }
            } else if (command.type == SPEAKER_COMMAND_METRONOME_BEGIN) {
                handle_metronome_begin();
            } else if (command.type == SPEAKER_COMMAND_METRONOME_CLICK) {
                handle_metronome_click(&command);
            } else if (command.type == SPEAKER_COMMAND_METRONOME_END) {
                portENTER_CRITICAL(&s_status_lock);
                bool active = s_status.state == SPEAKER_STATE_METRONOME;
                portEXIT_CRITICAL(&s_status_lock);
                if (active) {
                    stop_output(ESP_OK);
                }
            } else if (command.type == SPEAKER_COMMAND_FILE_PLAY) {
                handle_file_play(command.data.file_name,
                                 &file_samples,
                                 &buffer_index);
            } else if (command.type == SPEAKER_COMMAND_TONE) {
                handle_tone(&command,
                            &frequency_hz,
                            &duration_ms,
                            &gain,
                            &phase,
                            &generated_samples);
            }
            continue;
        }

        portENTER_CRITICAL(&s_status_lock);
        state = s_status.state;
        portEXIT_CRITICAL(&s_status_lock);

        if (state == SPEAKER_STATE_FILE) {
            size_t bytes = 0;
            esp_err_t error = wav_stream_read(&s_file,
                                              s_pcm[buffer_index],
                                              sizeof(s_pcm[buffer_index]),
                                              &bytes);
            if (error != ESP_OK || bytes == 0) {
                stop_output(error);
                continue;
            }
            error = board_audio_write(s_pcm[buffer_index],
                                      bytes / sizeof(int16_t));
            buffer_index ^= 1U;
            if (error != ESP_OK) {
                stop_output(error);
                continue;
            }
            file_samples += bytes / sizeof(int16_t);
            portENTER_CRITICAL(&s_status_lock);
            s_status.elapsed_ms = (uint32_t)(file_samples * 1000ULL /
                                  s_file.sample_rate_hz);
            s_status.task_stack_min_words =
                uxTaskGetStackHighWaterMark(NULL);
            portEXIT_CRITICAL(&s_status_lock);
            continue;
        }

        if (state == SPEAKER_STATE_STREAM) {
            portENTER_CRITICAL(&s_stream_lock);
            const bool stream_held = s_stream_held;
            portEXIT_CRITICAL(&s_stream_lock);
            if (stream_held) {
                /* Holding only pauses the consumer. Producers remain fully
                 * non-blocking and can continue filling the one PSRAM ring. */
                vTaskDelay(1);
                continue;
            }
            size_t available =
                xStreamBufferBytesAvailable(s_stream_buffer);
            if (!s_stream_output_started) {
                const size_t threshold = s_stream_rebuffering
                                             ? stream_rebuffer_threshold_bytes()
                                             : SPEAKER_STREAM_PREBUFFER_BYTES;
                if (available < threshold &&
                    !(s_stream_finish_requested && available > 0U)) {
                    if (s_stream_finish_requested && available == 0U) {
                        finish_stream_output(ESP_OK);
                        ESP_LOGI(TAG, "empty PCM stream finished");
                    } else {
                        /* P4 uses a 10 ms RTOS tick.  pdMS_TO_TICKS(2)
                         * becomes zero and leaves this high-priority task
                         * continuously runnable, starving both the voice
                         * staging worker and IDLE0 until the watchdog fires. */
                        vTaskDelay(1);
                    }
                    continue;
                }
                esp_err_t start_error = board_audio_begin_output();
                if (start_error != ESP_OK) {
                    finish_stream_output(start_error);
                    continue;
                }
                const bool recovered_from_underrun =
                    s_stream_rebuffering;
                s_stream_output_started = true;
                s_stream_rebuffering = false;
                s_stream_low_water_ticks = 0;
                portENTER_CRITICAL(&s_stream_lock);
                const size_t max_buffered = s_stream_max_buffered_bytes;
                portEXIT_CRITICAL(&s_stream_lock);
                ESP_LOGI(TAG,
                         "PCM playback released: ring=%u buffered=%u max=%u bytes underruns=%" PRIu32,
                         (unsigned)SPEAKER_STREAM_BUFFER_BYTES,
                         (unsigned)available,
                         (unsigned)max_buffered,
                         s_stream_underruns);
                if (recovered_from_underrun) {
                    ESP_LOGI(TAG,
                             "PCM rebuffer recovered automatically: buffered=%u threshold=%u bytes underruns=%" PRIu32,
                             (unsigned)available,
                             (unsigned)threshold,
                             s_stream_underruns);
                }
            }

            available = xStreamBufferBytesAvailable(s_stream_buffer);
            size_t read_bytes = sizeof(s_pcm[buffer_index]);
            if (available < read_bytes) {
                if (s_stream_finish_requested) {
                    read_bytes = available;
                } else {
                    if (++s_stream_low_water_ticks >=
                        SPEAKER_STREAM_LOW_WATER_TICKS) {
                        esp_err_t pause_error = board_audio_end_output();
                        if (pause_error != ESP_OK) {
                            finish_stream_output(pause_error);
                            continue;
                        }
                        s_stream_output_started = false;
                        s_stream_rebuffering = true;
                        s_stream_low_water_ticks = 0;
                        portENTER_CRITICAL(&s_stream_lock);
                        ++s_stream_underruns;
                        const uint32_t underruns = s_stream_underruns;
                        const size_t max_buffered =
                            s_stream_max_buffered_bytes;
                        const size_t rebuffer_threshold =
                            stream_rebuffer_threshold_bytes();
                        portEXIT_CRITICAL(&s_stream_lock);
                        ESP_LOGW(TAG,
                                 "PCM underrun=%" PRIu32
                                 "; buffered=%u max=%u, rebuffering to %u bytes",
                                 underruns,
                                 (unsigned)available,
                                 (unsigned)max_buffered,
                                 (unsigned)rebuffer_threshold);
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(
                            SPEAKER_STREAM_READ_WAIT_MS));
                    }
                    continue;
                }
            } else {
                s_stream_low_water_ticks = 0;
            }
            size_t bytes = read_bytes > 0U
                               ? xStreamBufferReceive(
                                     s_stream_buffer,
                                     s_pcm[buffer_index], read_bytes, 0)
                               : 0U;
            if (bytes > 0U) {
                esp_err_t error = board_audio_write(
                    s_pcm[buffer_index], bytes / sizeof(int16_t));
                buffer_index ^= 1U;
                if (error != ESP_OK) {
                    stop_output(error);
                } else {
                    portENTER_CRITICAL(&s_stream_lock);
                    s_stream_played_bytes += bytes;
                    portEXIT_CRITICAL(&s_stream_lock);
                }
                continue;
            }
            if (s_stream_finish_requested) {
                /* The I2S DMA queue can still own roughly 64 ms of 24 kHz
                 * mono PCM after the software ring reaches zero. */
                vTaskDelay(pdMS_TO_TICKS(
                    SPEAKER_STREAM_OUTPUT_DRAIN_MS));
                finish_stream_output(ESP_OK);
                ESP_LOGI(TAG, "PCM stream playback finished");
            }
            continue;
        }

        if (state != SPEAKER_STATE_TONE) {
            continue;
        }
        uint64_t total_samples =
            (uint64_t)duration_ms *
            BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ / 1000ULL;
        size_t sample_count = SPEAKER_CHUNK_SAMPLES;
        if (duration_ms != 0 &&
            generated_samples + sample_count > total_samples) {
            sample_count = (size_t)(total_samples - generated_samples);
        }
        if (sample_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(SPEAKER_OUTPUT_DRAIN_MS));
            stop_output(ESP_OK);
            continue;
        }

        generate_sine(s_pcm[buffer_index],
                      &phase,
                      frequency_hz,
                      gain,
                      BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ,
                      sample_count);
        esp_err_t error = board_audio_write(s_pcm[buffer_index], sample_count);
        buffer_index ^= 1U;
        if (error != ESP_OK) {
            stop_output(error);
            continue;
        }
        generated_samples += sample_count;
        portENTER_CRITICAL(&s_status_lock);
        s_status.elapsed_ms = (uint32_t)(generated_samples * 1000ULL /
                              BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ);
        s_status.task_stack_min_words = uxTaskGetStackHighWaterMark(NULL);
        portEXIT_CRITICAL(&s_status_lock);
    }
}

static esp_err_t queue_metronome_click(uint8_t beat)
{
    speaker_command_t command = {
        .type = SPEAKER_COMMAND_METRONOME_CLICK,
        .data.click = {
            .beat = beat,
            .frequency_hz = beat == 1
                                ? METRONOME_ACCENT_FREQUENCY_HZ
                                : METRONOME_CLICK_FREQUENCY_HZ,
            .duration_ms = beat == 1
                               ? METRONOME_ACCENT_DURATION_MS
                               : METRONOME_CLICK_DURATION_MS,
            .gain = beat == 1 ? METRONOME_ACCENT_GAIN
                              : METRONOME_CLICK_GAIN,
        },
    };
    esp_err_t error = send_speaker_command(&command, false);
    if (error != ESP_OK) {
        record_metronome_queue_error();
    }
    return error;
}

static void metronome_task(void *context)
{
    (void)context;
    metronome_command_t command;
    while (true) {
        TickType_t wait = portMAX_DELAY;
        portENTER_CRITICAL(&s_metronome_lock);
        if (s_metronome_status.state == SPEAKER_METRONOME_RUNNING) {
            wait = pdMS_TO_TICKS(60000U / s_metronome_status.bpm);
        }
        portEXIT_CRITICAL(&s_metronome_lock);

        if (xQueueReceive(s_metronome_queue, &command, wait) == pdTRUE) {
            if (command.type == METRONOME_COMMAND_START) {
                bool current;
                portENTER_CRITICAL(&s_metronome_lock);
                current = command.generation == s_metronome_generation;
                if (current) {
                    s_metronome_status.bpm = command.bpm;
                    s_metronome_status.beats_per_measure = command.beats;
                    s_metronome_status.beat_unit = command.unit;
                    s_metronome_status.state = SPEAKER_METRONOME_RUNNING;
                    s_metronome_status.beat_index = 1;
                }
                portEXIT_CRITICAL(&s_metronome_lock);
                if (!current) {
                    continue;
                }
                esp_err_t error = queue_metronome_click(1);
                if (error != ESP_OK) {
                    set_metronome_state(SPEAKER_METRONOME_STOPPED, 0);
                } else {
                    ESP_LOGI(TAG, "metronome started: %u BPM, %u/%u",
                             command.bpm, command.beats, command.unit);
                }
            } else if (command.type == METRONOME_COMMAND_PAUSE) {
                uint8_t beat;
                bool current;
                portENTER_CRITICAL(&s_metronome_lock);
                current = command.generation == s_metronome_generation;
                beat = s_metronome_status.beat_index;
                portEXIT_CRITICAL(&s_metronome_lock);
                if (!current) {
                    continue;
                }
                set_metronome_state(SPEAKER_METRONOME_PAUSED, beat);
                speaker_command_t end = {
                    .type = SPEAKER_COMMAND_METRONOME_END,
                };
                send_speaker_command(&end, true);
            } else {
                portENTER_CRITICAL(&s_metronome_lock);
                bool current =
                    command.generation == s_metronome_generation;
                portEXIT_CRITICAL(&s_metronome_lock);
                if (!current) {
                    continue;
                }
                set_metronome_state(SPEAKER_METRONOME_STOPPED, 0);
                speaker_command_t end = {
                    .type = SPEAKER_COMMAND_METRONOME_END,
                };
                send_speaker_command(&end, true);
            }
            continue;
        }

        uint8_t beat;
        bool running;
        portENTER_CRITICAL(&s_metronome_lock);
        running = s_metronome_status.state == SPEAKER_METRONOME_RUNNING;
        beat = s_metronome_status.beat_index;
        if (running) {
            beat = (uint8_t)(beat %
                             s_metronome_status.beats_per_measure + 1);
            s_metronome_status.beat_index = beat;
        }
        portEXIT_CRITICAL(&s_metronome_lock);
        if (running) {
            queue_metronome_click(beat);
        }
    }
}

esp_err_t speaker_service_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    initialize_status_defaults(true);

    esp_err_t error = board_audio_init();
    if (error != ESP_OK) {
        s_status.state = SPEAKER_STATE_ERROR;
        s_status.last_error = error;
        return error;
    }

    s_queue = xQueueCreate(SPEAKER_QUEUE_LENGTH, sizeof(speaker_command_t));
    s_metronome_queue = xQueueCreate(METRONOME_QUEUE_LENGTH,
                                     sizeof(metronome_command_t));
    s_stream_buffer = xStreamBufferCreateWithCaps(
        SPEAKER_STREAM_BUFFER_BYTES, 1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue == NULL || s_metronome_queue == NULL) {
        if (s_queue != NULL) {
            vQueueDelete(s_queue);
        }
        if (s_metronome_queue != NULL) {
            vQueueDelete(s_metronome_queue);
        }
        if (s_stream_buffer != NULL) {
            vStreamBufferDeleteWithCaps(s_stream_buffer);
        }
        s_queue = NULL;
        s_metronome_queue = NULL;
        s_stream_buffer = NULL;
        board_audio_deinit();
        return ESP_ERR_NO_MEM;
    }
    if (s_stream_buffer == NULL) {
        ESP_LOGW(TAG,
                 "real-time PCM buffer unavailable; existing speaker sources remain enabled");
    }

    if (xTaskCreate(speaker_task,
                    "speaker",
                    SPEAKER_TASK_STACK_BYTES,
                    NULL,
                    SPEAKER_TASK_PRIORITY,
                    &s_task) != pdPASS ||
        xTaskCreate(metronome_task,
                    "metronome",
                    METRONOME_TASK_STACK_BYTES,
                    NULL,
                    METRONOME_TASK_PRIORITY,
                    &s_metronome_task) != pdPASS) {
        if (s_task != NULL) {
            vTaskDelete(s_task);
            s_task = NULL;
        }
        if (s_metronome_task != NULL) {
            vTaskDelete(s_metronome_task);
            s_metronome_task = NULL;
        }
        vQueueDelete(s_queue);
        vQueueDelete(s_metronome_queue);
        if (s_stream_buffer != NULL) {
            vStreamBufferDeleteWithCaps(s_stream_buffer);
        }
        s_queue = NULL;
        s_metronome_queue = NULL;
        s_stream_buffer = NULL;
        board_audio_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "speaker service ready; mono differential output, initial volume=%u%%, stream ring=%u bytes PSRAM",
             BOARD_WT99_AUDIO_INITIAL_VOLUME_PERCENT,
             (unsigned)SPEAKER_STREAM_BUFFER_BYTES);
    return ESP_OK;
}

esp_err_t speaker_service_init_control_only(void)
{
    if (s_control_only_ready) {
        return ESP_OK;
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    initialize_status_defaults(false);
    const esp_timer_create_args_t metronome_args = {
        .callback = control_metronome_tick_cb,
        .name = "control_metronome",
    };
    esp_err_t error = esp_timer_create(&metronome_args,
                                       &s_control_metronome_timer);
    if (error != ESP_OK) {
        s_status.state = SPEAKER_STATE_ERROR;
        s_status.last_error = error;
        return error;
    }
    const esp_timer_create_args_t tone_args = {
        .callback = control_tone_timeout_cb,
        .name = "control_tone",
    };
    error = esp_timer_create(&tone_args, &s_control_tone_timer);
    if (error != ESP_OK) {
        esp_timer_delete(s_control_metronome_timer);
        s_control_metronome_timer = NULL;
        s_status.state = SPEAKER_STATE_ERROR;
        s_status.last_error = error;
        return error;
    }
    s_control_only_ready = true;
    ESP_LOGI(TAG,
             "control-only audio service ready; hardware output remains disabled");
    return ESP_OK;
}

bool speaker_service_is_ready(void)
{
    return s_control_only_ready ||
           (s_queue != NULL && s_task != NULL &&
            s_metronome_queue != NULL && s_metronome_task != NULL);
}

esp_err_t speaker_service_play_tone(float frequency_hz,
                                    uint32_t duration_ms,
                                    float gain)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frequency_hz < 20.0f || frequency_hz > 8000.0f ||
        duration_ms > 600000 || gain < 0.0f || gain > 0.5f) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_only_ready) {
        control_stop_all();
        portENTER_CRITICAL(&s_status_lock);
        s_status.state = SPEAKER_STATE_TONE;
        s_status.frequency_hz = frequency_hz;
        s_status.duration_ms = duration_ms;
        s_status.elapsed_ms = 0;
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
        if (duration_ms > 0) {
            esp_err_t error = esp_timer_start_once(
                s_control_tone_timer, (uint64_t)duration_ms * 1000ULL);
            if (error != ESP_OK) {
                control_stop_all();
                set_last_error(error);
                return error;
            }
        }
        return ESP_OK;
    }
    cancel_metronome_for_new_source();
    xQueueReset(s_queue);
    speaker_command_t command = {
        .type = SPEAKER_COMMAND_TONE,
        .data.tone = {
            .frequency_hz = frequency_hz,
            .duration_ms = duration_ms,
            .gain = gain,
        },
    };
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_stop(void)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_control_only_ready) {
        control_stop_all();
        return ESP_OK;
    }
    cancel_metronome_for_new_source();
    /* Emergency stop must not leave an older queued PLAY behind it. */
    if (s_stream_buffer != NULL) xStreamBufferReset(s_stream_buffer);
    xQueueReset(s_queue);
    speaker_command_t command = {.type = SPEAKER_COMMAND_STOP};
    return send_speaker_command(&command, true);
}

esp_err_t speaker_service_set_volume(uint8_t percent)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > BOARD_WT99_AUDIO_MAX_VOLUME_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_only_ready) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.hardware.volume_percent = percent;
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_OK;
    }
    speaker_command_t command = {
        .type = SPEAKER_COMMAND_VOLUME,
        .data.volume = percent,
    };
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_set_mute(bool muted)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_control_only_ready) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.hardware.muted = muted;
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_OK;
    }
    speaker_command_t command = {
        .type = SPEAKER_COMMAND_MUTE,
        .data.muted = muted,
    };
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_stream_start(uint32_t sample_rate_hz)
{
    if (!speaker_service_is_ready() || s_control_only_ready ||
        s_stream_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate_hz < 8000U || sample_rate_hz > 48000U) {
        return ESP_ERR_INVALID_ARG;
    }
    cancel_running_metronome_for_stream();
    speaker_command_t command = {
        .type = SPEAKER_COMMAND_STREAM_START,
        .data.stream = {
            .sample_rate_hz = sample_rate_hz,
            .held = false,
        },
    };
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_stream_start_held(uint32_t sample_rate_hz)
{
    if (!speaker_service_is_ready() || s_control_only_ready ||
        s_stream_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate_hz < 8000U || sample_rate_hz > 48000U) {
        return ESP_ERR_INVALID_ARG;
    }
    cancel_running_metronome_for_stream();
    speaker_command_t command = {
        .type = SPEAKER_COMMAND_STREAM_START,
        .data.stream = {
            .sample_rate_hz = sample_rate_hz,
            .held = true,
        },
    };
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_stream_release(void)
{
    if (!speaker_service_is_ready() || s_control_only_ready ||
        s_stream_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    speaker_command_t command = {.type = SPEAKER_COMMAND_STREAM_RELEASE};
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_stream_write(const int16_t *pcm,
                                       size_t sample_count,
                                       size_t *accepted_samples)
{
    if (accepted_samples != NULL) *accepted_samples = 0;
    if (pcm == NULL || sample_count == 0U) return ESP_ERR_INVALID_ARG;
    if (!speaker_service_is_ready() || s_control_only_ready ||
        s_stream_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count > SIZE_MAX / sizeof(*pcm)) return ESP_ERR_INVALID_SIZE;
    const size_t bytes = sample_count * sizeof(*pcm);
    /* Keep the byte stream sample-aligned even when FreeRTOS reports an odd
     * final byte of free ring space.  There is one serialized PCM writer. */
    const size_t writable =
        xStreamBufferSpacesAvailable(s_stream_buffer) & ~(size_t)1U;
    const size_t write_bytes = bytes < writable ? bytes : writable;
    size_t sent = write_bytes > 0U
                      ? xStreamBufferSend(
                            s_stream_buffer, pcm, write_bytes, 0)
                      : 0U;
    const size_t buffered = xStreamBufferBytesAvailable(s_stream_buffer);
    portENTER_CRITICAL(&s_stream_lock);
    s_stream_received_bytes += sent;
    if (buffered > s_stream_max_buffered_bytes) {
        s_stream_max_buffered_bytes = buffered;
    }
    const size_t max_buffered = s_stream_max_buffered_bytes;
    if (accepted_samples != NULL) {
        *accepted_samples = sent / sizeof(*pcm);
    }
    if (sent < bytes) {
        ++s_stream_backpressure_events;
        const uint32_t backpressure_events =
            s_stream_backpressure_events;
        portEXIT_CRITICAL(&s_stream_lock);
        if (backpressure_events == 1U ||
            (backpressure_events %
             SPEAKER_STREAM_BACKPRESSURE_LOG_INTERVAL) == 0U) {
            ESP_LOGW(TAG,
                     "PCM ring backpressure=%" PRIu32
                     ": accepted=%u/%u samples buffered=%u max=%u",
                     backpressure_events,
                     (unsigned)(sent / sizeof(*pcm)),
                     (unsigned)sample_count, (unsigned)buffered,
                     (unsigned)max_buffered);
        }
    } else {
        portEXIT_CRITICAL(&s_stream_lock);
    }
    return sent > 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t speaker_service_stream_finish(void)
{
    if (!speaker_service_is_ready() || s_control_only_ready ||
        s_stream_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    speaker_command_t command = {.type = SPEAKER_COMMAND_STREAM_FINISH};
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_stream_abort(void)
{
    if (!speaker_service_is_ready() || s_control_only_ready ||
        s_stream_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Queue behind an already accepted START so START->ABORT is preserved.
     * This prevents a late queued START from resurrecting a canceled stream. */
    speaker_command_t command = {.type = SPEAKER_COMMAND_STREAM_ABORT};
    return send_speaker_command(&command, false);
}

void speaker_service_stream_get_metrics(speaker_stream_metrics_t *out_metrics)
{
    if (out_metrics == NULL) return;
    memset(out_metrics, 0, sizeof(*out_metrics));
    out_metrics->capacity_bytes = SPEAKER_STREAM_BUFFER_BYTES;
    if (s_stream_buffer != NULL) {
        out_metrics->buffered_bytes =
            xStreamBufferBytesAvailable(s_stream_buffer);
    }
    portENTER_CRITICAL(&s_status_lock);
    out_metrics->active = s_status.state == SPEAKER_STATE_STREAM;
    portEXIT_CRITICAL(&s_status_lock);
    portENTER_CRITICAL(&s_stream_lock);
    out_metrics->held = s_stream_held;
    out_metrics->output_started = s_stream_output_started;
    out_metrics->rebuffering = s_stream_rebuffering;
    out_metrics->finish_requested = s_stream_finish_requested;
    out_metrics->max_buffered_bytes = s_stream_max_buffered_bytes;
    out_metrics->received_bytes = s_stream_received_bytes;
    out_metrics->played_bytes = s_stream_played_bytes;
    out_metrics->underruns = s_stream_underruns;
    out_metrics->backpressure_events = s_stream_backpressure_events;
    portEXIT_CRITICAL(&s_stream_lock);
}

static bool valid_meter(uint8_t beats, uint8_t unit)
{
    return (unit == 4 && (beats == 2 || beats == 3 || beats == 4)) ||
           (unit == 8 && beats == 6);
}

esp_err_t speaker_service_metronome_start(uint16_t bpm,
                                          uint8_t beats_per_measure,
                                          uint8_t beat_unit)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bpm < 30 || bpm > 240 ||
        !valid_meter(beats_per_measure, beat_unit)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_only_ready) {
        control_stop_all();
        portENTER_CRITICAL(&s_metronome_lock);
        ++s_metronome_generation;
        s_metronome_status.state = SPEAKER_METRONOME_RUNNING;
        s_metronome_status.bpm = bpm;
        s_metronome_status.beats_per_measure = beats_per_measure;
        s_metronome_status.beat_unit = beat_unit;
        s_metronome_status.beat_index = 1;
        portEXIT_CRITICAL(&s_metronome_lock);
        portENTER_CRITICAL(&s_status_lock);
        s_status.state = SPEAKER_STATE_METRONOME;
        s_status.frequency_hz = 0.0f;
        s_status.duration_ms = 0;
        s_status.elapsed_ms = 0;
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
        esp_err_t error = esp_timer_start_periodic(
            s_control_metronome_timer, 60000000ULL / bpm);
        if (error != ESP_OK) {
            control_stop_all();
            set_last_error(error);
            return error;
        }
        return ESP_OK;
    }
    /* Queue BEGIN directly so all main-source replacements share one order. */
    uint32_t generation;
    portENTER_CRITICAL(&s_metronome_lock);
    generation = ++s_metronome_generation;
    s_metronome_status.state = SPEAKER_METRONOME_STOPPED;
    s_metronome_status.beat_index = 0;
    portEXIT_CRITICAL(&s_metronome_lock);
    xQueueReset(s_metronome_queue);
    xQueueReset(s_queue);
    speaker_command_t begin = {
        .type = SPEAKER_COMMAND_METRONOME_BEGIN,
    };
    esp_err_t error = send_speaker_command(&begin, false);
    if (error != ESP_OK) {
        return error;
    }
    metronome_command_t command = {
        .type = METRONOME_COMMAND_START,
        .generation = generation,
        .bpm = bpm,
        .beats = beats_per_measure,
        .unit = beat_unit,
    };
    if (xQueueSend(s_metronome_queue,
                   &command,
                   pdMS_TO_TICKS(50)) == pdTRUE) {
        return ESP_OK;
    }
    xQueueReset(s_queue);
    speaker_command_t stop = {.type = SPEAKER_COMMAND_STOP};
    send_speaker_command(&stop, true);
    return ESP_ERR_TIMEOUT;
}

esp_err_t speaker_service_metronome_pause(void)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_control_only_ready) {
        portENTER_CRITICAL(&s_metronome_lock);
        speaker_metronome_state_t state = s_metronome_status.state;
        if (state == SPEAKER_METRONOME_RUNNING) {
            s_metronome_status.state = SPEAKER_METRONOME_PAUSED;
        }
        portEXIT_CRITICAL(&s_metronome_lock);
        if (state == SPEAKER_METRONOME_PAUSED) return ESP_OK;
        if (state != SPEAKER_METRONOME_RUNNING) {
            return ESP_ERR_INVALID_STATE;
        }
        stop_control_timer(s_control_metronome_timer);
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_metronome_lock);
    speaker_metronome_state_t state = s_metronome_status.state;
    uint32_t generation = s_metronome_generation;
    portEXIT_CRITICAL(&s_metronome_lock);
    if (state == SPEAKER_METRONOME_PAUSED) {
        return ESP_OK;
    }
    if (state != SPEAKER_METRONOME_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    metronome_command_t command = {
        .type = METRONOME_COMMAND_PAUSE,
        .generation = generation,
    };
    return xQueueSendToFront(s_metronome_queue,
                             &command,
                             pdMS_TO_TICKS(50)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t speaker_service_metronome_stop(void)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_control_only_ready) {
        stop_control_timer(s_control_metronome_timer);
        portENTER_CRITICAL(&s_metronome_lock);
        s_metronome_status.state = SPEAKER_METRONOME_STOPPED;
        s_metronome_status.beat_index = 0;
        portEXIT_CRITICAL(&s_metronome_lock);
        portENTER_CRITICAL(&s_status_lock);
        if (s_status.state == SPEAKER_STATE_METRONOME) {
            s_status.state = SPEAKER_STATE_STOPPED;
            s_status.last_error = ESP_OK;
        }
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_metronome_lock);
    speaker_metronome_state_t state = s_metronome_status.state;
    uint32_t generation = s_metronome_generation;
    if (state != SPEAKER_METRONOME_STOPPED) {
        generation = ++s_metronome_generation;
        s_metronome_status.state = SPEAKER_METRONOME_STOPPED;
        s_metronome_status.beat_index = 0;
    }
    portEXIT_CRITICAL(&s_metronome_lock);
    if (state == SPEAKER_METRONOME_STOPPED) {
        return ESP_OK;
    }
    metronome_command_t command = {
        .type = METRONOME_COMMAND_STOP,
        .generation = generation,
    };
    return xQueueSendToFront(s_metronome_queue,
                             &command,
                             pdMS_TO_TICKS(50)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t speaker_service_list_files(
    char names[][SPEAKER_FILE_NAME_MAX],
    size_t capacity,
    size_t *out_count)
{
    return wav_stream_list(names, capacity, out_count);
}

esp_err_t speaker_service_list_files_page(
    const char *search,
    size_t requested_page,
    size_t page_size,
    char names[][SPEAKER_FILE_NAME_MAX],
    size_t capacity,
    size_t *out_count,
    size_t *out_total,
    size_t *out_page)
{
    return wav_stream_list_page(search, requested_page, page_size, names,
                                capacity, out_count, out_total, out_page);
}

esp_err_t speaker_service_play_file(const char *safe_name)
{
    if (!speaker_service_is_ready() || safe_name == NULL ||
        safe_name[0] == '\0' || strlen(safe_name) >= SPEAKER_FILE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_only_ready) {
        control_stop_all();
        portENTER_CRITICAL(&s_status_lock);
        s_status.state = SPEAKER_STATE_FILE;
        strlcpy(s_status.file_name, safe_name,
                sizeof(s_status.file_name));
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
        return ESP_OK;
    }
    cancel_metronome_for_new_source();
    xQueueReset(s_queue);
    speaker_command_t command = {.type = SPEAKER_COMMAND_FILE_PLAY};
    strlcpy(command.data.file_name,
            safe_name,
            sizeof(command.data.file_name));
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_pause_file(void)
{
    if (!speaker_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_control_only_ready) {
        portENTER_CRITICAL(&s_status_lock);
        speaker_state_t state = s_status.state;
        if (state == SPEAKER_STATE_FILE) {
            s_status.state = SPEAKER_STATE_FILE_PAUSED;
        } else if (state == SPEAKER_STATE_FILE_PAUSED) {
            s_status.state = SPEAKER_STATE_FILE;
        }
        s_status.last_error =
            state == SPEAKER_STATE_FILE ||
            state == SPEAKER_STATE_FILE_PAUSED
                ? ESP_OK
                : ESP_ERR_INVALID_STATE;
        esp_err_t error = s_status.last_error;
        portEXIT_CRITICAL(&s_status_lock);
        return error;
    }
    speaker_command_t command = {.type = SPEAKER_COMMAND_FILE_PAUSE};
    return send_speaker_command(&command, false);
}

esp_err_t speaker_service_stop_file(void)
{
    return speaker_service_stop();
}

void speaker_service_get_status(speaker_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    portENTER_CRITICAL(&s_metronome_lock);
    out_status->metronome = s_metronome_status;
    portEXIT_CRITICAL(&s_metronome_lock);

    if (!s_control_only_ready) {
        board_audio_get_status(&out_status->hardware);
        out_status->hardware_output_enabled =
            out_status->hardware.codec_ready;
    }
    board_sdcard_status_t card;
    board_sdcard_get_status(&card);
    out_status->sd_present = card.mounted;
    out_status->sd_capacity_bytes = card.capacity_bytes;
    out_status->sd_last_error = card.last_error;
}
