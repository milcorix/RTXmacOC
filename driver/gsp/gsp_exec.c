/*
 * gsp_exec.c — отправка собственной работы на GPU поверх канала, который поднял
 * слой 4. Это фундамент слоя 6 (вычисления): Metal тут ни при чём, мы просто
 * пользуемся своим же каналом GPFIFO и движком копирования.
 *
 * Механика повторяет проход C слоя 4, уже проверенный на железе:
 *   методы в пушбуфер (через окно PRAMIN) → запись в кольцо GPFIFO →
 *   USERD.GP_PUT → дверной звонок → ожидание релиза семафора.
 *
 * Единственное, что здесь добавлено сверх прохода C, — состояние: номер слота
 * кольца и счётчик payload'ов живут в контексте, поэтому команды можно слать
 * многократно, а не один раз за загрузку.
 */
#include "gsp_bringup.h"
#include "gsp_fifo.h"
#include "gmmu.h"

int nv_gsp_gpu_copy(const nv_mmio_t *io, uint64_t *win_base, nv_gsp_gpu_ctx_t *gpu,
                    uint64_t src_va, uint64_t dst_va, uint32_t bytes,
                    uint32_t timeout_ms)
{
    if (!io || !win_base || !gpu || !gpu->ok || !bytes) return -1;
    if (!gpu->ring_entries) return -1;

    /* Payload обязан отличаться от предыдущего, иначе «уже освобождённый»
       семафор будет неотличим от свежего и ожидание завершится мгновенно. */
    uint32_t payload = 0xC0DE0000u | ((++gpu->seq) & 0xffffu);

    /* SET_OBJECT нужен один раз на канал. */
    uint32_t set_obj = gpu->object_bound ? 0u : gpu->class_engine_id;

    uint32_t pb[32];
    uint32_t nd = nv_gsp_fifo_build_ce_copy(pb, set_obj, src_va, dst_va, bytes,
                                            gpu->sem_va, payload);
    if (!nd) return -1;

    /* Семафор в исходное состояние — до постановки работы в очередь. */
    nv_pramin_wr32(io, win_base, gpu->sem_phys, 0u);

    /* Тело пушбуфера. */
    for (uint32_t i = 0; i < nd; i++)
        nv_pramin_wr32(io, win_base, gpu->pb_phys + i * 4u, pb[i]);

    /* Запись кольца в текущем слоте. */
    uint32_t slot = gpu->gp_put % gpu->ring_entries;
    uint32_t e0 = 0, e1 = 0;
    nv_gsp_fifo_gpfifo_entry(gpu->pb_va, nd, &e0, &e1);
    nv_pramin_wr32(io, win_base, gpu->ring_phys + slot * 8u + 0u, e0);
    nv_pramin_wr32(io, win_base, gpu->ring_phys + slot * 8u + 4u, e1);

    /* Сдвинуть GP_PUT и позвонить в дверной звонок. */
    gpu->gp_put = (gpu->gp_put + 1u) % gpu->ring_entries;
    nv_pramin_wr32(io, win_base, gpu->userd_phys + NV_USERD_GP_PUT_OFF, gpu->gp_put);
    io->wr(io->ctx, NV_VFN_DOORBELL_ADDR, (gpu->runlist << 16) | gpu->chid);

    /* Ожидание завершения — строго по семафору. Шаг опроса мелкий: копия
       занимает единицы микросекунд, и шаг в миллисекунду не только тратил бы
       время впустую, но и делал бы измеренную длительность бессмысленной —
       наружу уходила бы цифра, заниженная на два порядка. */
    uint32_t ticks = (timeout_ms ? timeout_ms : 2000u) * 50u;   /* по 20 мкс */
    for (uint32_t i = 0; i < ticks; i++) {
        if (nv_pramin_rd32(io, win_base, gpu->sem_phys) == payload) {
            if (set_obj) gpu->object_bound = 1;
            return 0;
        }
        io->udelay(io->ctx, 20);
    }
    nv_log(io, "GPU-копия: таймаут (%u мс), семафор=0x%08x ждали 0x%08x — "
               "канал считается потерянным до следующего bring-up'а\n",
           timeout_ms ? timeout_ms : 2000u,
           nv_pramin_rd32(io, win_base, gpu->sem_phys), payload);
    /* Состояние канала после таймаута неизвестно: GP_PUT сдвинут, запись кольца
       осталась. Продолжать слать в него — значит копить неисполненные записи и
       молча упереться в переполнение кольца. Помечаем контекст негодным, чтобы
       наружу шло честное «не готов», а не «просто ещё один таймаут». */
    gpu->ok = 0;
    return -1;
}
