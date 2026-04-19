#include <cmath>
#include <algorithm>
#include <cstdio>

#include "driver/uart.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dl_image_jpeg.hpp"
#include "human_face_detect.hpp"

// ---------------------------
// XIAO ESP32-S3 Sense camera
// ---------------------------
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// UART link to the second ESP32 (TX + GND required).
// Adjust pins if your wiring uses different GPIOs.
#define LINK_UART_PORT    UART_NUM_1
#define LINK_UART_TX_GPIO GPIO_NUM_1
#define LINK_UART_BAUD    115200

static esp_err_t init_link_uart(void)
{
    const uart_config_t cfg = {
        .baud_rate = LINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(
        LINK_UART_PORT, LINK_UART_TX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    return ESP_OK;
}

static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline float absf(float x) {
    return x < 0 ? -x : x;
}

static inline float dist2d(float x1, float y1, float x2, float y2) {
    const float dx = x1 - x2;
    const float dy = y1 - y2;
    return std::sqrt(dx * dx + dy * dy);
}

static esp_err_t init_camera(void) {
    camera_config_t config = {};
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;

    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;

    // Good starting point for face detection on S3.
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;   // 320x240
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.sccb_i2c_port = 0;

    return esp_camera_init(&config);
}

// Pick the strongest face if multiple are detected.
template <typename ResultsT>
static const typename ResultsT::value_type *pick_best_face(const ResultsT &results) {
    const typename ResultsT::value_type *best = nullptr;
    float best_score = -1.0f;

    for (const auto &r : results) {
        const int x1 = r.box[0];
        const int y1 = r.box[1];
        const int x2 = r.box[2];
        const int y2 = r.box[3];
        const float area = (float)std::max(0, x2 - x1) * (float)std::max(0, y2 - y1);

        // Prefer high confidence and a reasonably large face.
        const float score = r.score * 0.8f + area * 0.00001f * 0.2f;
        if (score > best_score) {
            best_score = score;
            best = &r;
        }
    }

    return best;
}

// Compute a 0..100 "looking straight" percentage.
// Uses:
// - detector confidence
// - box centering
// - face size
// - landmark symmetry when available
template <typename FaceResultT>
static float compute_straight_percent(const FaceResultT &res, int img_w, int img_h) {
    const float x1 = (float)res.box[0];
    const float y1 = (float)res.box[1];
    const float x2 = (float)res.box[2];
    const float y2 = (float)res.box[3];

    const float w = std::max(1.0f, x2 - x1);
    const float h = std::max(1.0f, y2 - y1);
    const float cx = (x1 + x2) * 0.5f;
    const float cy = (y1 + y2) * 0.5f;

    // 1) detection confidence
    const float conf_score = clamp01(res.score);

    // 2) centering in frame
    const float norm_dx = absf(cx - img_w * 0.5f) / (img_w * 0.5f);
    const float norm_dy = absf(cy - img_h * 0.5f) / (img_h * 0.5f);
    const float center_x_score = clamp01(1.0f - norm_dx);
    const float center_y_score = clamp01(1.0f - norm_dy);

    // 3) face size term
    const float area_ratio = (w * h) / (float)(img_w * img_h);
    float size_score = 0.0f;
    if (area_ratio < 0.05f) {
        size_score = area_ratio / 0.05f;
    } else if (area_ratio > 0.40f) {
        size_score = 0.40f / area_ratio;
    } else {
        size_score = 1.0f;
    }
    size_score = clamp01(size_score);

    // 4) landmark symmetry term (if available)
    float symmetry_score = 0.60f; // fallback if landmarks absent
    float roll_score = 0.60f;

    if (res.keypoint.size() >= 10) {
        const float lx = (float)res.keypoint[0];
        const float ly = (float)res.keypoint[1];
        const float lmx = (float)res.keypoint[2];
        const float lmy = (float)res.keypoint[3];
        const float nx = (float)res.keypoint[4];
        const float ny = (float)res.keypoint[5];
        const float rx = (float)res.keypoint[6];
        const float ry = (float)res.keypoint[7];
        const float rmx = (float)res.keypoint[8];
        const float rmy = (float)res.keypoint[9];

        // Eyes should be roughly symmetric around the nose for a frontal face.
        const float d_left_eye = dist2d(lx, ly, nx, ny);
        const float d_right_eye = dist2d(rx, ry, nx, ny);
        const float eye_sym = 1.0f - absf(d_left_eye - d_right_eye) / std::max(1.0f, (d_left_eye + d_right_eye) * 0.5f);

        // Mouth corners should also be roughly symmetric around the nose.
        const float d_left_mouth = dist2d(lmx, lmy, nx, ny);
        const float d_right_mouth = dist2d(rmx, rmy, nx, ny);
        const float mouth_sym = 1.0f - absf(d_left_mouth - d_right_mouth) / std::max(1.0f, (d_left_mouth + d_right_mouth) * 0.5f);

        symmetry_score = clamp01(0.55f * eye_sym + 0.45f * mouth_sym);

        // If face is tilted, eye line or mouth line will not be level.
        const float eye_roll = absf(ly - ry) / h;
        const float mouth_roll = absf(lmy - rmy) / h;
        roll_score = clamp01(1.0f - (0.7f * eye_roll + 0.3f * mouth_roll) * 3.0f);
    }

    // Weighted fusion.
    const float total =
        0.30f * conf_score +
        0.20f * center_x_score +
        0.10f * center_y_score +
        0.10f * size_score +
        0.20f * symmetry_score +
        0.10f * roll_score;

    return 100.0f * clamp01(total);
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(init_camera());
    ESP_ERROR_CHECK(init_link_uart());

    // Create detector once and reuse it.
    HumanFaceDetect *detector = new HumanFaceDetect();

    // Sampling and publishing schedule:
    // - fetch/detect every 250 ms
    // - publish rolling mean of last 2 s every 500 ms
    const int sample_period_ms = 250;
    const int publish_period_ms = 500;
    const int mean_window_ms = 2000;
    const int mean_window_samples = mean_window_ms / sample_period_ms; // 8 samples

    float recent_percents[mean_window_samples] = {0};
    int recent_count = 0;
    int recent_write_idx = 0;

    float mean_percent_2s = 0.0f;
    int64_t next_publish_ms = 0;

    const TickType_t check_period_ticks = pdMS_TO_TICKS(sample_period_ms);
    TickType_t last_wake_tick = xTaskGetTickCount();

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelayUntil(&last_wake_tick, check_period_ticks);
            continue;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            esp_camera_fb_return(fb);
            vTaskDelayUntil(&last_wake_tick, check_period_ticks);
            continue;
        }

        dl::image::jpeg_img_t jpeg_img = {
            .data = (void *)fb->buf,
            .data_len = (size_t)fb->len
        };

        auto img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
        if (!img.data) {
            esp_camera_fb_return(fb);
            vTaskDelayUntil(&last_wake_tick, check_period_ticks);
            continue;
        }

        auto &results = detector->run(img);
        const int64_t sample_ts_ms = esp_timer_get_time() / 1000;

        float instant_percent = 0.0f;

        if (const auto *best = pick_best_face(results)) {
            instant_percent = compute_straight_percent(*best, img.width, img.height);
        }

        // Keep recent samples in a rolling 2-second window (8 samples at 250 ms).
        recent_percents[recent_write_idx] = instant_percent;
        recent_write_idx = (recent_write_idx + 1) % mean_window_samples;
        if (recent_count < mean_window_samples) {
            recent_count++;
        }

        if (next_publish_ms == 0) {
            next_publish_ms = sample_ts_ms + publish_period_ms;
        }

        if (sample_ts_ms >= next_publish_ms) {
            float sum = 0.0f;
            for (int i = 0; i < recent_count; ++i) {
                sum += recent_percents[i];
            }
            mean_percent_2s = (recent_count > 0) ? (sum / (float)recent_count) : 0.0f;

            // Only output/send the 2-second rolling mean every 500 ms.
            char out_line[24];
            const int out_len = std::snprintf(out_line, sizeof(out_line), "%.1f\n", mean_percent_2s);
            if (out_len > 0) {
                uart_write_bytes(LINK_UART_PORT, out_line, out_len);
                // Keep USB console output identical to UART payload for easy debug.
                printf("%s", out_line);
            }

            do {
                next_publish_ms += publish_period_ms;
            } while (sample_ts_ms >= next_publish_ms);
        }

        heap_caps_free(img.data);
        esp_camera_fb_return(fb);

        vTaskDelayUntil(&last_wake_tick, check_period_ticks);
    }

    // Unreachable in normal use.
    delete detector;
}