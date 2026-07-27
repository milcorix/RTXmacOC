/*
 * MilcorixUserClient.h — граница между обычной программой и видеокартой.
 *
 * Это и есть слой 6A: не Metal и не попытка притвориться Metal'ом, а свой
 * интерфейс поверх канала, который уже исполняет команды на железе. Приложение
 * кладёт данные в память GPU, просит движок их обработать и забирает результат.
 *
 * Почему свой интерфейс, а не Apple'овский: путь через Metal требует, чтобы kext
 * был полноценным акселератором IOAcceleratorFamily2 с недокументированным ABI,
 * плюс компилятор AIR, которого нет ни у кого. Свой user-client не требует
 * ничего из этого и работает уже сегодня — см. docs/accel-plan.md.
 *
 * Протокол намеренно узкий: четыре операции, никакого разделяемого состояния
 * между клиентами, все смещения проверяются по границам области. Расширять его
 * будем по мере появления настоящих операций (вычислительные ядра, декод видео).
 */
#ifndef MILCORIX_USER_CLIENT_H
#define MILCORIX_USER_CLIENT_H

#include <IOKit/IOUserClient.h>

class MilcorixFB;

/* Тип соединения. IOFramebuffer занимает свои типы под WindowServer, поэтому
   берём заведомо чужой и отдаём его только своему клиенту. */
#define MILCORIX_CONNECT_TYPE   0x4D4C4358u   /* 'MLCX' */

/* Селекторы внешних методов. */
enum {
    kMilcorixMethodGetInfo = 0,   /* сведения о контексте GPU */
    kMilcorixMethodWrite   = 1,   /* хост → память GPU */
    kMilcorixMethodRead    = 2,   /* память GPU → хост */
    kMilcorixMethodCopy    = 3,   /* копирование силами GPU */
    kMilcorixMethodCount
};

/* Ответ kMilcorixMethodGetInfo. Раскладка общая с userspace-утилитой. */
typedef struct {
    uint32_t ready;          /* 1 — канал жив, команды принимаются */
    uint32_t channel;        /* хэндл канала GPFIFO */
    uint32_t copy_engine;    /* хэндл объекта копирования */
    uint32_t reserved;
    uint64_t scratch_size;   /* сколько памяти GPU доступно под данные */
} MilcorixGpuInfo;

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
    MilcorixFB *fOwner;
    task_t      fTask;
    bool        fCounted;   /* мы заняли слот клиента слоя 6 */

    IOReturn methodGetInfo(IOExternalMethodArguments *args);
    IOReturn methodWrite(IOExternalMethodArguments *args);
    IOReturn methodRead(IOExternalMethodArguments *args);
    IOReturn methodCopy(IOExternalMethodArguments *args);
};

#endif /* MILCORIX_USER_CLIENT_H */
