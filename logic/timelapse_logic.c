/* timelapse_logic.c
 *
 * Timelapse de fotos 360 usando:
 *  - Estado de cámara vía status_logic (push 1D02)
 *  - Conexión/protocolo vía connect_logic
 *  - Disparo vía command_logic_key_report_snapshot()
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "timelapse_logic.h"
#include "enums_logic.h"
#include "status_logic.h"
#include "command_logic.h"
#include "connect_logic.h"
#include "driver/gpio.h"

#define TAG "TIMELAPSE"

// physical button trigger
static const int photoLapseToggleGPIO = 12;

// Estado interno
static volatile bool s_running = false;

// Semáforo para “cámara lista para siguiente foto”
static SemaphoreHandle_t s_ready_sem = NULL;

// Contador global (solo informativo)
static uint32_t s_photo_count = 0;

// ─────────────────────────────────────────────
// Helpers internos
// ─────────────────────────────────────────────

static bool is_protocol_ready(void)
{
    return (connect_logic_get_state() == PROTOCOL_CONNECTED);
}

static bool is_camera_ready_for_photo(void)
{
    return camera_status_initialized &&
           (current_camera_status == CAMERA_STATUS_LIVE_STREAMING);
}

static esp_err_t wait_for_protocol_connected(TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();

    while (xTaskGetTickCount() - start < timeout_ticks) {
        if (is_protocol_ready()) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "Timeout esperando PROTOCOL_CONNECTED");
    return ESP_FAIL;
}

static esp_err_t wait_for_camera_ready(TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();

    while (xTaskGetTickCount() - start < timeout_ticks) {

        if (!s_running) return ESP_FAIL;

        if (!is_protocol_ready()) {
            ESP_LOGW(TAG, "Protocolo no conectado mientras esperábamos cámara lista");
            return ESP_FAIL;
        }

        if (is_camera_ready_for_photo()) {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "Timeout esperando cámara en estado LIVE_STREAMING");
    return ESP_FAIL;
}

// ─────────────────────────────────────────────
// Physical trigger button
// ─────────────────────────────────────────────
static void timelapse_button_task(void *arg)
{
    bool last_state = true;  // pull-up → HIGH cuando no se pulsa

    while (1) {
        bool state = gpio_get_level(photoLapseToggleGPIO);

        // Detectar flanco descendente (HIGH → LOW)
        if (last_state == true && state == false) {

            vTaskDelay(pdMS_TO_TICKS(50)); // debounce

            if (gpio_get_level(photoLapseToggleGPIO) == false) {

                if (timelapse_is_running()) {
                    ESP_LOGI("TIMELAPSE_BTN", "Botón físico: STOP");
                    timelapse_stop();
                } else {
                    ESP_LOGI("TIMELAPSE_BTN", "Botón físico: START");
                    timelapse_start();
                }
            }
        }

        last_state = state;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ─────────────────────────────────────────────
// Tarea principal del timelapse
// ─────────────────────────────────────────────
static void timelapse_task(void *arg)
{
    ESP_LOGI(TAG, "Timelapse task started");

    s_photo_count = 0;

    while (s_running) {

        if (wait_for_protocol_connected(pdMS_TO_TICKS(30000)) != ESP_OK) {
            ESP_LOGW(TAG, "No hay protocolo, reintentando en 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (current_camera_mode != CAMERA_MODE_PANORAMIC_PHOTO_360) {
            ESP_LOGI(TAG, "Cambiando cámara a PANORAMIC_PHOTO_360 (0x3F)...");
            camera_mode_switch_response_frame_t *sw =
                command_logic_switch_camera_mode(CAMERA_MODE_PANORAMIC_PHOTO_360);

            if (sw != NULL) {
                ESP_LOGI(TAG, "Mode switch OK (ret_code=%d)", sw->ret_code);
                free(sw);
            } else {
                ESP_LOGW(TAG, "Mode switch sin respuesta");
            }

            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        if (wait_for_camera_ready(pdMS_TO_TICKS(15000)) != ESP_OK) {
            ESP_LOGW(TAG, "Cámara no lista para foto, reintentando...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_running) break;

        ESP_LOGI(TAG, "📸 Iniciando captura #%lu", (unsigned long)(s_photo_count + 1));

        record_control_response_frame_t *rec = command_logic_start_record();
        if (rec == NULL) {
            ESP_LOGW(TAG, "Start_record falló, reintentando ciclo...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        free(rec);

        TickType_t start = xTaskGetTickCount();
        while (current_camera_status == CAMERA_STATUS_LIVE_STREAMING &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {
            if (!s_running) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!s_running) break;

        ESP_LOGI(TAG, "Esperando fin de captura (vuelta a LIVE_STREAMING)...");

        if (xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(15000)) == pdFALSE) {
            ESP_LOGW(TAG, "Timeout esperando LIVE_STREAMING tras captura");
            continue;
        }

        record_control_response_frame_t *stop = command_logic_stop_record();
        if (stop != NULL) free(stop);

        s_photo_count++;
        ESP_LOGI(TAG, "✅ Captura #%lu completada", (unsigned long)s_photo_count);
    }

    ESP_LOGI(TAG, "Timelapse detenido. Total capturas: %lu", (unsigned long)s_photo_count);
    vTaskDelete(NULL);
}

// ─────────────────────────────────────────────
// API pública
// ─────────────────────────────────────────────

void timelapse_logic_init(void)
{
    if (s_ready_sem == NULL) {
        s_ready_sem = xSemaphoreCreateBinary();
    }

    // Inicializar GPIO del botón físico
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << photoLapseToggleGPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // Crear tarea del botón
    xTaskCreate(timelapse_button_task, "tl_btn", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Timelapse logic initialized. Commands: 'tstart' / 'tstop'");
}

void timelapse_on_camera_status_changed(void)
{
    if (!s_running) return;

    if (is_protocol_ready() && is_camera_ready_for_photo()) {
        xSemaphoreGive(s_ready_sem);
    }
}

void timelapse_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Timelapse ya está corriendo");
        return;
    }

    if (!is_protocol_ready()) {
        ESP_LOGE(TAG, "No hay protocolo con la cámara");
        return;
    }

    s_running = true;
    s_photo_count = 0;

    if (s_ready_sem != NULL) {
        xSemaphoreTake(s_ready_sem, 0);
    }

    BaseType_t ok = xTaskCreate(
        timelapse_task,
        "timelapse_task",
        4096,
        NULL,
        5,
        NULL
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear timelapse_task");
        s_running = false;
    } else {
        ESP_LOGI(TAG, "🎬 Timelapse INICIADO");
    }
}

void timelapse_stop(void)
{
    if (!s_running) {
        ESP_LOGW(TAG, "Timelapse no está corriendo");
        return;
    }

    s_running = false;

    if (s_ready_sem != NULL) {
        xSemaphoreGive(s_ready_sem);
    }

    ESP_LOGI(TAG, "⏹️  Timelapse DETENIDO");
}

bool timelapse_is_running(void)
{
    return s_running;
}
