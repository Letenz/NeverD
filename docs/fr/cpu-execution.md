**Langues** : [English](../cpu-execution.md) | [简体中文](../zh-CN/cpu-execution.md) | [繁體中文](../zh-TW/cpu-execution.md) | [日本語](../ja/cpu-execution.md) | [한국어](../ko/cpu-execution.md) | [Français](cpu-execution.md) | [Deutsch](../de/cpu-execution.md) | [Español](../es/cpu-execution.md) | [Italiano](../it/cpu-execution.md) | [Русский](../ru/cpu-execution.md) | [العربية](../ar/cpu-execution.md)

[← Index de la documentation](README.md)

# Exécution CPU et requêtes de capacités

L’exécution CPU est indépendante du système invité, du chargeur d’image et de la convention d’appel. `NEVERD_ENABLE_CPU_EMULATION` permet de la construire seule ; `NEVERD_ENABLE_DRIVER_EMULATION` inclut aussi le modèle de pilotes Windows. Le [guide d’architecture](architecture.md) décrit les responsabilités, le choix du backend et les limites de plateforme.

## Configuration

L’[`ExecutionConfiguration`](../../include/neverd/emulation/ExecutionConfiguration.h) publique est utilisée par la fabrique CPU et le rapport de capacités. Les exigences sont vérifiées avant l’allocation du CPU ou l’attachement d’un espace d’adressage. Les valeurs omises prennent le profil fixe du contrat ; toute valeur explicite non prise en charge échoue.

| Champ JSON | Défaut | Signification |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` ou `whp` |
| `contract` | `software-cpu-v1` | Sémantique d’exécution versionnée |
| `architecture` | `x86_64` | `x86_64` ou `aarch64` |
| `privilege` | Profil du contrat | `flat`, `supervisor` ou `user`, conforme au contrat |
| `virtual_address_bits` | Profil du contrat | Profils vérifiés : 48 bits ; profils plats : espace direct de 64 bits |
| `page_size` | 4096 | Granule de mappage invité ; les autres valeurs sont rejetées |
| `required_features` | `[]` | Noms requis de [`ExecutionConfiguration.def`](../../include/neverd/emulation/ExecutionConfiguration.def) |

`driver-strict` accepte x64 ; `software-cpu-v1`, x64 et ARM64. `checked-x64-v1` et `checked-aarch64-v1` exigent l’architecture indiquée et le privilège superviseur. `checked-user-x64-v1` et `checked-user-aarch64-v1` exécutent l’inventaire borné correspondant au contrat à CPL3 et EL0, avec isolation MMU et sorties explicites de requête de service. Ils prennent en charge Unicorn et KVM/WHP correspondant à l’hôte ; `auto` suit le choix de l’hôte. Les profils plats ne garantissent pas l’isolation architecturale utilisateur/superviseur. Le profil ARM64 vérifié rejette FP/SIMD ; x64 admet les familles bornées ci-dessous. Le x64 superviseur ajoute des transactions MMIO limitées et des lectures de chaînes préparées ; les profils utilisateur rejettent les mappages de périphériques. Tous les profils vérifiés rejettent toujours les E/S de ports et les besoins de CPU parallèle. Seuls les profils utilisateur annoncent `service_traps`.

L’exécution utilisateur exige `UserAccessible` ainsi que le droit approprié `Read`, `Write` ou `Execute` sur **chaque** page mappée. Les mappages existants sont superviseur par défaut ; les alias ont des droits indépendants même lorsqu’ils partagent les mêmes octets physiques. `UserAccessible` seul n’accorde aucun accès. Les opérations hôte fiables et les CPU superviseur utilisent RWX. Exemple :

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

Le contrat fixe le privilège. Restaurer un contexte ou lier un autre espace ne le modifie pas ; les sélecteurs de segment x64 ne peuvent pas l’élever. Un défaut de données récupérable conserve l’instruction et les registres d’origine jusqu’à son traitement par le propriétaire. `canAccess` vérifie exactement les droits demandés ; ajoutez `UserAccessible` pour interroger la visibilité utilisateur. Les tables de pages sont des projections privées au CPU, sans accès à des tables invité modifiables ni API de changement de privilège. Sur ARM64, les pages utilisateur sont aussi non exécutables à EL1.

Les champs inconnus ou nuls, noms/largeurs numériques invalides, fonctionnalités requises en double et combinaisons non prises en charge échouent. L’entrée est limitée à 64 KiB ; les anciennes fabriques CPU et options C des pilotes restent compatibles.

## Interroger sans exécuter de charge

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}' \
  --probe-host
```

Le schéma est version 1. Le rapport sépare `requested_configuration` (avant valeurs de profil), la `configuration` normalisée, les `capabilities` sémantiques statiques, `build` (prise en charge de l’adaptateur et ABI) et `host` (null sans `--probe-host`). La sonde initialise un CPU temporaire sur de la RAM privée ; elle prouve seulement l’initialisation, pas la compatibilité d’une charge ni l’exécution ARM64 native. La disponibilité peut changer et aucun backend indisponible ne bascule silencieusement. Le CLI renvoie 0 pour un rapport valide, même si le backend est indisponible, et 1 en cas de requête/configuration invalide.

## Frontières SDK et C++

[`neverd_cpu_capabilities_json`](../../include/neverd/sdk/NeverDCAPICPU.h) reçoit une session, un JSON de configuration facultatif et `ProbeHost` à 0 ou 1 ; aucune image chargée n’est nécessaire. Libérez le résultat avec `neverd_free_string` ; NULL indique une erreur décrite par `neverd_last_error`. Les builds sans CPU exportent la même fonction et signalent explicitement la désactivation. Les plugins Python utilisent `session.cpu_capabilities(...)`. En C++, `executionCapabilities`, `resolveExecutionConfiguration`, `queryExecutionBackendBuild` et `probeExecutionBackend` sont séparables ; `createExecutionBackend` attache un CPU à un espace existant ou crée RAM et espace par défaut.

## Résultats CPU et budgets

`CPU.runUntilExit(PC, TimeoutMicroseconds)` renvoie un [`ExecutionExit`](../../include/neverd/emulation/ExecutionExit.h) typé. Les erreurs de configuration renvoient `llvm::Error` ; une exécution commencée indique explicitement arrêt, échéance, requête de service, défaut récupérable, défaut/piège invité, opération non prise en charge, erreur de périphérique/backend ou arrêt inexpliqué du moteur. Les défauts CPU/périphérique/backend priment sur arrêt/échéance simultanés, tout en conservant les faits et détails indépendants. Le délai doit être positif et représentable comme durée et échéance absolue ; sinon l’appel échoue avant de modifier le CPU. Chaque invocation doit avoir un budget fini ; zéro ne signifie ni illimité ni échéance immédiate valide. Le contrôle est coopératif, sans borne temps réel stricte. Le résultat ne consomme pas un défaut récupérable : le propriétaire OS doit le prendre et installer un transfert d’exception validé avant reprise. `run`, `fault` et `timedOut` restent disponibles ; les implémentations CPU externes qui ne redéfinissent que `run` rejettent la nouvelle interface typée.

## Requêtes de service

Le profil user-x64 intercepte uniquement l’encodage exact de `SYSCALL` sans préfixe ; user-ARM64 intercepte `SVC #imm16`. `SYSENTER`, `INT`, `HVC`, `BRK` et autres mécanismes restent non pris en charge. L’observateur d’instruction s’exécute en premier. S’il ne stoppe ni n’échoue, le CPU renvoie `ExecutionExitKind::ServiceRequest` **avant** l’instruction ou l’entrée dans le backend, avec le type, le `PC` original, le `NextPC` séquentiel et l’immédiat SVC. Registres, flags, pile et privilège restent inchangés : les clobbers RCX/R11 de SYSCALL ne se sont pas produits et ARM64 n’a pas emprunté de vecteur d’exception. L’immédiat SVC n’est pas un numéro de service universel.

La requête reste en attente et bloque exécution, mutation du CPU, liaison d’espace et capture/restauration de contexte jusqu’à sa consommation unique par le propriétaire OS via `takeServiceRequest()` lorsque le CPU est arrêté. Ce propriétaire décode l’ABI OS, traite le service et choisit explicitement les registres résultat et le prochain PC/transfert d’exception. Un service non pris en charge échoue à cette frontière ; réessayer au PC d’origine produit une nouvelle requête, sans NOP ni succès fabriqué. L’événement de service prime sur arrêt/échéance simultanés, mais les défauts invité/backend lui sont supérieurs. Arrêt CPU, HLT logiciel, échéance ou piège ne prouvent pas le succès de la charge. Le [profil Linux de processus](process-emulation.md) possède son propre modèle OS ; il ne prouve pas la compatibilité Windows, Android ou Darwin. L’exécution native Windows/ARM64 exige encore une validation à l’exécution.

## Extensions x64 et état CPU natif

Le x64 vérifié admet des déplacements et opérations logiques SSE/SSE2 historiques bornés, `MOVLHPS`/`MOVHLPS` et les formes scalaires masquées `CVTTSS2SI`/`CVTTSD2SI`/`SUBSS`/`SUBSD`. MXCSR préserve les statuts sticky, l’arrondi et FTZ ; DAZ et les exceptions non masquées sont rejetés. ARM64 vérifié continue de rejeter FP/SIMD. KVM/WHP synchronisent les 16 registres XMM et MXCSR ; les encodages/opérandes non listés restent exclus.

Le pointeur de thread couvre FS/GS sur x64 et `TPIDR_EL0` sur ARM64 avec les encodages exacts `MRS`/`MSR`. Les transports natifs et instantanés CPU préservent cet état indépendamment de la mémoire ; cela ne crée ni threads OS ni blocs TLS. Le superviseur x64 admet les transactions MMIO scalaires alignées de 1/2/4 octets et un élément MOVS par frontière de reprise. Une lecture de périphérique exige une prévisualisation pure puis un commit au plus une fois. Les profils utilisateur rejettent les mappages de périphériques ; RMW, MMIO large et E/S de ports restent rejetés.

KVM et WHP annulent les entrées natives actives et acquittent l’annulation avant de libérer les ressources. KVM utilise un thread privé et débloque temporairement un signal realtime ; le signal choisi ne doit pas être ignoré pendant l’entrée. Les masques et gestionnaires du caller ne changent pas. Si la progression du guest est incertaine, l’annulation est terminale ; aucune échéance temps réel stricte n’est garantie.
