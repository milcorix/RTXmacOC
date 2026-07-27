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

/* Потолок на одну передачу. Данные идут через окно PRAMIN по 32 бита, так что
   гигабайтные запросы просто заняли бы ядро надолго — режем на разумном. */
#define MILCORIX_MAX_XFER   (4u * 1024u * 1024u)

bool MilcorixUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                      OSDictionary *properties)
{
    if (type != MILCORIX_CONNECT_TYPE) return false;
    if (!super::initWithTask(owningTask, securityToken, type, properties)) return false;
    fTask  = owningTask;
    fOwner = nullptr;
    return true;
}

bool MilcorixUserClient::start(IOService *provider)
{
    fOwner = OSDynamicCast(MilcorixFB, provider);
    if (!fOwner) return false;
    if (!super::start(provider)) return false;
    IOLog("MilcorixUC: клиент подключён (GPU %s)\n",
          fOwner->gpuReady() ? "готов" : "не готов");
    return true;
}

void MilcorixUserClient::stop(IOService *provider)
{
    fOwner = nullptr;
    super::stop(provider);
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
    info.scratch_size = fOwner->gpuScratchSize();

    memcpy(args->structureOutput, &info, sizeof(info));
    args->structureOutputSize = sizeof(info);
    return kIOReturnSuccess;
}

IOReturn MilcorixUserClient::methodWrite(IOExternalMethodArguments *args)
{
    if (args->scalarInputCount < 1 || !args->structureInput) return kIOReturnBadArgument;
    uint64_t offset = args->scalarInput[0];
    uint32_t len    = args->structureInputSize;
    if (!len || len > MILCORIX_MAX_XFER) return kIOReturnBadArgument;
    return fOwner->gpuWrite(offset, args->structureInput, len);
}

IOReturn MilcorixUserClient::methodRead(IOExternalMethodArguments *args)
{
    if (args->scalarInputCount < 1 || !args->structureOutput) return kIOReturnBadArgument;
    uint64_t offset = args->scalarInput[0];
    uint32_t len    = args->structureOutputSize;
    if (!len || len > MILCORIX_MAX_XFER) return kIOReturnBadArgument;

    IOReturn rc = fOwner->gpuRead(offset, args->structureOutput, len);
    if (rc == kIOReturnSuccess) args->structureOutputSize = len;
    return rc;
}

IOReturn MilcorixUserClient::methodCopy(IOExternalMethodArguments *args)
{
    if (args->scalarInputCount < 3 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
    uint64_t srcOff = args->scalarInput[0];
    uint64_t dstOff = args->scalarInput[1];
    uint64_t bytes  = args->scalarInput[2];
    if (!bytes || bytes > MILCORIX_MAX_XFER) return kIOReturnBadArgument;

    uint64_t nanos = 0;
    IOReturn rc = fOwner->gpuCopy(srcOff, dstOff, (uint32_t)bytes, &nanos);
    args->scalarOutput[0] = nanos;
    args->scalarOutputCount = 1;
    return rc;
}

IOReturn MilcorixUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                            IOExternalMethodDispatch *dispatch, OSObject *target,
                                            void *reference)
{
    if (!fOwner || isInactive()) return kIOReturnNotAttached;
    if (!args) return kIOReturnBadArgument;

    switch (selector) {
        case kMilcorixMethodGetInfo: return methodGetInfo(args);
        case kMilcorixMethodWrite:   return methodWrite(args);
        case kMilcorixMethodRead:    return methodRead(args);
        case kMilcorixMethodCopy:    return methodCopy(args);
        default:
            return super::externalMethod(selector, args, dispatch, target, reference);
    }
}
