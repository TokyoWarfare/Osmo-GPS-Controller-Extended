// ─────────────────────────────────────────────────────────────────────
// main/app_main.c
// ─────────────────────────────────────────────────────────────────────

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "connect_logic.h"
#include "gps_logic.h"
#include "key_logic.h"
#include "light_logic.h"
#include "timelapse_logic.h"
#include "status_logic.h"

#define TAG "MAIN"


// ─────────────────────────────────────────────
// Consola serial simple
// ─────────────────────────────────────────────
static void console_task(void *arg)
{
    char cmd[32];
    int pos = 0;

    vTaskDelay(pdMS_TO_TICKS(3000));

    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║       Osmo360 GPS Controller         ║\n");
    printf("║  tstart - iniciar timelapse          ║\n");
    printf("║  tstop  - parar timelapse            ║\n");
    printf("║  status - estado cámara              ║\n");
    printf("║  h      - ayuda                      ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("> ");
    fflush(stdout);

    while (1) {
        int c = getchar();
        
        if (c != EOF && c != -1) {
            if (c == '\n' || c == '\r') {
                cmd[pos] = '\0';
                printf("\n");

                if (pos > 0) {
                    if (strcmp(cmd, "tstart") == 0) {
                        timelapse_start();
                    }
                    else if (strcmp(cmd, "tstop") == 0) {
                        timelapse_stop();
                    }
                    else if (strcmp(cmd, "status") == 0) {
                        printf("Timelapse: %s\n", timelapse_is_running() ? "CORRIENDO" : "PARADO");
                        printf("Camara mode: %d  status: %d\n",
                               current_camera_mode, current_camera_status);
                    }
                    else if (c == 'h' || strcmp(cmd, "help") == 0) {
                        printf("Comandos:\n");
                        printf("  tstart  - iniciar timelapse foto 360\n");
                        printf("  tstop   - parar timelapse\n");
                        printf("  status  - estado actual\n");
                    }
                    else {
                        printf("Desconocido: '%s'  (escribe 'h')\n", cmd);
                    }
                }

                printf("> ");
                fflush(stdout);
                pos = 0;
            }
            else if ((c == 127 || c == 8) && pos > 0) {  // Backspace
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
            else if (pos < (int)(sizeof(cmd) - 1) && c >= 0x20) {
                cmd[pos++] = (char)c;
                printf("%c", c);
                fflush(stdout);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─────────────────────────────────────────────
// app_main
// ─────────────────────────────────────────────
void app_main(void)
{
    int res = 0;

    /* Inicializar LED */
    res = init_light_logic();
    if (res != 0) return;

    /* Inicializar GPS */
    initSendGpsDataToCameraTask();
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Inicializar BLE */
    res = connect_logic_ble_init();
    if (res != 0) return;

    /* Inicializar botón BOOT */
    key_logic_init();

    /* Inicializar timelapse (aquí, dentro, se puede crear la tarea del botón físico) */
    timelapse_logic_init();

    /* Arrancar consola serial */
    xTaskCreate(console_task, "console", 4096, NULL, 0, NULL);

    


    /* Loop principal */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    

    
    
}
