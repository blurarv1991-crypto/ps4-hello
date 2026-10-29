#include <sys/stat.h>
#include <stdint.h>
#include <stddef.h>

// Función de libkernel para proteger memoria en PS4
extern "C" int sceKernelMprotect(const void *addr, size_t len, int prot);

extern "C" {
    // Array estático de 32 MiB alineado a 16 KiB (0x4000)
    alignas(16384) static uint8_t s_emusuni_jit_buffer[32 * 1024 * 1024];
    static bool s_jit_buffer_ready = false;

    void* emusuni_jit_arena(size_t size, const char* name) {
        (void)name;
        if (size > sizeof(s_emusuni_jit_buffer)) {
            return nullptr;
        }

        // Interruptor: solo se activa el JIT si existe el archivo jit.txt
        // Si no existe, devuelve nullptr y SwanStation usa el Cached Interpreter
        struct stat st;
        bool jit_enabled = (stat("/data/swan/jit.txt", &st) == 0) ||
                           (stat("/mnt/usb0/jit.txt", &st) == 0) ||
                           (stat("jit.txt", &st) == 0);

        if (!jit_enabled) {
            return nullptr;
        }

        // Hacemos el buffer ejecutable una sola vez con el kernel de PS4
        if (!s_jit_buffer_ready) {
            // PROT_READ (1) | PROT_WRITE (2) | PROT_EXEC (4) = 7
            int ret = sceKernelMprotect(s_emusuni_jit_buffer, sizeof(s_emusuni_jit_buffer), 7);
            if (ret != 0) {
                return nullptr;
            }
            s_jit_buffer_ready = true;
        }

        return s_emusuni_jit_buffer;
    }
}