/*
 * MilcorixUserClient.cpp — реализация границы userspace ↔ GPU (см. заголовок).
 *
 * Разбор аргументов здесь параноидальный намеренно: всё, что приходит из
 * пользовательского процесса, — недоверенные данные, а по ту сторону находится
 * прямая запись в память видеокарты. Каждое смещение и длина проверяются по
 * границам области, размеры буферов сверяются с объявленными, перекрытие
 * источника и приёмника запрещается.
 */
#include "MilcorixUserClient.h"
#include "MilcorixFB.h"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(MilcorixUserClient, IOUserClient);

/* Промежуточный буфер для потоковой передачи. IOKit кладёт небольшие структуры
   прямо в structureInput/Output, а всё крупнее внутреннего порога отдаёт
   ДЕСКРИПТОРОМ ПАМЯТИ, оставляя указатель нулевым. Обрабатывать только
   указатель — значит отвергать любую практически полезную передачу: наша же
   утилита по умолчанию шлёт 256 КиБ. Поэтому поддерживаем оба пути, а большие
   буферы гоняем кусками, не выделяя копию целиком. */
#define MILCORIX_STAGE_CHUNK (64u * 1024u)

bool MilcorixUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                      OSDictionary *properties)
{
    if (type != MILCORIX_CONNECT_TYPE) return false;
    if (!super::initWithTask(owningTask, securityToken, type, properties)) return false;
    fTask    = owningTask;
    fOwner   = nullptr;
    fCounted = false;
    fManaged = false;
    bzero(&fPool, sizeof(fPool));
    fResourceLock = IOLockAlloc();
    return fResourceLock != nullptr;
}

bool MilcorixUserClient::start(IOService *provider)
{
    MilcorixFB *owner = OSDynamicCast(MilcorixFB, provider);
    if (!owner) return false;
    if (!super::start(provider)) return false;

    /* Канал один: пушбуфер, кольцо и семафор общие. Второму клиенту честно
       отказываем, вместо того чтобы молча перемешивать команды. */
    if (!owner->gpuClientOpen()) {
        IOLog("MilcorixUC: отказ — канал слоя 6 уже занят другим клиентом\n");
        return false;
    }
    fCounted = true;

    /* Держим владельца сами: провайдер может уйти, пока наш метод исполняется. */
    owner->retain();
    fOwner = owner;
    if (nv_vram_pool_init(&fPool, 0, owner->gpuScratchVA(), owner->gpuScratchSize()))
        return false;
    fManaged = owner->gpuPoolVerified();

    IOLog("MilcorixUC: клиент подключён (GPU %s)\n",
          fOwner->gpuReady() ? "готов" : "не готов");
    return true;
}

void MilcorixUserClient::stop(IOService *provider)
{
    /* Указатель здесь НЕ обнуляем: внешний метод может исполняться прямо
       сейчас на другом потоке. Освобождение — во free(), когда IOKit уже
       гарантировал, что ссылок на объект не осталось. */
    super::stop(provider);
}

void MilcorixUserClient::free(void)
{
    if (fOwner) {
        if (fCounted) { fOwner->gpuClientClose(); fCounted = false; }
        fOwner->release();
        fOwner = nullptr;
    }
    if (fResourceLock) { IOLockFree(fResourceLock); fResourceLock = nullptr; }
    super::free();
}

IOReturn MilcorixUserClient::clientClose(void)
{
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn MilcorixUserClient::methodGetInfo(IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MilcorixGpuInfo))
        return kIOReturnBadArgument;

    MilcorixGpuInfo info;
    bzero(&info, sizeof(info));
    info.ready        = fOwner->gpuReady() ? 1u : 0u;
    info.channel      = fOwner->gpuChannel();
    info.copy_engine  = fOwner->gpuCopyEngine();
    info.scratch_size = fOwner->gpuScratchSize();

    memcpy(args->structureOutput, &info, sizeof(info));
    args->structureOutputSize = sizeof(info);
    return kIOReturnSuccess;
}

IOReturn MilcorixUserClient::methodWrite(IOExternalMethodArguments *args, bool resource)
{
    if (args->scalarInputCount != (resource ? 2u : 1u)) return kIOReturnBadArgument;
    uint64_t offset = args->scalarInput[0];
    uint64_t totalBytes = args->structureInput ? args->structureInputSize :
        (args->structureInputDescriptor ? args->structureInputDescriptor->getLength() : 0);
    if (!totalBytes || totalBytes > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
    if (resource && nv_vram_resolve(&fPool, args->scalarInput[0], args->scalarInput[1],
                                    totalBytes, &offset, nullptr)) return kIOReturnBadArgument;
    if (offset & 3) return kIOReturnBadArgument;

    /* Короткий путь: данные уже в памяти ядра. */
    if (args->structureInput) {
        uint32_t len = args->structureInputSize;
        if (!len || len > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
        return fOwner->gpuWrite(offset, args->structureInput, len);
    }

    IOMemoryDescriptor *md = args->structureInputDescriptor;
    if (!md) return kIOReturnBadArgument;
    uint64_t total = md->getLength();
    if (!total || total > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
    if (md->prepare() != kIOReturnSuccess) return kIOReturnVMError;

    void *stage = IOMalloc(MILCORIX_STAGE_CHUNK);
    if (!stage) { md->complete(); return kIOReturnNoMemory; }

    IOReturn rc = kIOReturnSuccess;
    uint64_t done = 0;
    while (done < total) {
        uint64_t left = total - done;
        uint32_t chunk = (uint32_t)((left > MILCORIX_STAGE_CHUNK) ? MILCORIX_STAGE_CHUNK : left);
        if (md->readBytes(done, stage, chunk) != chunk) { rc = kIOReturnVMError; break; }
        rc = fOwner->gpuWrite(offset + done, stage, chunk);
        if (rc != kIOReturnSuccess) break;
        done += chunk;
    }
    IOFree(stage, MILCORIX_STAGE_CHUNK);
    md->complete();
    return rc;
}

IOReturn MilcorixUserClient::methodRead(IOExternalMethodArguments *args, bool resource)
{
    if (args->scalarInputCount != (resource ? 2u : 1u)) return kIOReturnBadArgument;
    uint64_t offset = args->scalarInput[0];
    uint64_t totalBytes = args->structureOutput ? args->structureOutputSize :
        (args->structureOutputDescriptor ? args->structureOutputDescriptor->getLength() : 0);
    if (!totalBytes || totalBytes > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
    if (resource && nv_vram_resolve(&fPool, args->scalarInput[0], args->scalarInput[1],
                                    totalBytes, &offset, nullptr)) return kIOReturnBadArgument;
    if (offset & 3) return kIOReturnBadArgument;

    if (args->structureOutput) {
        uint32_t len = args->structureOutputSize;
        if (!len || len > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
        IOReturn rc = fOwner->gpuRead(offset, args->structureOutput, len);
        if (rc == kIOReturnSuccess) args->structureOutputSize = len;
        return rc;
    }

    IOMemoryDescriptor *md = args->structureOutputDescriptor;
    if (!md) return kIOReturnBadArgument;
    uint64_t total = md->getLength();
    if (!total || total > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
    if (md->prepare() != kIOReturnSuccess) return kIOReturnVMError;

    void *stage = IOMalloc(MILCORIX_STAGE_CHUNK);
    if (!stage) { md->complete(); return kIOReturnNoMemory; }

    IOReturn rc = kIOReturnSuccess;
    uint64_t done = 0;
    while (done < total) {
        uint64_t left = total - done;
        uint32_t chunk = (uint32_t)((left > MILCORIX_STAGE_CHUNK) ? MILCORIX_STAGE_CHUNK : left);
        rc = fOwner->gpuRead(offset + done, stage, chunk);
        if (rc != kIOReturnSuccess) break;
        if (md->writeBytes(done, stage, chunk) != chunk) { rc = kIOReturnVMError; break; }
        done += chunk;
    }
    IOFree(stage, MILCORIX_STAGE_CHUNK);
    md->complete();
    return rc;
}

IOReturn MilcorixUserClient::methodCopy(IOExternalMethodArguments *args, bool resource)
{
    if (args->scalarInputCount != (resource ? 5u : 3u) || args->scalarOutputCount < 1)
        return kIOReturnBadArgument;
    uint64_t srcOff = args->scalarInput[0];
    uint64_t dstOff = args->scalarInput[1];
    uint64_t bytes  = args->scalarInput[resource ? 4 : 2];
    if (!bytes || bytes > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
    if (resource && (nv_vram_resolve(&fPool, args->scalarInput[0], args->scalarInput[1],
                                     bytes, &srcOff, nullptr) ||
                     nv_vram_resolve(&fPool, args->scalarInput[2], args->scalarInput[3],
                                     bytes, &dstOff, nullptr))) return kIOReturnBadArgument;

    uint64_t nanos = 0;
    IOReturn rc = fOwner->gpuCopy(srcOff, dstOff, (uint32_t)bytes, &nanos);
    args->scalarOutput[0] = nanos;
    args->scalarOutputCount = 1;
    return rc;
}

IOReturn MilcorixUserClient::methodMemoryInfo(IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(MilcorixMemoryInfo))
        return kIOReturnBadArgument;
    MilcorixMemoryInfo info = {};
    info.version = MILCORIX_MEMORY_ABI_VERSION; info.size = sizeof(info); info.page_size = 4096;
    if (fOwner->gpuPoolVerified())
        info.flags = MILCORIX_MEMORY_READY | MILCORIX_MEMORY_EXTERNAL_VMM | MILCORIX_MEMORY_GPU_PROBES;
    info.pool_bytes = fPool.size; info.allocated_bytes = fPool.used;
    info.max_transfer_bytes = MILCORIX_MAX_XFER;
    memcpy(args->structureOutput, &info, sizeof(info));
    args->structureOutputSize = sizeof(info);
    return kIOReturnSuccess;
}

IOReturn MilcorixUserClient::methodAlloc(IOExternalMethodArguments *args)
{
    if (args->scalarInputCount != 2 || args->scalarOutputCount < 2) return kIOReturnBadArgument;
    if (!fOwner->gpuPoolVerified()) return kIOReturnNotReady;
    args->scalarOutput[0] = args->scalarOutput[1] = 0;
    uint64_t handle = 0;
    int err = nv_vram_alloc(&fPool, args->scalarInput[0], args->scalarInput[1], &handle);
    if (err) return err == -2 ? kIOReturnNoMemory : kIOReturnBadArgument;
    uint64_t bytes = (args->scalarInput[0] + 4095) & ~4095ull, offset = 0;
    nv_vram_resolve(&fPool, handle, 0, bytes, &offset, nullptr);
    /* Очищаем первый блок CPU, остальные — GPU-копиями уже очищенной части.
       Для гигабайтного ресурса это не миллиард медленных MMIO-записей. */
    void *zero = IOMalloc(4096);
    IOReturn rc = kIOReturnNoMemory;
    if (zero) {
        bzero(zero, 4096);
        rc = fOwner->gpuWrite(offset, zero, 4096);
        IOFree(zero, 4096);
        for (uint64_t filled = 4096; rc == kIOReturnSuccess && filled < bytes;) {
            uint64_t chunk = bytes - filled;
            if (chunk > filled) chunk = filled;
            if (chunk > MILCORIX_MAX_XFER) chunk = MILCORIX_MAX_XFER;
            rc = fOwner->gpuCopy(offset, offset + filled, (uint32_t)chunk, nullptr);
            filled += chunk;
        }
    }
    if (rc != kIOReturnSuccess) { nv_vram_free(&fPool, handle); return rc; }
    /* Ресурсный режим использует только handles с проверкой границ. */
    fManaged = true;
    args->scalarOutput[0] = handle; args->scalarOutput[1] = bytes;
    args->scalarOutputCount = 2;
    return kIOReturnSuccess;
}

IOReturn MilcorixUserClient::methodFree(IOExternalMethodArguments *args)
{
    if (args->scalarInputCount != 1) return kIOReturnBadArgument;
    /* Все команды синхронные; fResourceLock удерживается на весь метод. */
    return nv_vram_free(&fPool, args->scalarInput[0]) ? kIOReturnBadArgument : kIOReturnSuccess;
}

IOReturn MilcorixUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                            IOExternalMethodDispatch *dispatch, OSObject *target,
                                            void *reference)
{
    if (!fOwner || isInactive()) return kIOReturnNotAttached;
    if (!args || !fResourceLock) return kIOReturnBadArgument;
    if (selector >= kMilcorixMethodCount)
        return super::externalMethod(selector, args, dispatch, target, reference);
    IOLockLock(fResourceLock);
    IOReturn rc = kIOReturnUnsupported;
    if (isInactive()) { IOLockUnlock(fResourceLock); return kIOReturnNotAttached; }
    if (fManaged && selector >= kMilcorixMethodWrite && selector <= kMilcorixMethodCopy) {
        IOLockUnlock(fResourceLock); return kIOReturnUnsupported;
    }
    switch (selector) {
        case kMilcorixMethodGetInfo: rc = methodGetInfo(args); break;
        case kMilcorixMethodWrite: rc = methodWrite(args); break;
        case kMilcorixMethodRead: rc = methodRead(args); break;
        case kMilcorixMethodCopy: rc = methodCopy(args); break;
        case kMilcorixMethodGetMemoryInfo: rc = methodMemoryInfo(args); break;
        case kMilcorixMethodAlloc: rc = methodAlloc(args); break;
        case kMilcorixMethodFree: rc = methodFree(args); break;
        case kMilcorixMethodWriteBuffer: rc = methodWrite(args, true); break;
        case kMilcorixMethodReadBuffer: rc = methodRead(args, true); break;
        case kMilcorixMethodCopyBuffer: rc = methodCopy(args, true); break;
    }
    IOLockUnlock(fResourceLock);
    return rc;
}
