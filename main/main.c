#include <math.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_params.h"

#define PIR_PIN GPIO_NUM_13
#define TRIG_PIN GPIO_NUM_5
#define ECHO_PIN GPIO_NUM_18

#define ULTRASONIC_TIMEOUT_US 30000
#define CLOSING_IN_THRESHOLD 1.0f
#define MIN_VALID_DISTANCE_CM 2.0f
#define MAX_VALID_DISTANCE_CM 400.0f

#define SAMPLE_PERIOD_MS 500
#define WINDOW_DURATION_MS 2000
#define WINDOW (WINDOW_DURATION_MS / SAMPLE_PERIOD_MS)
#define CLOSE_THRESH 100.0f
#define PRINT_ALERTS 0
#define PRINT_FEATURE_STREAM 0
#define PRINT_MODEL_STREAM 1
#define FIRMWARE_TAG "FUSION_DECISION_V1"

static const float DECISION_MATRIX[2] = {0.65f, 0.35f};
#define DECISION_BIAS 0.0f
#define LOOKING_THRESHOLD 0.50f
#define CAMERA_PLACEHOLDER_PROBABILITY 0.50f

typedef struct {
    float q;
    float r;
    float x;
    float p;
    float k;
} SimpleKalmanFilter;

static bool g_runtime_inference_enabled = false;

static void kalman_init(SimpleKalmanFilter *kf, float q, float r, float p, float initial_value) {
    kf->q = q;
    kf->r = r;
    kf->p = p;
    kf->x = initial_value;
}

static float kalman_update(SimpleKalmanFilter *kf, float measurement) {
    kf->p = kf->p + kf->q;
    kf->k = kf->p / (kf->p + kf->r);
    kf->x = kf->x + kf->k * (measurement - kf->x);
    kf->p = (1.0f - kf->k) * kf->p;
    return kf->x;
}

static float meanFloat(const float *buf) {
    float sum = 0.0f;
    for (int i = 0; i < WINDOW; i++) {
        sum += buf[i];
    }
    return sum / (float)WINDOW;
}

static float minFloat(const float *buf) {
    float min_val = buf[0];
    for (int i = 1; i < WINDOW; i++) {
        if (buf[i] < min_val) {
            min_val = buf[i];
        }
    }
    return min_val;
}

static float maxFloat(const float *buf) {
    float max_val = buf[0];
    for (int i = 1; i < WINDOW; i++) {
        if (buf[i] > max_val) {
            max_val = buf[i];
        }
    }
    return max_val;
}

static float varianceFloat(const float *buf) {
    float mean = meanFloat(buf);
    float sum_sq = 0.0f;
    for (int i = 0; i < WINDOW; i++) {
        float delta = buf[i] - mean;
        sum_sq += delta * delta;
    }
    return sum_sq / (float)WINDOW;
}

static int countClose(const float *buf, float threshold_cm) {
    int count = 0;
    for (int i = 0; i < WINDOW; i++) {
        if (buf[i] <= threshold_cm) {
            count++;
        }
    }
    return count;
}

static float meanInt(const int *buf) {
    int sum = 0;
    for (int i = 0; i < WINDOW; i++) {
        sum += buf[i];
    }
    return (float)sum / (float)WINDOW;
}

static int countTransitions(const int *buf) {
    int transitions = 0;
    for (int i = 1; i < WINDOW; i++) {
        if (buf[i] != buf[i - 1]) {
            transitions++;
        }
    }
    return transitions;
}

static void shiftFloatLeft(float *buf) {
    for (int i = 0; i < WINDOW - 1; i++) {
        buf[i] = buf[i + 1];
    }
}

static void shiftIntLeft(int *buf) {
    for (int i = 0; i < WINDOW - 1; i++) {
        buf[i] = buf[i + 1];
    }
}

static float sigmoidf_stable(float value) {
    if (value >= 0.0f) {
        float z = expf(-value);
        return 1.0f / (1.0f + z);
    }

    float z = expf(value);
    return z / (1.0f + z);
}

static float model_predict_probability(const float *features) {
    float logit = MODEL_INTERCEPT;
    for (int i = 0; i < MODEL_FEATURE_COUNT; i++) {
        float scale = MODEL_SCALER_SCALE[i];
        if (scale == 0.0f) {
            scale = 1.0f;
        }

        float standardized = (features[i] - MODEL_SCALER_MEAN[i]) / scale;
        logit += MODEL_COEFFICIENTS[i] * standardized;
    }
    return sigmoidf_stable(logit);
}

static float clamp01f(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static float get_esp32cam_probability_placeholder(void) {
    return CAMERA_PLACEHOLDER_PROBABILITY;
}

static float fuse_decision_score(float sensor_probability, float camera_probability) {
    float score =
        (DECISION_MATRIX[0] * sensor_probability) +
        (DECISION_MATRIX[1] * camera_probability) +
        DECISION_BIAS;
    return clamp01f(score);
}

static void start_runtime_inference(void) {
    g_runtime_inference_enabled = true;
    printf("FW_TAG,%s,build=%s %s\n", FIRMWARE_TAG, __DATE__, __TIME__);
    printf("System initialized. Onboard decision inference active.\n");
    printf(
        "FUSION_CONFIG,sensor_weight=%.2f,camera_weight=%.2f,bias=%.2f,threshold=%.2f\n",
        DECISION_MATRIX[0],
        DECISION_MATRIX[1],
        DECISION_BIAS,
        LOOKING_THRESHOLD
    );
    if (PRINT_FEATURE_STREAM) {
        printf(
            "FEATURE_HEADER,dist_mean,dist_min,dist_max,dist_start,dist_end,"
            "dist_slope,dist_var,time_close,pir_high_fraction,pir_transition_count\n"
        );
    }
    if (PRINT_MODEL_STREAM) {
        printf("PRED_HEADER,sensor_probability,sensor_label\n");
        printf("FUSION_HEADER,sensor_probability,camera_probability,fused_score,looking_label\n");
    }
    printf("DISPLAY_HEADER,state\n");
}

static void sensor_task(void *pvParameters) {
    (void)pvParameters;

    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);
    gpio_set_level(TRIG_PIN, 0);

    SimpleKalmanFilter distance_filter;
    kalman_init(&distance_filter, 2.0f, 1.5f, 1.0f, 0.0f);

    bool is_first_reading = true;
    float previous_distance = 0.0f;

    float dist_buf[WINDOW] = {0.0f};
    int pir_buf[WINDOW] = {0};
    int samples_collected = 0;
    float features[MODEL_FEATURE_COUNT] = {0.0f};

    while (1) {
        int pir_state = gpio_get_level(PIR_PIN);

        gpio_set_level(TRIG_PIN, 0);
        esp_rom_delay_us(2);
        gpio_set_level(TRIG_PIN, 1);
        esp_rom_delay_us(10);
        gpio_set_level(TRIG_PIN, 0);

        int64_t start_time = esp_timer_get_time();
        int timeout_counter = 0;

        while (gpio_get_level(ECHO_PIN) == 0 && timeout_counter < ULTRASONIC_TIMEOUT_US) {
            esp_rom_delay_us(1);
            timeout_counter++;
        }

        start_time = esp_timer_get_time();
        timeout_counter = 0;

        while (gpio_get_level(ECHO_PIN) == 1 && timeout_counter < ULTRASONIC_TIMEOUT_US) {
            esp_rom_delay_us(1);
            timeout_counter++;
        }

        int64_t end_time = esp_timer_get_time();
        int64_t duration = end_time - start_time;

        bool have_distance_sample = false;
        float filtered_distance = previous_distance;

        if (timeout_counter < ULTRASONIC_TIMEOUT_US) {
            float raw_distance_cm = ((float)duration * 0.0343f) / 2.0f;

            if (raw_distance_cm >= MIN_VALID_DISTANCE_CM && raw_distance_cm <= MAX_VALID_DISTANCE_CM) {
                if (is_first_reading) {
                    distance_filter.x = raw_distance_cm;
                    previous_distance = raw_distance_cm;
                    is_first_reading = false;
                }

                filtered_distance = kalman_update(&distance_filter, raw_distance_cm);
                have_distance_sample = true;
            }
        }

        if (!have_distance_sample && !is_first_reading) {
            filtered_distance = previous_distance;
            have_distance_sample = true;
        }

        if (have_distance_sample) {
            if (
                PRINT_ALERTS &&
                pir_state == 1 &&
                (previous_distance - filtered_distance > CLOSING_IN_THRESHOLD)
            ) {
                printf(
                    "ALERT: Target detected and closing in! (Current Dist: %6.2f cm)\n",
                    filtered_distance
                );
            }

            previous_distance = filtered_distance;

            if (samples_collected < WINDOW) {
                dist_buf[samples_collected] = filtered_distance;
                pir_buf[samples_collected] = pir_state;
                samples_collected++;
            } else {
                shiftFloatLeft(dist_buf);
                shiftIntLeft(pir_buf);
                dist_buf[WINDOW - 1] = filtered_distance;
                pir_buf[WINDOW - 1] = pir_state;
            }

            if (g_runtime_inference_enabled && samples_collected == WINDOW) {
                features[0] = meanFloat(dist_buf);
                features[1] = minFloat(dist_buf);
                features[2] = maxFloat(dist_buf);
                features[3] = dist_buf[0];
                features[4] = dist_buf[WINDOW - 1];
                features[5] = dist_buf[WINDOW - 1] - dist_buf[0];
                features[6] = varianceFloat(dist_buf);
                features[7] = (float)countClose(dist_buf, CLOSE_THRESH);
                features[8] = meanInt(pir_buf);
                features[9] = (float)countTransitions(pir_buf);

                if (PRINT_FEATURE_STREAM) {
                    printf(
                        "FEATURES,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%d\n",
                        features[0],
                        features[1],
                        features[2],
                        features[3],
                        features[4],
                        features[5],
                        features[6],
                        (int)features[7],
                        features[8],
                        (int)features[9]
                    );
                }

                float sensor_probability = model_predict_probability(features);
                int sensor_label = sensor_probability >= MODEL_THRESHOLD ? 1 : 0;
                float camera_probability = get_esp32cam_probability_placeholder();
                float fused_score = fuse_decision_score(sensor_probability, camera_probability);
                int looking_label = fused_score >= LOOKING_THRESHOLD ? 1 : 0;

                if (PRINT_MODEL_STREAM) {
                    printf("PRED,%.4f,%d\n", sensor_probability, sensor_label);
                    printf(
                        "FUSION,%.4f,%.4f,%.4f,%d\n",
                        sensor_probability,
                        camera_probability,
                        fused_score,
                        looking_label
                    );
                }
                printf("DISPLAY,%s\n", looking_label ? "LOOKING" : "NOT_LOOKING");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void app_main(void) {
    start_runtime_inference();
    xTaskCreate(sensor_task, "sensor_read_task", 4096, NULL, 5, NULL);
}
