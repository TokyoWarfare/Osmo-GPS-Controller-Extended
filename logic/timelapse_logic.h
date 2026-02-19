#ifndef TIMELAPSE_LOGIC_H
#define TIMELAPSE_LOGIC_H

#include <stdbool.h>

// Inicializar el módulo de timelapse
void timelapse_logic_init(void);

// Llamar desde update_camera_state_handler cuando cambia el estado
void timelapse_on_camera_status_changed(void);

// Start/stop desde consola
void timelapse_start(void);
void timelapse_stop(void);

bool timelapse_is_running(void);

#endif
