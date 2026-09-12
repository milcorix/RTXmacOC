/*
 * MilcorixUserClient.h — граница между обычной программой и видеокартой.
 *
 * Приложение выделяет VRAM-ресурсы, передаёт данные и ожидает GPU-копии.
 * ABI описан в MilcorixABI.h. Это этап проверки памяти и submission на пути
 * к полноценному ускорителю (docs/DRIVER-PLAN.md); работа на macOS требует
 * отдельного аппаратного лога. Пока доступен один клиент и синхронные команды.
 */
#ifndef MILCORIX_USER_CLIENT_H
#define MILCORIX_USER_CLIENT_H

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "MilcorixABI.h"
extern "C" {
#include "../gsp/vram.h"
}

class MilcorixFB;

class MilcorixUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(MilcorixUserClient);

public:
    virtual bool     initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                  OSDictionary *properties) override;
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;
    virtual void     free(void) override;
    virtual IOReturn clientClose(void) override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference) override;

private:
    /* Ссылка на владельца УДЕРЖИВАЕТСЯ (retain) на всё время жизни клиента.
       Иначе stop() у провайдера обнулял бы указатель под уже выполняющимся
       внешним методом: проверка и разыменование разнесены во времени, а метод
       идёт на потоке вызывающего. Классический use-after-free. */
    MilcorixFB *fOwner = nullptr;
    task_t      fTask;
    bool        fCounted = false;   /* мы заняли слот клиента слоя 6 */
    IOLock     *fResourceLock = nullptr;
    nv_vram_pool fPool;     /* адреса здесь — смещения в scratch владельца */
    bool        fManaged;

    IOReturn methodGetInfo(IOExternalMethodArguments *args);
    IOReturn methodWrite(IOExternalMethodArguments *args, bool resource = false);
    IOReturn methodRead(IOExternalMethodArguments *args, bool resource = false);
    IOReturn methodCopy(IOExternalMethodArguments *args, bool resource = false);
    IOReturn methodMemoryInfo(IOExternalMethodArguments *args);
    IOReturn methodAlloc(IOExternalMethodArguments *args);
    IOReturn methodFree(IOExternalMethodArguments *args);
};

#endif /* MILCORIX_USER_CLIENT_H */
