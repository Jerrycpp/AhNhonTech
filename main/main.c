#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h" 

// --- Pin Definitions ---
#define PIR_PIN    GPIO_NUM_13
#define TRIG_PIN   GPIO_NUM_5
#define ECHO_PIN   GPIO_NUM_18

// --- Configuration Limits ---
#define ULTRASONIC_TIMEOUT_US 30000 
#define CLOSING_IN_THRESHOLD  1.0f 
#define MIN_VALID_DISTANCE_CM 2.0f
#define MAX_VALID_DISTANCE_CM 400.0f

// --- Kalman Filter Struct & Functions ---
typedef struct {
    float q; 
    float r; 
    float x; 
    float p; 
    float k; 
} SimpleKalmanFilter;

void kalman_init(SimpleKalmanFilter *kf, float q, float r, float p, float initial_value) {
    kf->q = q;
    kf->r = r;
    kf->p = p;
    kf->x = initial_value;
}

float kalman_update(SimpleKalmanFilter *kf, float measurement) {
    kf->p = kf->p + kf->q;
    kf->k = kf->p / (kf->p + kf->r);
    kf->x = kf->x + kf->k * (measurement - kf->x);
    kf->p = (1.0f - kf->k) * kf->p;
    return kf->x;
}

void sensor_task(void *pvParameters) {
    // Configure Pins
    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);
    gpio_set_level(TRIG_PIN, 0); 

    // Initialize Kalman Filter (Tuned for speed: Q=2.0, R=1.5)
    SimpleKalmanFilter distance_filter;
    kalman_init(&distance_filter, 2.0f, 1.5f, 1.0f, 0.0f);
    
    bool is_first_reading = true;
    float previous_distance = 0.0f;

    printf("System initialized. Fast polling with outlier rejection active...\n");
    printf("-----------------------------------------------------------------\n");

    while (1) {
        // 1. Read PIR Sensor 
        int pir_state = gpio_get_level(PIR_PIN);

        // 2. Trigger Ultrasonic Sensor
        gpio_set_level(TRIG_PIN, 0);
        esp_rom_delay_us(2);
        gpio_set_level(TRIG_PIN, 1);
        esp_rom_delay_us(10);
        gpio_set_level(TRIG_PIN, 0);

        // 3. Measure Echo Time
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

        // 4. Process Data
        if (timeout_counter < ULTRASONIC_TIMEOUT_US) {
             float raw_distance_cm = (duration * 0.0343) / 2.0;
             
             // OUTLIER REJECTION: Only process physically plausible readings
             if (raw_distance_cm >= MIN_VALID_DISTANCE_CM && raw_distance_cm <= MAX_VALID_DISTANCE_CM) {
                 
                 // Seed the filter on the very first valid read
                 if (is_first_reading) {
                     distance_filter.x = raw_distance_cm;
                     previous_distance = raw_distance_cm;
                     is_first_reading = false;
                 }

                 // The Kalman filter now only sees "sane" data
                 float filtered_distance = kalman_update(&distance_filter, raw_distance_cm);

                 // CONDITION CHECK: Motion detected AND object is closing in
                 if (pir_state == 1 && (previous_distance - filtered_distance > CLOSING_IN_THRESHOLD)) {
                     printf("ALERT: Target detected and closing in! (Current Dist: %6.2f cm)\n", filtered_distance);
                 }

                 // Store current filtered distance for the next loop's comparison
                 previous_distance = filtered_distance;
                 
             }
        }

        // HARDWARE LIMIT: 60ms delay outside of all logic to prevent acoustic collisions and Watchdog resets
        vTaskDelay(pdMS_TO_TICKS(60)); 
    }
}

void app_main(void) {
    // Spin up the sensor task
    xTaskCreate(&sensor_task, "sensor_read_task", 4096, NULL, 5, NULL);
}