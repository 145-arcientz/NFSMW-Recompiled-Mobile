#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reproduce lo que hace el SDK con la memoria del invitado (src/system/xmemory.cpp):
// una zona compartida de 0x120000000 bytes vista dos veces dentro de una reserva de
// 0x220000000, mas la captura de fallos de pagina de mmio_handler. Escribe el informe
// en salida (texto) y devuelve 0 si todo lo necesario funciona.
int nfsmw_prueba_memoria(const char* dir_tmp, char* salida, size_t tam_salida);

#ifdef __cplusplus
}
#endif
