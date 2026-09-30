**Langues** : [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Index de la documentation](README.md)

# Émulation de processus invités

`neverd emulate` exécute une image sous un profil explicite d’OS invité. Le transport CPU, l’analyse d’image, l’entrée du processus et les services OS ont des propriétaires distincts. Activez `NEVERD_ENABLE_CPU_EMULATION=ON` ; l’émulation des pilotes l’inclut aussi.

Le premier profil, `linux-elf64-v1`, exécute des programmes ELF `ET_EXEC` autonomes x64 et AArch64 à CPL3 ou EL0. Il charge les vrais segments ELF, construit la pile initiale, reprend l’exécution par quanta et traite les requêtes explicites d’appels système Linux. C’est un modèle de processus autonome, pas une distribution Linux complète ni une promesse d’exécuter n’importe quel binaire libc. Liaison dynamique, `PT_TLS`, signaux, threads, FP/SIMD et services non pris en charge échouent explicitement. Windows, Android, Darwin et les autres charges noyau restent séparés.

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
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate)Tests$' --output-on-failure
# Dans un build avec bibliothèque partagée/CLI :
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Les tests compilent de vrais fichiers ELF d’entrée en assembleur et C pour les deux ISA. Ils couvrent données/BSS, métadonnées initiales, erreurs syscall, sortie binaire, permissions, écritures partielles, services non pris en charge et budgets maintenus entre quanta. Les backends indisponibles sont explicitement marqués comme ignorés. La suite publique traverse ABI C/CLI et vérifie la cohérence rapport/code de sortie. Compilation croisée et Unicorn ARM64 ne prouvent pas l’exécution ARM64 native KVM/WHP.
