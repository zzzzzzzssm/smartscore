#include "voice_recognition.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_inmp441.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "qwen_realtime.h"
#include "voice_config.h"
#include "voice_uart_link.h"

static const char *TAG = "voice_recognition";

typedef enum {
    RECOGNITION_WAIT_WAKE = 0,
    RECOGNITION_WAIT_COMMAND,
    RECOGNITION_AI_STREAMING,
    RECOGNITION_AI_WAIT_RESPONSE,
    RECOGNITION_WAKE_REARM_GUARD,
} recognition_state_t;

typedef struct {
    int command_id;
    const char *display_text;
} canonical_command_t;

typedef struct {
    int command_id;
    const char *display_text;
    const char *model_text;
} command_alias_t;

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static void yield_to_idle_during_model_init(void)
{
    /* ESP-SR constructors are synchronous and can occupy CPU0 for seconds. */
    vTaskDelay(pdMS_TO_TICKS(20));
}

static const canonical_command_t s_canonical_commands[] = {
    {VOICE_COMMAND_START_PRACTICE, "开始练习"},
    {VOICE_COMMAND_STOP_PRACTICE, "停止练习"},
    {VOICE_COMMAND_NEXT_PAGE, "下一页"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "上一页"},
    {VOICE_COMMAND_RESTART, "重新开始"},
    {VOICE_COMMAND_VIEW_SCORE, "查看评分"},
    {VOICE_COMMAND_HOME, "返回首页"},
};

static const command_alias_t s_command_aliases[] = {
    {VOICE_COMMAND_START_PRACTICE, "开始练习", "kai shi lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "开始训练", "kai shi xun lian"},
    {VOICE_COMMAND_START_PRACTICE, "进入练习", "jin ru lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "进入训练", "jin ru xun lian"},
    {VOICE_COMMAND_START_PRACTICE, "开启练习", "kai qi lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "启动练习", "qi dong lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "进行练习", "jin xing lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "现在开始", "xian zai kai shi"},
    {VOICE_COMMAND_START_PRACTICE, "现在开始练习", "xian zai kai shi lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "我要练习", "wo yao lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "我要开始练习", "wo yao kai shi lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "开始本次练习", "kai shi ben ci lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "请开始练习", "qing kai shi lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "帮我开始练习", "bang wo kai shi lian xi"},
    {VOICE_COMMAND_START_PRACTICE, "准备开始练习", "zhun bei kai shi lian xi"},

    {VOICE_COMMAND_STOP_PRACTICE, "停止练习", "ting zhi lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "停止训练", "ting zhi xun lian"},
    {VOICE_COMMAND_STOP_PRACTICE, "结束练习", "jie shu lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "结束训练", "jie shu xun lian"},
    {VOICE_COMMAND_STOP_PRACTICE, "退出练习", "tui chu lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "终止练习", "zhong zhi lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "暂停练习", "zan ting lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "先停一下", "xian ting yi xia"},
    {VOICE_COMMAND_STOP_PRACTICE, "停下练习", "ting xia lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "停止本次练习", "ting zhi ben ci lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "结束本次练习", "jie shu ben ci lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "练习结束", "lian xi jie shu"},
    {VOICE_COMMAND_STOP_PRACTICE, "请停止练习", "qing ting zhi lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "帮我停止练习", "bang wo ting zhi lian xi"},
    {VOICE_COMMAND_STOP_PRACTICE, "不练了", "bu lian le"},

    {VOICE_COMMAND_NEXT_PAGE, "下一页", "xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "翻下一页", "fan xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "翻到下一页", "fan dao xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "进入下一页", "jin ru xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "打开下一页", "da kai xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "切换到下一页", "qie huan dao xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "跳到下一页", "tiao dao xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "往后翻一页", "wang hou fan yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "向后翻一页", "xiang hou fan yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "往后翻页", "wang hou fan ye"},
    {VOICE_COMMAND_NEXT_PAGE, "向后翻页", "xiang hou fan ye"},
    {VOICE_COMMAND_NEXT_PAGE, "后面一页", "hou mian yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "请翻下一页", "qing fan xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "帮我翻下一页", "bang wo fan xia yi ye"},
    {VOICE_COMMAND_NEXT_PAGE, "下一张", "xia yi zhang"},

    {VOICE_COMMAND_PREVIOUS_PAGE, "上一页", "shang yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "翻上一页", "fan shang yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "翻到上一页", "fan dao shang yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "前一页", "qian yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "往前翻一页", "wang qian fan yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "向前翻一页", "xiang qian fan yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "往前翻页", "wang qian fan ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "向前翻页", "xiang qian fan ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "前面一页", "qian mian yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "退回一页", "tui hui yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "后退一页", "hou tui yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "回退一页", "hui tui yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "请翻上一页", "qing fan shang yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "帮我翻上一页", "bang wo fan shang yi ye"},
    {VOICE_COMMAND_PREVIOUS_PAGE, "上一张", "shang yi zhang"},

    {VOICE_COMMAND_RESTART, "重新开始", "chong xin kai shi"},
    {VOICE_COMMAND_RESTART, "从头开始", "cong tou kai shi"},
    {VOICE_COMMAND_RESTART, "重头开始", "chong tou kai shi"},
    {VOICE_COMMAND_RESTART, "重来一次", "chong lai yi ci"},
    {VOICE_COMMAND_RESTART, "再来一次", "zai lai yi ci"},
    {VOICE_COMMAND_RESTART, "重新来过", "chong xin lai guo"},
    {VOICE_COMMAND_RESTART, "从头练习", "cong tou lian xi"},
    {VOICE_COMMAND_RESTART, "重新练习", "chong xin lian xi"},
    {VOICE_COMMAND_RESTART, "再练一次", "zai lian yi ci"},
    {VOICE_COMMAND_RESTART, "重新训练", "chong xin xun lian"},
    {VOICE_COMMAND_RESTART, "重新开始练习", "chong xin kai shi lian xi"},
    {VOICE_COMMAND_RESTART, "我要再来", "wo yao zai lai"},
    {VOICE_COMMAND_RESTART, "请重新开始", "qing chong xin kai shi"},
    {VOICE_COMMAND_RESTART, "帮我重新开始", "bang wo chong xin kai shi"},
    {VOICE_COMMAND_RESTART, "再做一遍", "zai zuo yi bian"},

    {VOICE_COMMAND_VIEW_SCORE, "查看评分", "cha kan ping fen"},
    {VOICE_COMMAND_VIEW_SCORE, "查看分数", "cha kan fen shu"},
    {VOICE_COMMAND_VIEW_SCORE, "查看成绩", "cha kan cheng ji"},
    {VOICE_COMMAND_VIEW_SCORE, "查看得分", "cha kan de fen"},
    {VOICE_COMMAND_VIEW_SCORE, "显示评分", "xian shi ping fen"},
    {VOICE_COMMAND_VIEW_SCORE, "显示分数", "xian shi fen shu"},
    {VOICE_COMMAND_VIEW_SCORE, "显示成绩", "xian shi cheng ji"},
    {VOICE_COMMAND_VIEW_SCORE, "显示得分", "xian shi de fen"},
    {VOICE_COMMAND_VIEW_SCORE, "看看评分", "kan kan ping fen"},
    {VOICE_COMMAND_VIEW_SCORE, "看看分数", "kan kan fen shu"},
    {VOICE_COMMAND_VIEW_SCORE, "评分结果", "ping fen jie guo"},
    {VOICE_COMMAND_VIEW_SCORE, "练习成绩", "lian xi cheng ji"},
    {VOICE_COMMAND_VIEW_SCORE, "请查看评分", "qing cha kan ping fen"},
    {VOICE_COMMAND_VIEW_SCORE, "帮我查看评分", "bang wo cha kan ping fen"},
    {VOICE_COMMAND_VIEW_SCORE, "我的分数", "wo de fen shu"},

    {VOICE_COMMAND_HOME, "返回首页", "fan hui shou ye"},
    {VOICE_COMMAND_HOME, "回到首页", "hui dao shou ye"},
    {VOICE_COMMAND_HOME, "进入首页", "jin ru shou ye"},
    {VOICE_COMMAND_HOME, "打开首页", "da kai shou ye"},
    {VOICE_COMMAND_HOME, "返回主页", "fan hui zhu ye"},
    {VOICE_COMMAND_HOME, "回到主页", "hui dao zhu ye"},
    {VOICE_COMMAND_HOME, "进入主页", "jin ru zhu ye"},
    {VOICE_COMMAND_HOME, "打开主页", "da kai zhu ye"},
    {VOICE_COMMAND_HOME, "返回主界面", "fan hui zhu jie mian"},
    {VOICE_COMMAND_HOME, "回到主界面", "hui dao zhu jie mian"},
    {VOICE_COMMAND_HOME, "返回主菜单", "fan hui zhu cai dan"},
    {VOICE_COMMAND_HOME, "回到主菜单", "hui dao zhu cai dan"},
    {VOICE_COMMAND_HOME, "请返回首页", "qing fan hui shou ye"},
    {VOICE_COMMAND_HOME, "帮我返回首页", "bang wo fan hui shou ye"},
    {VOICE_COMMAND_HOME, "回到主页面", "hui dao zhu ye mian"},
};

static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_multinet_data;
static size_t s_feed_chunk_samples;
static size_t s_fetch_chunk_samples;
static volatile bool s_running;
static bool s_commands_allocated;

_Static_assert(ARRAY_SIZE(s_canonical_commands) == 7,
               "Exactly seven voice commands are required");
_Static_assert(ARRAY_SIZE(s_command_aliases) == 105,
               "Exactly 105 command aliases are required");
_Static_assert(ARRAY_SIZE(s_command_aliases) <= ESP_MN_MAX_PHRASE_NUM,
               "Command aliases exceed the MultiNet phrase limit");

static void cleanup_partial_init(void)
{
    s_running = false;
    if (s_commands_allocated) {
        (void)esp_mn_commands_free();
        s_commands_allocated = false;
    }
    if (s_multinet != NULL && s_multinet_data != NULL) {
        s_multinet->destroy(s_multinet_data);
        s_multinet_data = NULL;
    }
    s_multinet = NULL;
    if (s_afe_handle != NULL && s_afe_data != NULL) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    s_afe_handle = NULL;
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    s_feed_chunk_samples = 0;
    s_fetch_chunk_samples = 0;
}

static esp_err_t validate_loaded_models(char **wake_name, char **multinet_name)
{
    if (s_models == NULL || s_models->num <= 0 || s_models->model_name == NULL) {
        ESP_LOGE(TAG, "模型分区为空或格式无效");
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < s_models->num; ++i) {
        if (s_models->model_name[i] != NULL) {
            ESP_LOGI(TAG, "已加载模型[%d]: %s", i, s_models->model_name[i]);
        }
    }

    *wake_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, VOICE_WAKE_MODEL_KEYWORD);
    if (*wake_name == NULL) {
        ESP_LOGE(TAG, "模型分区中找不到官方“%s”WakeNet 模型", VOICE_WAKE_WORD_TEXT);
        return ESP_ERR_NOT_FOUND;
    }

    *multinet_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (*multinet_name == NULL) {
        ESP_LOGE(TAG, "模型分区中找不到中文 MultiNet 模型");
        return ESP_ERR_NOT_FOUND;
    }

    const char *wake_words = esp_srmodel_get_wake_words(s_models, *wake_name);
    ESP_LOGI(TAG, "WakeNet: %s (%s)", *wake_name,
             wake_words != NULL ? wake_words : VOICE_WAKE_WORD_TEXT);
    ESP_LOGI(TAG, "MultiNet: %s", *multinet_name);
    return ESP_OK;
}

static esp_err_t register_commands(void)
{
    esp_err_t err = esp_mn_commands_alloc(s_multinet, s_multinet_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "分配 MultiNet 命令表失败: %s", esp_err_to_name(err));
        return err;
    }
    s_commands_allocated = true;

    for (size_t i = 0; i < ARRAY_SIZE(s_command_aliases); ++i) {
        err = esp_mn_commands_add(s_command_aliases[i].command_id,
                                  s_command_aliases[i].model_text);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "添加命令别名 %d（%s）失败: %s",
                     s_command_aliases[i].command_id,
                     s_command_aliases[i].display_text,
                     esp_err_to_name(err));
            return err;
        }
    }

    esp_mn_error_t *command_errors = esp_mn_commands_update();
    if (command_errors != NULL) {
        ESP_LOGE(TAG, "MultiNet 拒绝 %d 条命令", command_errors->num);
        if (command_errors->num > 0 && command_errors->phrases != NULL) {
            const int count = command_errors->num > (int)ARRAY_SIZE(s_command_aliases)
                                  ? (int)ARRAY_SIZE(s_command_aliases)
                                  : command_errors->num;
            for (int i = 0; i < count; ++i) {
                if (command_errors->phrases[i] != NULL) {
                    ESP_LOGE(TAG, "无效命令: %s",
                             command_errors->phrases[i]->string != NULL
                                 ? command_errors->phrases[i]->string
                                 : "<null>");
                }
            }
        }
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "已注册 %u 条语音短语，映射到 %u 个命令 ID",
             (unsigned)ARRAY_SIZE(s_command_aliases),
             (unsigned)ARRAY_SIZE(s_canonical_commands));
    return ESP_OK;
}

static const char *command_display_text_from_id(int command_id)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_canonical_commands); ++i) {
        if (s_canonical_commands[i].command_id == command_id) {
            return s_canonical_commands[i].display_text;
        }
    }
    return NULL;
}

esp_err_t voice_recognition_init(void)
{
    if (s_models != NULL || s_afe_data != NULL || s_multinet_data != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_models = esp_srmodel_init(VOICE_MODEL_PARTITION_LABEL);
    if (s_models == NULL) {
        ESP_LOGE(TAG, "从“%s”分区加载 ESP-SR 模型失败", VOICE_MODEL_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    yield_to_idle_during_model_init();

    char *wake_name = NULL;
    char *multinet_name = NULL;
    esp_err_t err = validate_loaded_models(&wake_name, &multinet_name);
    if (err != ESP_OK) {
        cleanup_partial_init();
        return err;
    }
    yield_to_idle_during_model_init();

    afe_config_t *afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "创建单麦克风 AFE 配置失败");
        cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }

    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->ns_init = true;
    afe_config->vad_init = true;
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = wake_name;
    afe_config->wakenet_model_name_2 = NULL;
    afe_config->vad_enable_channel_trigger = false;
    afe_config->fixed_first_channel = true;
    afe_config->fixed_output_channel = true;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_config_t *checked_config = afe_config_check(afe_config);
    if (checked_config == NULL) {
        ESP_LOGE(TAG, "AFE 配置检查返回空指针");
        afe_config_free(afe_config);
        cleanup_partial_init();
        return ESP_ERR_INVALID_STATE;
    }
    afe_config = checked_config;
    if (afe_config->pcm_config.mic_num != 1 ||
        afe_config->pcm_config.ref_num != 0 || afe_config->aec_init ||
        afe_config->se_init || !afe_config->ns_init || !afe_config->vad_init ||
        !afe_config->wakenet_init) {
        ESP_LOGE(TAG, "AFE 配置检查失败：必须为单麦克风、VAD/NS/WakeNet 开启、AEC/SE 关闭");
        afe_config_free(afe_config);
        cleanup_partial_init();
        return ESP_ERR_INVALID_STATE;
    }

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    if (s_afe_handle == NULL) {
        ESP_LOGE(TAG, "获取 AFE 接口失败");
        afe_config_free(afe_config);
        cleanup_partial_init();
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "创建 AFE 实例失败");
        cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }

    yield_to_idle_during_model_init();

    const int feed_chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    const int fetch_chunk = s_afe_handle->get_fetch_chunksize(s_afe_data);
    const int feed_channels = s_afe_handle->get_feed_channel_num(s_afe_data);
    const int sample_rate = s_afe_handle->get_samp_rate(s_afe_data);
    if (feed_chunk <= 0 || fetch_chunk <= 0 || feed_channels != 1 ||
        sample_rate != (int)VOICE_SAMPLE_RATE_HZ) {
        ESP_LOGE(TAG, "AFE 音频参数无效: feed=%d fetch=%d channels=%d rate=%d",
                 feed_chunk, fetch_chunk, feed_channels, sample_rate);
        cleanup_partial_init();
        return ESP_ERR_INVALID_SIZE;
    }
    s_feed_chunk_samples = (size_t)feed_chunk;
    s_fetch_chunk_samples = (size_t)fetch_chunk;

    s_multinet = esp_mn_handle_from_name(multinet_name);
    if (s_multinet == NULL) {
        ESP_LOGE(TAG, "获取 MultiNet 接口失败: %s", multinet_name);
        cleanup_partial_init();
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_multinet_data = s_multinet->create(multinet_name, VOICE_COMMAND_TIMEOUT_MS);
    if (s_multinet_data == NULL) {
        ESP_LOGE(TAG, "创建 MultiNet 实例失败: %s", multinet_name);
        cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }

    yield_to_idle_during_model_init();

    const int multinet_chunk = s_multinet->get_samp_chunksize(s_multinet_data);
    const int multinet_rate = s_multinet->get_samp_rate(s_multinet_data);
    const char *multinet_language = s_multinet->get_language(s_multinet_data);
    if (multinet_chunk != fetch_chunk || multinet_rate != (int)VOICE_SAMPLE_RATE_HZ ||
        multinet_language == NULL || strcmp(multinet_language, ESP_MN_CHINESE) != 0) {
        ESP_LOGE(TAG, "AFE/MultiNet 参数不匹配: afe=%d mn=%d mn_rate=%d",
                 fetch_chunk, multinet_chunk, multinet_rate);
        cleanup_partial_init();
        return ESP_ERR_INVALID_SIZE;
    }

    err = register_commands();
    if (err != ESP_OK) {
        cleanup_partial_init();
        return err;
    }
    yield_to_idle_during_model_init();

    s_afe_handle->print_pipeline(s_afe_data);
    s_running = true;
    ESP_LOGI(TAG, "AFE feed=%u samples, fetch=%u samples",
             (unsigned)s_feed_chunk_samples, (unsigned)s_fetch_chunk_samples);
    return ESP_OK;
}

size_t voice_recognition_get_feed_chunk_samples(void)
{
    return s_feed_chunk_samples;
}

void voice_recognition_stop(void)
{
    s_running = false;
}

void voice_recognition_feed_task(void *arg)
{
    (void)arg;
    int16_t *pcm = heap_caps_malloc(
        s_feed_chunk_samples * sizeof(*pcm),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        ESP_LOGE(TAG, "采集任务无法分配 PCM 帧缓冲");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        esp_err_t err = audio_inmp441_read_pcm(pcm, s_feed_chunk_samples);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S 读取失败，停止语音链路: %s", esp_err_to_name(err));
            s_running = false;
            break;
        }

        const int feed_result = s_afe_handle->feed(s_afe_data, pcm);
        if (feed_result < 0) {
            ESP_LOGE(TAG, "AFE feed 失败: %d", feed_result);
            s_running = false;
            break;
        }
    }

    free(pcm);
    vTaskDelete(NULL);
}

static bool finish_command_session(void)
{
    s_multinet->clean(s_multinet_data);
    const int enable_result = s_afe_handle->enable_wakenet(s_afe_data);
    if (enable_result < 1) {
        ESP_LOGE(TAG, "重新启用 WakeNet 失败: %d", enable_result);
        s_running = false;
        return false;
    }
    return true;
}

static bool prepare_wake_rearm_guard(void)
{
    s_multinet->clean(s_multinet_data);

    /* WakeNet normally disables itself after detection.  Keep it explicitly
     * disabled while the final speaker tail is removed from the AFE. */
    if (s_afe_handle->disable_wakenet != NULL) {
        const int disable_result =
            s_afe_handle->disable_wakenet(s_afe_data);
        if (disable_result < 0) {
            ESP_LOGE(TAG, "failed to hold WakeNet disabled: %d",
                     disable_result);
            return false;
        }
    }
    if (s_afe_handle->reset_vad != NULL) {
        const int reset_vad_result = s_afe_handle->reset_vad(s_afe_data);
        if (reset_vad_result < 0) {
            ESP_LOGW(TAG, "VAD reset before wake rearm failed: %d",
                     reset_vad_result);
        }
    }
    if (s_afe_handle->reset_buffer != NULL) {
        const int reset_result = s_afe_handle->reset_buffer(s_afe_data);
        if (reset_result < 1) {
            ESP_LOGE(TAG, "AFE buffer reset before wake rearm failed: %d",
                     reset_result);
            return false;
        }
    }
    return true;
}

static uint32_t recognition_frames_for_ms(uint32_t duration_ms)
{
    const uint64_t denominator =
        (uint64_t)s_fetch_chunk_samples * 1000U;
    if (denominator == 0U) return 1U;
    const uint64_t numerator =
        (uint64_t)duration_ms * VOICE_SAMPLE_RATE_HZ;
    uint32_t frames = (uint32_t)((numerator + denominator - 1U) /
                                 denominator);
    return frames > 0U ? frames : 1U;
}

static bool begin_ai_conversation(const char *trigger,
                                  bool send_timeout_event)
{
    esp_err_t link_error = ESP_OK;
    if (send_timeout_event) {
        link_error = voice_uart_link_send_timeout();
        if (link_error != ESP_OK) {
            ESP_LOGW(TAG, "failed to close local command wait UI: %s",
                     esp_err_to_name(link_error));
        }
    }
    link_error = qwen_realtime_begin();
    if (link_error != ESP_OK) {
        ESP_LOGW(TAG, "failed to confirm Qwen conversation: %s",
                 esp_err_to_name(link_error));
        return false;
    }
    link_error = qwen_realtime_speech_end();
    if (link_error != ESP_OK) {
        ESP_LOGW(TAG, "failed to queue Qwen speech end: %s",
                 esp_err_to_name(link_error));
        qwen_realtime_cancel_local();
        return false;
    }
    ESP_LOGI(TAG, "entering one-turn Qwen AI conversation: %s",
             trigger != NULL ? trigger : "local-command-not-matched");
    return true;
}

void voice_recognition_detect_task(void *arg)
{
    (void)arg;
    recognition_state_t state = RECOGNITION_WAIT_WAKE;
    uint32_t wait_command_frames = 0;
    uint32_t query_speech_frames = 0;
    uint32_t query_silence_frames = 0;
    bool query_gate_open = false;
    const uint32_t post_wake_guard_frames =
        recognition_frames_for_ms(VOICE_AI_POST_WAKE_GUARD_MS);
    const uint32_t min_query_speech_frames =
        recognition_frames_for_ms(VOICE_AI_MIN_QUERY_SPEECH_MS);
    const uint32_t end_silence_frames =
        recognition_frames_for_ms(VOICE_AI_END_SILENCE_MS);
    const uint32_t wake_rearm_guard_frames =
        recognition_frames_for_ms(VOICE_WAKE_REARM_GUARD_MS);
    uint32_t wake_rearm_frames_remaining = 0U;

    while (s_running) {
        afe_fetch_result_t *result = s_afe_handle->fetch(s_afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch 失败，停止语音链路");
            s_running = false;
            break;
        }
        if (result->data == NULL || result->data_size <
                                        (int)(s_fetch_chunk_samples * sizeof(int16_t))) {
            ESP_LOGE(TAG, "AFE fetch 返回的音频帧无效: %d 字节", result->data_size);
            s_running = false;
            break;
        }

        if (state == RECOGNITION_AI_WAIT_RESPONSE) {
            if (qwen_realtime_state() == QWEN_VOICE_IDLE) {
                if (!prepare_wake_rearm_guard()) {
                    s_running = false;
                    break;
                }
                wake_rearm_frames_remaining = wake_rearm_guard_frames;
                state = RECOGNITION_WAKE_REARM_GUARD;
                ESP_LOGI(TAG,
                         "Qwen answer completed; clearing speaker tail for "
                         "%u ms before WakeNet rearm",
                         (unsigned)VOICE_WAKE_REARM_GUARD_MS);
            }
            continue;
        }

        if (state == RECOGNITION_WAKE_REARM_GUARD) {
            if (wake_rearm_frames_remaining > 0U) {
                --wake_rearm_frames_remaining;
                continue;
            }
            if (!finish_command_session()) break;
            state = RECOGNITION_WAIT_WAKE;
            ESP_LOGI(TAG,
                     "wake path re-armed; explicit wake word is required");
            continue;
        }

        if (state == RECOGNITION_WAIT_WAKE && result->wakeup_state == WAKENET_DETECTED) {
            s_multinet->clean(s_multinet_data);
            state = RECOGNITION_WAIT_COMMAND;
            printf("唤醒成功，请说出命令\n");
            fflush(stdout);
            esp_err_t link_error = voice_uart_link_send_wake();
            esp_err_t arm_error = qwen_realtime_arm();
            wait_command_frames = 0;
            query_speech_frames = 0;
            query_silence_frames = 0;
            query_gate_open = false;
            if (link_error != ESP_OK) {
                ESP_LOGW(TAG, "发送唤醒事件失败: %s",
                         esp_err_to_name(link_error));
            }
            if (arm_error != ESP_OK) {
                ESP_LOGW(TAG, "Qwen candidate arm failed: %s",
                         esp_err_to_name(arm_error));
            }
        }

        if (state != RECOGNITION_WAIT_COMMAND) {
            continue;
        }

        ++wait_command_frames;
        bool ai_utterance_complete = false;
        if (wait_command_frames > post_wake_guard_frames) {
            if (!query_gate_open) {
                query_gate_open = true;
                ESP_LOGI(TAG,
                         "post-wake guard ended; listening for one query");
            }
        }
        if (query_gate_open) {
            esp_err_t stream_error = qwen_realtime_push_pcm(
                result->data, s_fetch_chunk_samples);
            if (stream_error != ESP_OK &&
                stream_error != ESP_ERR_INVALID_STATE &&
                stream_error != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "AI candidate audio enqueue failed: %s",
                         esp_err_to_name(stream_error));
            }
            if (result->vad_state == VAD_SPEECH) {
                ++query_speech_frames;
                query_silence_frames = 0;
            } else if (query_speech_frames >= min_query_speech_frames) {
                ++query_silence_frames;
            }
            ai_utterance_complete =
                query_speech_frames >= min_query_speech_frames &&
                query_silence_frames >= end_silence_frames;
        }

        const esp_mn_state_t mn_state = s_multinet->detect(s_multinet_data, result->data);
        if (mn_state == ESP_MN_STATE_DETECTING) {
            if (ai_utterance_complete) {
                if (begin_ai_conversation("VAD end-of-utterance", true)) {
                    state = RECOGNITION_AI_WAIT_RESPONSE;
                } else {
                    qwen_realtime_cancel_local();
                    if (!finish_command_session()) break;
                    state = RECOGNITION_WAIT_WAKE;
                }
            }
            continue;
        }

        if (mn_state == ESP_MN_STATE_TIMEOUT) {
            if (query_speech_frames >= min_query_speech_frames &&
                !ai_utterance_complete) {
                /* Do not cut a longer spoken question at MultiNet's timeout. */
                continue;
            }
            ESP_LOGI(TAG, "本地命令未命中");
            esp_err_t link_error = voice_uart_link_send_timeout();
            if (link_error != ESP_OK) {
                ESP_LOGW(TAG, "发送超时事件失败: %s",
                         esp_err_to_name(link_error));
            }
            if (query_speech_frames < min_query_speech_frames) {
                ESP_LOGI(TAG, "no post-wake utterance; returning to WakeNet");
                qwen_realtime_cancel_local();
                if (!finish_command_session()) break;
                state = RECOGNITION_WAIT_WAKE;
                continue;
            }
            if (qwen_realtime_state() == QWEN_VOICE_IDLE) {
                ESP_LOGW(TAG, "Qwen candidate unavailable; restoring local wake mode");
                if (!finish_command_session()) break;
                state = RECOGNITION_WAIT_WAKE;
                continue;
            }
            ESP_LOGI(TAG, "进入豆包 AI 开放式对话");
            if (!begin_ai_conversation("MultiNet timeout", false)) {
                link_error = ESP_FAIL;
                ESP_LOGW(TAG, "发送 AI 对话开始事件失败: %s",
                         esp_err_to_name(link_error));
                qwen_realtime_cancel_local();
                if (!finish_command_session()) break;
                state = RECOGNITION_WAIT_WAKE;
                continue;
            }
            state = RECOGNITION_AI_WAIT_RESPONSE;
            continue;
        }

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = s_multinet->get_results(s_multinet_data);
            int command_id = 0;
            if (mn_result != NULL && mn_result->num > 0 &&
                mn_result->num <= ESP_MN_RESULT_MAX_NUM) {
                command_id = mn_result->command_id[0];
            }

            if (command_id >= VOICE_COMMAND_ID_MIN && command_id <= VOICE_COMMAND_ID_MAX) {
                const char *command_text = command_display_text_from_id(command_id);
                ESP_LOGI(TAG, "识别命令: %s (ID=%d)",
                         command_text != NULL ? command_text : "未知", command_id);
                printf("%d\n", command_id);
                fflush(stdout);
                qwen_realtime_cancel_local();
                esp_err_t link_error = voice_uart_link_send_command(
                    (uint8_t)command_id);
                if (link_error != ESP_OK) {
                    ESP_LOGW(TAG, "发送命令事件失败: %s",
                             esp_err_to_name(link_error));
                }
            } else {
                ESP_LOGW(TAG, "忽略无效命令结果: ID=%d", command_id);
                qwen_realtime_cancel_local();
            }

            if (!finish_command_session()) {
                break;
            }
            state = RECOGNITION_WAIT_WAKE;
        }
    }

    vTaskDelete(NULL);
}
