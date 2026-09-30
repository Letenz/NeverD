**Langues** : [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Index de la documentation](README.md)

# Émulation de processus invités

`neverd emulate` exécute une image sous un profil explicite d’OS invité. Le transport CPU, l’analyse d’image, l’entrée du processus et les services OS ont des propriétaires distincts. Activez `NEVERD_ENABLE_CPU_EMULATION=ON` ; l’émulation des pilotes l’inclut aussi.

Le premier profil `linux-elf64-v1` exécute des ELF `ET_EXEC` x64/AArch64 et des PIE statiques `ET_DYN` auto-relocatifs à CPL3 ou EL0. Il charge de vrais segments ELF, construit la pile initiale, reprend par quanta et traite les requêtes explicites d’appels système Linux. C’est un modèle de processus autonome, pas une distribution Linux complète ni une promesse d’exécuter n’importe quel binaire libc. Liaison dynamique, signaux, threads, systèmes de fichiers et services non pris en charge échouent explicitement. Le profil x64 admet quelques formes SSE/SSE2 bornées ; AArch64 reste entier. Windows, Android, Darwin et les autres charges noyau restent séparés.

## CLI et SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Les hôtes Linux compatibles choisissent KVM et les hôtes Windows compatibles WHP ; les autres combinaisons d’ISA hôte/invité utilisent Unicorn. Un backend choisi mais indisponible entraîne une erreur, sans bascule silencieuse. L’ELF conserve le modèle de processus Linux même sous Windows. Voir [exécution CPU](cpu-execution.md) pour l’inventaire d’instructions et ses limites.

Le CLI émet un seul rapport JSON. Code de sortie 0 pour un état invité nul, 2 pour un autre état, 3 pour une exécution incomplète (défauts et limites inclus), 1 pour une configuration/API invalide. L’état réel figure dans `exit_status`. L’entrée C additive [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) reçoit une session, un chemin non vide, un profil explicite et un JSON d’options facultatif. Libérez le résultat avec `neverd_free_string` ; NULL indique un échec de configuration détaillé par `neverd_last_error`. Un défaut invité ou un arrêt sur ressource renvoie un rapport. L’image d’analyse de la session n’est ni requise ni modifiée.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## Options et résultats

Les options sont un objet JSON de 64 KiB maximum. Champs inconnus/null, types incorrects, NUL intégrés aux chaînes et limites non positives sont rejetés.

| Option | Défaut | Contrat |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` ou `whp` |
| `arguments` | Nom du fichier d’entrée | argv complet, argv[0] inclus ; vide = défaut |
| `environment` | `[]` | Chaînes invité explicites ; l’environnement hôte n’est jamais hérité |
| `instruction_limit` | 100000 | Tentatives d’instruction admises et partagées |
| `event_limit` | 10000 | Événements syscall, facturés avant le service OS |
| `timeout_microseconds` | 5000000 | Deadline monotone démarrant après le setup du processus |
| `memory_limit` | 67108864 | Budget mémoire physique/mappé |
| `stack_size` | 1048576 | Pile alignée sur page dans le budget |
| `output_limit` | 1048576 | Total des octets stdout/stderr capturés |
| `instruction_quantum` | 1024 | Intervalle d’admission avant cession à la runtime |

`schema_version` vaut 1. Le rapport inclut profil, architecture, backend sélectionné et motif, `stop_reason`, `exit_status` nullable, diagnostic, PC d’entrée/courant, compteurs, enregistrements de services et dernière sortie CPU typée. Adresses, numéros syscall, registres d’arguments et bits de retour sont des chaînes hexadécimales **sans** `0x` ; `stdout_hex`/`stderr_hex` préservent NUL et UTF-8 invalide. Un résultat syscall null signifie aucun retour modélisé (exit ou requête non prise en charge, par exemple), et non un zéro réussi.

## Sémantique du profil Linux

La politique OS réutilise les en-têtes de programme déjà décodés par le chargeur ELF. Elle vérifie tags ABI, alignement des segments, tables d’en-têtes mappées et limites d’adresses utilisateur. Un plan générique vérifie étendues, permissions, chevauchements et budget avant allocation, et ne publie qu’un espace privé entièrement préparé. Il conserve préfixes/restes des pages de fichier, met BSS à zéro, respecte les droits de segment et réserve des pages de garde de pile. Les dispositions à pages chevauchantes et en-têtes contradictoires sont rejetés sans supposition.

La pile initiale contient argc/argv/envp/auxv alignés, PHDR/PHENT/PHNUM, entry, taille de page et identités. PID/TID/UID/GID modélisés valent 1000. `AT_RANDOM` contient les 16 premiers octets du SHA-256 d’entrée pour la reproductibilité ; c’est une politique déterministe du modèle, pas une entropie cryptographique. HWCAP/HWCAP2 sont nuls ; aucun vDSO n’est fourni.

Les appels implémentés sont `write`, `exit`, `exit_group`, `getpid` et `gettid`, avec des numéros distincts pour [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) et [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). Le retour d’un SYSCALL x64 applique les clobbers RCX/R11, RAX et le PC suivant. ARM64 utilise x8 pour le numéro et x0 pour le résultat. Les appels inconnus arrêtent l’exécution avec `unsupported_service` ; aucun syscall hôte n’est exécuté.

Les descripteurs 1 et 2 sont des puits d’octets virtuels. `write` valide les pages utilisateur lisibles, renvoie le préfixe lisible si une page suivante est inaccessible et `EFAULT` si aucun octet ne l’est. Un descripteur invalide donne `EBADF` ; une écriture de longueur nulle avec descripteur valide ne lit pas le pointeur. L’atomicité des pipes Linux et les fichiers ne sont pas modélisés. La limite de sortie arrête avant publication d’une écriture trop grande.

## Vérification

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# Dans un build avec bibliothèque partagée/CLI :
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Les tests compilent de vrais fichiers ELF d’entrée en assembleur et C pour les deux ISA. Ils couvrent données/BSS, métadonnées initiales, erreurs syscall, sortie binaire, permissions, écritures partielles, services non pris en charge et budgets maintenus entre quanta. Les backends indisponibles sont explicitement marqués comme ignorés. La suite publique traverse ABI C/CLI et vérifie la cohérence rapport/code de sortie. Compilation croisée et Unicorn ARM64 ne prouvent pas l’exécution ARM64 native KVM/WHP.

## PIE statique, TLS et vérification

Le PIE statique utilise un load bias déterministe d’au moins `0x40000000`, augmenté pour respecter les alignements `PT_LOAD` supérieurs. Segments, PC d’entrée et `AT_PHDR`/`AT_ENTRY` partagent ce biais ; les valeurs originales des en-têtes restent inchangées et `AT_BASE` vaut zéro sans interpréteur. Le mappage utilise les octets du fichier original, jamais les fixups d’analyse ; le guest réalise ses propres relocations et initialisation. Le loader décode `PT_DYNAMIC` depuis des enregistrements bornés du fichier original, indépendamment des sections. Présente, la table doit être lisible, terminée et contenir au plus 4096 entrées. `PT_INTERP` et les tags externes de dépendance/filter/audit sont rejetés ; aucun linker dynamique, résolveur de symboles ou constructeur n’est fourni.

Les modèles TLS statiques `PT_TLS` sont validés comme faits du loader : un seul modèle, étendues fichier/mémoire bornées, alignement congruent et octets initialisés lisibles. Le démarrage guest alloue/initialise les blocs TLS et installe le pointeur de thread ; le modèle Linux n’invente ni TCB ni DTV propre à libc. Cela permet le TLS local-exec généré par compilateur dans les programmes freestanding. TLS dynamique et threads OS restent hors périmètre.

Sur x64, `arch_prctl` prend en charge `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` et `ARCH_GET_GS`. Set accepte une base d’espace utilisateur même non mappée ; les déréférencements ultérieurs vérifient toujours les droits. Une base noyau renvoie `EPERM` invité ; une destination Get invalide renvoie `EFAULT` sans défaut CPU. Les autres opérations échouent explicitement. ARM64 installe `TPIDR_EL0` par `MSR` ; `MRS`, accès mémoire FS/GS et restauration de contexte préservent le pointeur entre quanta et entrées backend. Cela n’implémente pas d’ordonnanceur.

Les fixtures PIE/TLS vérifient blocs indépendants alignés, BSS TLS, valeurs auxv relocalisées et slots RELA originaux nuls avant les relocations du guest. Les tests x64 vérifient les erreurs `arch_prctl` sans perdre la base précédente. Ajouter `NeverDThreadPointerTests` à la commande build/CTest ci-dessus ; les backends absents restent des skips explicites.
