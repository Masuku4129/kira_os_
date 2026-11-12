#include "syscall_services.h"
#include "syscall_numbers.h"
#include "audio.h"

int syscall_service_audio(uint64_t nr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t* result) {
    if (!result) return 0;
    switch (nr) {
    case SYS_SND_OPEN: case SYS_SND_WRITE: case SYS_SND_CLOSE:
        *result = (uint64_t)audio_sys((unsigned)(nr - SYS_SND_OPEN), a1, a2, a3);
        return 1;
    case SYS_SND_VOLUME:
        *result = (uint64_t)audio_sys(3, a1, a2, 0);
        return 1;
    case SYS_SND_RECORD:
        *result = (uint64_t)audio_record_sys(a1, a2, a3);
        return 1;
    default:
        return 0;
    }
}
