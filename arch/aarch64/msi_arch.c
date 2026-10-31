/* ARM64: прерывания сообщениями идут через GICv2m (кадр MSI у GICv2) или ITS (GICv3) - их перенос - следующий шаг
   плана (docs/PROGRESS.md). Пока вектор не выдаётся: драйверы честно остаются на опросе со сном по тику, как раньше. */
#include "../../msi.h"

int msi_alloc(msi_fn fn, void* ctx, uint64_t* addr, uint32_t* data) {
    (void)fn; (void)ctx; (void)addr; (void)data;
    return -1;
}
