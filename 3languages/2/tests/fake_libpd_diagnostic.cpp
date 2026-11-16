#include <cstring>

// Minimal test double for the libpd symbols consumed by PdBackend.
// It deliberately emits fragmented callbacks like libpd's print hook.
static void (*printHook)(const char*) = nullptr;
extern "C" {
void libpd_set_printhook(void (*fn)(const char*)) { printHook = fn; }
int libpd_init() { return 0; }
int libpd_blocksize() { return 64; }
int libpd_init_audio(int, int, int) { return 0; }
void* libpd_openfile(const char*, const char*) {
    if (printHook) {
        printHook("verbose(0): ");
        printHook("oc~");
        printHook(" 440\n");
        printHook("error: ");
        printHook("... couldn't create\n");
    }
    return reinterpret_cast<void*>(1);
}
int libpd_getdollarzero(void*) { return 1001; }
void libpd_closefile(void*) {}
int libpd_process_float(int, const float*, float* out) {
    std::memset(out, 0, sizeof(float) * 128);
    return 0;
}
int libpd_bang(const char*) { return 0; }
int libpd_float(const char*, float) { return 0; }
int libpd_start_message(int) { return 0; }
void libpd_add_float(float) {}
int libpd_finish_message(const char*, const char*) { return 0; }
}
