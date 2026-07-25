#include "score_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "sdkconfig.h"
#include "score_json_parser.h"

static const char *TAG = "score_storage";

#define SPIFFS_SCORE_DIR "/spiffs"
#define SD_MOUNT_DIR     "/sdcard"
#define SD_SCORE_DIR     "/sdcard/scores"
#define SCORE_EXT       ".json"
#define BUILTIN_SCORE_FILENAME "@builtin_twinkle.json"
#define CREATOR_TITLE_PREFIX "\xE5\xBE\x85\xE5\x91\xBD\xE5\x90\x8D\xE6\x9B\xB2\xE7\x9B\xAE"
#define CREATOR_TEMP_FILENAME "CRSAVE.TMP"
#define RENAME_TEMP_FILENAME "SCRNM.TMP"
#define RENAME_BACKUP_FILENAME "SCRNM.BAK"

/* Always-available single-voice melody. Grand-staff test data belongs in a
 * separately annotated score; low pitches must not manufacture a bass staff. */
static const char s_builtin_twinkle_json[] =
    "{\"title\":\"小星星（内置测试）\",\"bpm\":100,"
    "\"time_signature\":\"4/4\",\"key\":\"C major\",\"notes\":["
    "{\"midi\":60,\"start\":0.0,\"duration\":0.6},"
    "{\"midi\":60,\"start\":0.6,\"duration\":0.6},"
    "{\"midi\":67,\"start\":1.2,\"duration\":0.6},"
    "{\"midi\":67,\"start\":1.8,\"duration\":0.6},"
    "{\"midi\":69,\"start\":2.4,\"duration\":0.6},"
    "{\"midi\":69,\"start\":3.0,\"duration\":0.6},"
    "{\"midi\":67,\"start\":3.6,\"duration\":1.2},"
    "{\"midi\":65,\"start\":4.8,\"duration\":0.6},"
    "{\"midi\":65,\"start\":5.4,\"duration\":0.6},"
    "{\"midi\":64,\"start\":6.0,\"duration\":0.6},"
    "{\"midi\":64,\"start\":6.6,\"duration\":0.6},"
    "{\"midi\":62,\"start\":7.2,\"duration\":0.6},"
    "{\"midi\":62,\"start\":7.8,\"duration\":0.6},"
    "{\"midi\":60,\"start\":8.4,\"duration\":1.2}]}";

static bool s_mounted = false;

esp_err_t score_storage_ensure_mounted(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_SCORE_DIR,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "storage ready at %s (total=%d used=%d)",
             SPIFFS_SCORE_DIR, total, used);
    return ESP_OK;
}

static bool has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    if (nl <= el) return false;
    return strcasecmp(name + nl - el, ext) == 0;
}

static bool valid_filename(const char *filename)
{
    return filename && filename[0] && !strstr(filename, "..") &&
           !strchr(filename, '/') && !strchr(filename, '\\');
}

bool score_storage_is_valid_sd_filename(const char *filename)
{
    return valid_filename(filename) && has_ext(filename, SCORE_EXT);
}

static bool quick_parse_meta(const char *path, score_info_t *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > SCORE_STORAGE_MAX_FILE_BYTES) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) { free(buf); return false; }
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    cJSON *title_json = cJSON_GetObjectItemCaseSensitive(root, "title");
    cJSON *bpm_json   = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *notes_json = cJSON_GetObjectItemCaseSensitive(root, "notes");
    cJSON *key_json = cJSON_GetObjectItemCaseSensitive(root, "key");
    cJSON *time_json = cJSON_GetObjectItemCaseSensitive(root, "time_signature");

    if (cJSON_IsString(title_json)) {
        snprintf(info->title, sizeof(info->title), "%s", title_json->valuestring);
    } else {
        snprintf(info->title, sizeof(info->title), "%s", "untitled");
    }
    info->bpm        = cJSON_IsNumber(bpm_json) ? bpm_json->valueint : 120;
    info->note_count = cJSON_IsArray(notes_json) ? cJSON_GetArraySize(notes_json) : 0;
    info->file_size  = (size_t)fsize;
    snprintf(info->key, sizeof(info->key), "%s",
             cJSON_IsString(key_json) ? key_json->valuestring : "C major");
    snprintf(info->time_signature, sizeof(info->time_signature), "%s",
             cJSON_IsString(time_json) ? time_json->valuestring : "4/4");

    int beats = 4, beat_type = 4;
    sscanf(info->time_signature, "%d/%d", &beats, &beat_type);
    if (beats <= 0) beats = 4;
    if (beat_type <= 0) beat_type = 4;
    double last_end = 0.0;
    cJSON *note = NULL;
    cJSON_ArrayForEach(note, notes_json) {
        cJSON *start = cJSON_GetObjectItemCaseSensitive(note, "start");
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(note, "duration");
        if (!cJSON_IsNumber(start)) continue;
        double end = start->valuedouble +
                     (cJSON_IsNumber(duration) ? duration->valuedouble : 0.0);
        if (end > last_end) last_end = end;
    }
    double measure_seconds = (60.0 / (info->bpm > 0 ? info->bpm : 120)) *
                             beats * 4.0 / beat_type;
    info->measure_count = measure_seconds > 0.0 ?
                          (int)(last_end / measure_seconds + 0.999999) : 0;

    cJSON_Delete(root);
    return true;
}

static bool filename_already_listed(const score_info_t *out, int count,
                                    const char *filename)
{
    for (int i = 0; i < count; ++i)
        if (strcasecmp(out[i].filename, filename) == 0)
            return true;
    return false;
}

static int scan_score_directory(const char *directory, score_info_t *out,
                                int count, int max_count,
                                bool include_invalid)
{
    DIR *dir = opendir(directory);
    if (!dir) return count;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) continue;
        if (!has_ext(entry->d_name, SCORE_EXT)) continue;
        if (filename_already_listed(out, count, entry->d_name)) continue;

        char path[300];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        memset(&out[count], 0, sizeof(out[count]));
        snprintf(out[count].filename, sizeof(out[count].filename), "%.*s",
                 (int)sizeof(out[count].filename) - 1, entry->d_name);

        if (!quick_parse_meta(path, &out[count])) {
            if (!include_invalid) continue;
            snprintf(out[count].title, sizeof(out[count].title), "%.*s",
                     (int)sizeof(out[count].title) - 1, entry->d_name);
            out[count].bpm = 0;
            out[count].note_count = 0;
            out[count].measure_count = 0;
            out[count].file_size = 0;
            snprintf(out[count].key, sizeof(out[count].key), "%s", "C major");
            snprintf(out[count].time_signature,
                     sizeof(out[count].time_signature), "%s", "4/4");
        }
        count++;
    }
    closedir(dir);
    return count;
}

static void sort_score_entries(score_info_t *out, int count)
{
    for (int i = 0; i < count - 1; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (strcasecmp(out[i].title, out[j].title) > 0) {
                score_info_t temp = out[i];
                out[i] = out[j];
                out[j] = temp;
            }
        }
    }
}

int score_storage_scan_sd(score_info_t *out, int max_count)
{
    if (!out || max_count <= 0) return -1;

    DIR *dir = opendir(SD_SCORE_DIR);
    if (!dir) return -1;
    closedir(dir);

    int count = scan_score_directory(SD_SCORE_DIR, out, 0, max_count, false);
    sort_score_entries(out, count);
    return count;
}

int score_storage_scan(score_info_t *out, int max_count)
{
    if (!out || max_count <= 0) return -1;

    int count = 0;
    memset(&out[count], 0, sizeof(out[count]));
    snprintf(out[count].filename, sizeof(out[count].filename), "%s",
             BUILTIN_SCORE_FILENAME);
    snprintf(out[count].title, sizeof(out[count].title), "%s",
             "小星星（内置测试）");
    snprintf(out[count].key, sizeof(out[count].key), "%s", "C major");
    snprintf(out[count].time_signature, sizeof(out[count].time_signature),
             "%s", "4/4");
    out[count].bpm = 100;
    out[count].note_count = 14;
    out[count].measure_count = 4;
    out[count].file_size = sizeof(s_builtin_twinkle_json) - 1;
    count++;

    count = scan_score_directory(SD_SCORE_DIR, out, count, max_count, true);
    if (score_storage_ensure_mounted() == ESP_OK)
        count = scan_score_directory(SPIFFS_SCORE_DIR, out, count,
                                     max_count, true);
    sort_score_entries(out, count);
    ESP_LOGI(TAG, "found %d score entries (SD preferred)", count);
    return count;
}

bool score_storage_read_json(const char *filename, char **json, size_t *size)
{
    if (json) *json = NULL;
    if (size) *size = 0;
    if (!valid_filename(filename) || !json) return false;

    if (strcmp(filename, BUILTIN_SCORE_FILENAME) == 0) {
        size_t builtin_size = sizeof(s_builtin_twinkle_json) - 1;
        char *copy = malloc(builtin_size + 1);
        if (!copy) return false;
        memcpy(copy, s_builtin_twinkle_json, builtin_size + 1);
        *json = copy;
        if (size) *size = builtin_size;
        return true;
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", SD_SCORE_DIR, filename);

    FILE *f = fopen(path, "rb");
    if (!f && score_storage_ensure_mounted() == ESP_OK) {
        snprintf(path, sizeof(path), "%s/%s", SPIFFS_SCORE_DIR, filename);
        f = fopen(path, "rb");
    }
    if (!f) {
        ESP_LOGE(TAG, "cannot open score %s", filename);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > SCORE_STORAGE_MAX_FILE_BYTES) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    if (rd != (size_t)fsize) { free(buf); return false; }
    buf[rd] = '\0';
    *json = buf;
    if (size) *size = rd;
    return true;
}

bool score_storage_read_sd_json(const char *filename, char **json, size_t *size)
{
    if (json) *json = NULL;
    if (size) *size = 0;
    if (!score_storage_is_valid_sd_filename(filename) || !json) return false;

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", SD_SCORE_DIR, filename);
    FILE *file = fopen(path, "rb");
    if (!file) return false;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || file_size > SCORE_STORAGE_MAX_FILE_BYTES) {
        fclose(file);
        return false;
    }
    rewind(file);

    char *buffer = malloc((size_t)file_size + 1);
    if (!buffer) {
        fclose(file);
        return false;
    }
    size_t read_size = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size != (size_t)file_size) {
        free(buffer);
        return false;
    }

    buffer[read_size] = '\0';
    *json = buffer;
    if (size) *size = read_size;
    return true;
}

esp_err_t score_storage_rename_sd_title(const char *filename,
                                        const char *title)
{
    if (!score_storage_is_valid_sd_filename(filename) || !title ||
        !title[0])
        return ESP_ERR_INVALID_ARG;

    size_t title_length = strlen(title);
    if (title_length >= sizeof(((score_info_t *)0)->title))
        return ESP_ERR_INVALID_SIZE;
    for (size_t index = 0; index < title_length; ++index) {
        unsigned char value = (unsigned char)title[index];
        if (value < 0x20 || value == 0x7f)
            return ESP_ERR_INVALID_ARG;
    }

    char *source = NULL;
    size_t source_size = 0;
    if (!score_storage_read_sd_json(filename, &source, &source_size))
        return ESP_ERR_NOT_FOUND;

    cJSON *root = cJSON_Parse(source);
    free(source);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *replacement = cJSON_CreateString(title);
    if (!replacement) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_HasObjectItem(root, "title")) {
        if (!cJSON_ReplaceItemInObjectCaseSensitive(root, "title",
                                                    replacement)) {
            cJSON_Delete(replacement);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    } else {
        cJSON_AddItemToObject(root, "title", replacement);
    }

    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!serialized) return ESP_ERR_NO_MEM;
    size_t serialized_size = strlen(serialized);
    if (!serialized_size ||
        serialized_size > SCORE_STORAGE_MAX_FILE_BYTES) {
        free(serialized);
        return ESP_ERR_INVALID_SIZE;
    }

    char path[300];
    char temp_path[300];
    char backup_path[300];
    snprintf(path, sizeof(path), "%s/%s", SD_SCORE_DIR, filename);
    snprintf(temp_path, sizeof(temp_path), "%s/%s", SD_SCORE_DIR,
             RENAME_TEMP_FILENAME);
    snprintf(backup_path, sizeof(backup_path), "%s/%s", SD_SCORE_DIR,
             RENAME_BACKUP_FILENAME);

    remove(temp_path);
    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        free(serialized);
        return ESP_FAIL;
    }
    bool written =
        fwrite(serialized, 1, serialized_size, file) == serialized_size;
    if (fflush(file) != 0) written = false;
    if (fclose(file) != 0) written = false;
    free(serialized);
    if (!written) {
        remove(temp_path);
        return ESP_FAIL;
    }

    remove(backup_path);
    if (rename(path, backup_path) != 0) {
        ESP_LOGE(TAG, "cannot stage score rename %s (errno=%d)",
                 path, errno);
        remove(temp_path);
        return ESP_FAIL;
    }
    if (rename(temp_path, path) != 0) {
        ESP_LOGE(TAG, "cannot finalize score rename %s (errno=%d)",
                 path, errno);
        rename(backup_path, path);
        remove(temp_path);
        return ESP_FAIL;
    }
    remove(backup_path);
    ESP_LOGI(TAG, "renamed SD score %s to %s", filename, title);
    return ESP_OK;
}

static esp_err_t ensure_sd_score_directory(void)
{
    struct stat st;
    if (stat(SD_MOUNT_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
        return ESP_ERR_NOT_FOUND;

    if (stat(SD_SCORE_DIR, &st) == 0)
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    if (mkdir(SD_SCORE_DIR, 0775) == 0 || errno == EEXIST)
        return ESP_OK;
    ESP_LOGE(TAG, "cannot create %s (errno=%d)", SD_SCORE_DIR, errno);
    return ESP_FAIL;
}

static int next_creator_score_id(void)
{
    int max_id = 0;
    DIR *dir = opendir(SD_SCORE_DIR);
    if (!dir) return 1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int id = 0;
        char trailing = '\0';
        if (sscanf(entry->d_name, "CR%6d.JSON%c", &id, &trailing) == 1 &&
            id > max_id && id <= 999999)
            max_id = id;
    }
    closedir(dir);
    return max_id < 999999 ? max_id + 1 : 0;
}

esp_err_t score_storage_save_midi_auto(const midi_data_t *score,
                                       char *saved_title, size_t title_size,
                                       char *saved_filename,
                                       size_t filename_size)
{
    if (saved_title && title_size) saved_title[0] = '\0';
    if (saved_filename && filename_size) saved_filename[0] = '\0';
    if (!score || score->note_count <= 0 ||
        score->note_count > MAX_NOTES)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = ensure_sd_score_directory();
    if (err != ESP_OK) return err;

    int id = next_creator_score_id();
    if (id <= 0) return ESP_FAIL;

    char filename[32];
    char title[64];
    char path[300];
    char temp_path[300];
    snprintf(filename, sizeof(filename), "CR%06d.JSON", id);
    snprintf(title, sizeof(title), CREATOR_TITLE_PREFIX "%d", id);
    snprintf(path, sizeof(path), "%s/%s", SD_SCORE_DIR, filename);
    snprintf(temp_path, sizeof(temp_path), "%s/%s", SD_SCORE_DIR,
             CREATOR_TEMP_FILENAME);

    int bpm = score->bpm > 0 ? score->bpm : 120;
    int ticks_per_quarter = score->ticks_per_quarter > 0 ?
                            score->ticks_per_quarter : 480;
    int denominator = score->time_sig_den >= 0 && score->time_sig_den <= 6 ?
                      1 << score->time_sig_den : 4;
    if (denominator != 4 && denominator != 8) denominator = 4;
    int numerator = score->time_sig_num > 0 ? score->time_sig_num : 4;
    char time_signature[16];
    snprintf(time_signature, sizeof(time_signature), "%d/%d",
             numerator, denominator);

    bool grand_staff = false;
    for (int i = 0; i < score->note_count; ++i)
        if (score->notes[i].staff == 2) grand_staff = true;

    cJSON *root = cJSON_CreateObject();
    cJSON *notes = cJSON_CreateArray();
    if (!root || !notes) {
        cJSON_Delete(root);
        cJSON_Delete(notes);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "title", title);
    cJSON_AddNumberToObject(root, "bpm", bpm);
    cJSON_AddStringToObject(root, "time_signature", time_signature);
    cJSON_AddStringToObject(root, "key", "C major");
    cJSON_AddStringToObject(root, "source", "creator");
    cJSON_AddNumberToObject(root, "ticks_per_quarter", ticks_per_quarter);
    cJSON_AddStringToObject(root, "staff_mode",
                            grand_staff ? "grand" : "single");
    cJSON_AddItemToObject(root, "notes", notes);

    double seconds_per_tick =
        60.0 / ((double)bpm * (double)ticks_per_quarter);
    for (int i = 0; i < score->note_count; ++i) {
        const midi_note_t *source = &score->notes[i];
        cJSON *note = cJSON_CreateObject();
        if (!note) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(note, "midi", source->note);
        cJSON_AddNumberToObject(note, "velocity", source->velocity);
        cJSON_AddNumberToObject(note, "start",
                                source->start_tick * seconds_per_tick);
        cJSON_AddNumberToObject(note, "duration",
                                source->duration * seconds_per_tick);
        cJSON_AddNumberToObject(note, "start_tick", source->start_tick);
        cJSON_AddNumberToObject(note, "duration_ticks", source->duration);
        cJSON_AddNumberToObject(note, "staff",
                                source->staff == 2 ? 2 : 1);
        cJSON_AddNumberToObject(note, "voice",
                                source->voice > 0 ? source->voice : 1);
        cJSON_AddItemToArray(notes, note);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    size_t json_size = strlen(json);
    if (!json_size || json_size > SCORE_STORAGE_MAX_FILE_BYTES) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }

    remove(temp_path);
    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "cannot create %s (errno=%d)", temp_path, errno);
        free(json);
        return ESP_FAIL;
    }
    bool written = fwrite(json, 1, json_size, file) == json_size;
    if (fflush(file) != 0) written = false;
    if (fclose(file) != 0) written = false;
    free(json);
    if (!written) {
        remove(temp_path);
        return ESP_FAIL;
    }
    if (rename(temp_path, path) != 0) {
        ESP_LOGE(TAG, "cannot finalize %s (errno=%d)", path, errno);
        remove(temp_path);
        return ESP_FAIL;
    }

    if (saved_title && title_size)
        snprintf(saved_title, title_size, "%s", title);
    if (saved_filename && filename_size)
        snprintf(saved_filename, filename_size, "%s", filename);
    ESP_LOGI(TAG, "saved creator score %s to %s (%d notes)",
             title, path, score->note_count);
    return ESP_OK;
}

bool score_storage_load_with_tempo(const char *filename, int tempo_bpm)
{
    char *buf = NULL;
    size_t size = 0;
    if (!score_storage_read_json(filename, &buf, &size)) return false;

    char error[128] = {0};
    esp_err_t err = score_json_parse_and_store_with_tempo(
        buf, size, tempo_bpm, error, sizeof(error));
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parse %s failed: %s", filename,
                 error[0] ? error : esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "loaded score: %s", filename);
    return true;
}

bool score_storage_load(const char *filename)
{
    return score_storage_load_with_tempo(filename, 0);
}

bool score_storage_load_sd(const char *filename)
{
    char *buffer = NULL;
    size_t size = 0;
    if (!score_storage_read_sd_json(filename, &buffer, &size)) return false;

    char error[128] = {0};
    esp_err_t err = score_json_parse_and_store_with_tempo(
        buffer, size, 0, error, sizeof(error));
    free(buffer);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parse SD score %s failed: %s", filename,
                 error[0] ? error : esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "loaded SD score: %s", filename);
    return true;
}
