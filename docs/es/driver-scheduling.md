# Planificación con desalojo de controladores Windows

El objeto opcional `scheduling` activa la planificación determinista con desalojo en CPU0 para controladores Windows x64 con Unicorn, KVM o WHP. Si se omite, se conserva la planificación cooperativa y el tiempo virtual avanza solo en reposo. Ambos campos son enteros positivos cuyo producto cabe en `uint64_t`. Un objeto vacío selecciona los valores siguientes.

```json
{
  "scheduling": {
    "quantum_instructions": 1024,
    "instruction_time_100ns": 1
  }
}
```

`quantum_instructions` limita los intentos admitidos de instrucciones de máquina de un hilo. `instruction_time_100ns` asigna tiempo virtual por intento; no estima la velocidad del hardware. Las API del modelo y las transacciones de instrucciones terminan antes de cambiar de hilo. Los eventos no reinician el cuanto restante. El informe conserva la política en `configuration.scheduling`.

`KeSetPriorityThread` y `KeQueryPriorityThread` acceden a la prioridad de ejecución en `PASSIVE_LEVEL`. Se acepta 1..31 y se devuelve la prioridad anterior; cero está reservado. El perfil determinista inicia cada hilo en 8. Se elige el hilo preparado de mayor prioridad; los empates rotan por orden de disponibilidad y cuanto. Un despertar o cambio de prioridad superior interrumpe antes de la siguiente instrucción invitada por debajo de DISPATCH_LEVEL y conserva el cuanto restante. Los callbacks anidados y SEH comparten la prioridad del hilo. Un objeto de hilo del sistema terminado pero referenciado sigue siendo consultable hasta su retirada; no se permite modificarlo durante o tras la terminación. Los objetos prestados de callback caducan con su hilo, evitando heredar prioridades al reutilizar la pila.

El mutex del núcleo pertenece al hilo lógico a través de callbacks anidados y SEH. `KeWaitForSingleObject` conserva esa identidad para una adquisición diferida; el propietario puede ejecutar `KeReleaseMutex` desde cualquiera de sus pilas. Las APC normales siguen desactivadas hasta la última liberación recursiva, y el retorno exterior rechaza mutex que sigan adquiridos.

`KeWaitForMultipleObjects` admite `WaitAll` y `WaitAny` sobre 1..64 eventos, temporizadores, semáforos, mutex inicializados o hilos del sistema referenciados distintos, en `KernelMode` no alertable con motivo `Executive`. `WaitAll` confirma todas las adquisiciones juntas; `WaitAny` devuelve el menor índice listo y consume solo ese objeto. Más de tres objetos requieren almacenamiento `KWAIT_BLOCK` escribible y no paginado. La espera diferida captura el arreglo y conserva todos los objetos y bloques del llamador hasta éxito o vencimiento. El sondeo con plazo cero admite DISPATCH_LEVEL; la espera bloqueante conserva un IRQL hasta APC_LEVEL. Duplicados, esperas alertables/de usuario y abandono de mutex siguen excluidos. Cada espera diferida tiene un identificador que nunca se reutiliza y un estado capturado inmutable; consultar una espera finalizada o alterada falla antes de consumir señales o liberar referencias.

Las continuaciones listas, nuevos trabajadores e hilos del sistema comparten el orden de preparación. Las llamadas anidadas y SEH comparten el cuanto de su hilo. El cambio conserva todo el contexto CPU, la identidad del hilo, el estado APC, el IRQL efectivo y los mapeos del proceso. PASSIVE/APC permite desalojo; DISPATCH y superiores bloquean el cambio de hilo. Las regiones críticas y protegidas desactivan APC, pero no impiden el desalojo.

Las instrucciones hacen avanzar temporizadores y DMA por plazos cronológicos. La cancelación espera el bloqueo cancel disponible y, para WDM, el retorno de dispatch. Las finalizaciones paginables del proveedor, la política de energía y PoFx esperan un límite PASSIVE; se conservan los plazos y se registra la hora real de servicio. Los callbacks independientes del reloj conservan una propiedad distinta de los callbacks PoFx bloqueantes del hilo original. El resultado de una espera queda fijado al vencer su plazo, antes de un reinicio de temporizador o una señal posterior, incluso si una instrucción cruza ambos plazos.

No se implementan clases y aumentos dinámicos de prioridad Windows, CPU paralelas del modelo OS, anidamiento arbitrario de interrupciones, entrega APC ni hilos Windows ring3. La ejecución CPU paralela es una capacidad separada. Controladores originales compilados verifican cuantos cortos, estado flotante/GS, esperas temporizadas, adjuntos de proceso intercalados, cancelación y continuaciones PoFx. La evidencia nativa ARM64 se mantiene por separado.
