#include "memoria.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <os/proc.h>
#endif

// Los tamanos del SDK (xmemory.cpp). En iOS entra por MapViewsMac (TARGET_OS_MAC vale 1): una
// sola reserva de 4,5 GB con todas las vistas dentro, no los 8,5 GB de MapViews en Linux.
#define TAM_COMPARTIDA 0x120000000ull
#define TAM_RESERVA 0x120000000ull

static char* g_salida;
static size_t g_tam, g_usado;

static void linea(const char* formato, ...) {
  if (g_usado + 1 >= g_tam) return;
  va_list args;
  va_start(args, formato);
  int n = vsnprintf(g_salida + g_usado, g_tam - g_usado, formato, args);
  va_end(args);
  if (n > 0) g_usado += (size_t)n < g_tam - g_usado ? (size_t)n : g_tam - g_usado - 1;
}

static double mb(uint64_t b) { return (double)b / (1024.0 * 1024.0); }

static uint64_t disponible(void) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  return (uint64_t)os_proc_available_memory();
#else
  return 0;
#endif
}

// mmio_handler: el SDK protege paginas y las arregla desde el manejador de SIGSEGV/SIGBUS.
static volatile uint8_t* g_pagina;
static size_t g_tam_pagina;
static volatile int g_fallos;
static void manejador(int sig, siginfo_t* info, void* ctx) {
  (void)ctx;
  if ((uint8_t*)info->si_addr >= g_pagina && (uint8_t*)info->si_addr < g_pagina + g_tam_pagina) {
    ++g_fallos;
    mprotect((void*)g_pagina, g_tam_pagina, PROT_READ | PROT_WRITE);
    return;  // se repite la instruccion, ya con permiso
  }
  signal(sig, SIG_DFL);
}

int nfsmw_prueba_memoria(const char* dir_tmp, char* salida, size_t tam_salida) {
  g_salida = salida;
  g_tam = tam_salida;
  g_usado = 0;
  if (tam_salida) salida[0] = 0;
  int bien = 1;

  char modelo[64] = "?";
  size_t t = sizeof(modelo);
  sysctlbyname("hw.machine", modelo, &t, NULL, 0);
  uint64_t ram = 0;
  t = sizeof(ram);
  sysctlbyname("hw.memsize", &ram, &t, NULL, 0);
  linea("NFSMW - prueba de memoria para iOS\n");
  linea("Dispositivo %s, RAM %.0f MB, pagina %ld bytes\n", modelo, mb(ram), sysconf(_SC_PAGESIZE));
  linea("Memoria disponible antes de que iOS cierre la app: %.0f MB\n", mb(disponible()));
  struct rlimit rl;
  if (getrlimit(RLIMIT_AS, &rl) == 0)
    linea("RLIMIT_AS: %s\n", rl.rlim_cur == RLIM_INFINITY ? "sin limite" : "con limite");

  // 1. La zona compartida: shm_open como en macOS (memory_posix.cpp) o un fichero.
  int fd = -1;
  const char* via = NULL;
  char nombre[64];
  snprintf(nombre, sizeof(nombre), "/nfsmwp%d", (int)getpid());
  fd = shm_open(nombre, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd >= 0) {
    shm_unlink(nombre);
    if (ftruncate(fd, (off_t)TAM_COMPARTIDA) == 0) {
      via = "shm_open";
    } else {
      linea("1. shm_open: si; ftruncate de %.0f MB: NO (errno %d %s)\n", mb(TAM_COMPARTIDA), errno,
            strerror(errno));
      close(fd);
      fd = -1;
    }
  } else {
    linea("1. shm_open: NO (errno %d %s)\n", errno, strerror(errno));
  }
  if (fd < 0 && dir_tmp) {
    char ruta[1024];
    snprintf(ruta, sizeof(ruta), "%s/nfsmw_memoria_%d", dir_tmp, (int)getpid());
    fd = open(ruta, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
      unlink(ruta);
      if (ftruncate(fd, (off_t)TAM_COMPARTIDA) == 0) {
        via = "fichero en tmp";
      } else {
        linea("1. fichero en tmp: ftruncate NO (errno %d %s)\n", errno, strerror(errno));
        close(fd);
        fd = -1;
      }
    }
  }
  if (fd < 0) {
    linea("RESULTADO: no hay zona compartida de %.0f MB\n", mb(TAM_COMPARTIDA));
    return 1;
  }
  linea("1. Zona compartida de %.0f MB: si, con %s\n", mb(TAM_COMPARTIDA), via);

  // 2. La reserva. Si no cabe, cuanto cabe.
  uint8_t* base = mmap(NULL, TAM_RESERVA, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (base == MAP_FAILED) {
    linea("2. Reserva de %.0f MB: NO (errno %d %s)\n", mb(TAM_RESERVA), errno, strerror(errno));
    for (uint64_t prueba = TAM_RESERVA; prueba >= 0x10000000ull; prueba -= 0x10000000ull) {
      void* p = mmap(NULL, prueba, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
      if (p != MAP_FAILED) {
        linea("   la mayor que cabe (en pasos de 256 MB): %.0f MB\n", mb(prueba));
        munmap(p, prueba);
        break;
      }
    }
    linea("RESULTADO: NO cabe la reserva del juego. Hace falta extended-virtual-addressing\n");
    close(fd);
    return 1;
  }
  linea("2. Reserva de %.0f MB: si, en %p\n", mb(TAM_RESERVA), (void*)base);

  // 3. Dos vistas en espejo como en MapViewsMac: 0xA0000000 y 0x100000000 apuntan las dos al
  // desplazamiento 0x100000000 de la zona compartida.
  void* v = mmap(base + 0xA0000000ull, 0x20000000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
                 0x100000000ll);
  void* f = mmap(base + 0x100000000ull, 0x20000000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
                 0x100000000ll);
  if (v == MAP_FAILED || f == MAP_FAILED) {
    linea("3. Vistas fijas: NO (virtual %s, fisica %s; errno %d %s)\n", v == MAP_FAILED ? "NO" : "si",
          f == MAP_FAILED ? "NO" : "si", errno, strerror(errno));
    bien = 0;
  } else {
    base[0xA0000000ull + 0x1000] = 0x5A;
    int espejo = base[0x100000000ull + 0x1000] == 0x5A;
    linea("3. Vistas fijas: si; espejo (escribir en una, leer en la otra): %s\n", espejo ? "si" : "NO");
    bien &= espejo;

    // 4. Usar memoria de verdad: 256 MB, a ver cuanto baja lo disponible.
    uint64_t antes = disponible();
    for (uint64_t o = 0; o < 0x10000000ull; o += 4096) base[0xA0000000ull + 0x2000000 + o] = 1;
    linea("4. Tocar 256 MB: disponible %.0f -> %.0f MB\n", mb(antes), mb(disponible()));
  }

  // 5. Fallos de pagina capturados (mmio_handler del SDK).
  g_tam_pagina = (size_t)sysconf(_SC_PAGESIZE);
  g_pagina = mmap(NULL, g_tam_pagina, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
  struct sigaction sa, viejo_segv, viejo_bus;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = manejador;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, &viejo_segv);
  sigaction(SIGBUS, &sa, &viejo_bus);
  g_pagina[0] = 7;
  linea("5. Fallo de pagina capturado y arreglado: %s (%d fallos, valor %d)\n",
        g_fallos > 0 && g_pagina[0] == 7 ? "si" : "NO", (int)g_fallos, (int)g_pagina[0]);
  bien &= g_fallos > 0;
  sigaction(SIGSEGV, &viejo_segv, NULL);
  sigaction(SIGBUS, &viejo_bus, NULL);
  munmap((void*)g_pagina, g_tam_pagina);

  munmap(base, TAM_RESERVA);
  close(fd);
  linea("RESULTADO: %s\n", bien ? "la memoria del juego cabe como en Android" : "falta algo (ver arriba)");
  return bien ? 0 : 1;
}
