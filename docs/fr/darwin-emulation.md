**Langues**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: adbd03b9316f8ea1f40f8be5f361cd5014048848e2ba6cffebb64faa36f86156 -->

[← Index de la documentation](README.md)

# Environnements de processus invités macOS et iOS

`lib/emulation/os/darwin/` fournit des processus Mach-O autonomes et bornés, distincts du transport CPU hôte. Activez `NEVERD_ENABLE_CPU_EMULATION` ; l’émulation des pilotes Windows n’est pas nécessaire. `macos/` et `ios/` définissent des profils explicites.

| Profil | Plateforme Mach-O | ISA invitées | Pages OS |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, ARM64 de base | 4 KiB x64 ; 16 KiB ARM64 |
| `ios-macho64-v1` | appareil iOS | ARM64 de base | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, ARM64 de base | 4 KiB x64 ; 16 KiB ARM64 |

Un binaire appareil n’est pas une image simulateur. La plateforme invitée ne se déduit pas de l’hôte. [HVF](macos-hvf.md) exécute les ISA correspondantes sous macOS ; `auto` utilise Unicorn pour les autres ISA. La granularité CPU reste de 4 KiB. Les [API C, Python et CLI](process-emulation.md) partagent options, limites et rapports.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Image et démarrage

Le chargeur `MachOExecutionImage` préserve les octets originaux, sans correctifs de relocation issus de l’analyse. Il accepte uniquement un Mach-O thin little-endian `MH_EXECUTE` avec plateforme et entrée non ambiguës. Une image universelle nécessite l’extraction explicite d’une tranche.

Le fichier complet, métadonnées et octets de fin inclus, doit respecter `memory_limit` avant analyse ou copie. Un instantané privé borné est lu depuis un fichier ordinaire ; chemins avec NUL, lectures courtes et changements de taille sont refusés. Aucun mapping de fichier vivant n’est conservé. Le budget de fichier et le budget de mémoire invitée sont deux plafonds séparés de même valeur ; les E/S hôte n’ont pas de délai temps réel garanti.

Les segments conservent permissions courantes/maximales et zones à zéro. `__PAGEZERO` réserve des adresses sans allouer son étendue. Les plages fichier/VM, alignements OS, chevauchements arrondis, propriétaire de l’en-tête, entrée exécutable et budget sont vérifiés. Le segment d’en-tête doit être lisible et exécutable ; pages de garde et porte de retour privée restent réservées. La dernière page de fichier garde les octets jusqu’à la limite de page ou EOF ; les pages VM complètes suivantes sont mises à zéro, selon le [chargeur XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` reçoit `argc`, `argv`, `envp` et le vecteur apple comme quatre arguments entiers ; le retour fournit les huit bits bas du statut de sortie. `/usr/lib/dyld` est accepté seulement pour ce transfert d’entrée sans imports : le dyld hôte n’est pas exécuté. Un `stacksize` non nul est refusé ; `stack_size` fourni par l’appelant fixe le budget.

`LC_UNIXTHREAD` exige un seul enregistrement complet de registres généraux 64 bits natifs, avec uniquement PC renseigné. La pile contient argc, argv/envp terminés et un vecteur apple terminé avec `executable_path=<input filename>`. SP/flags personnalisés, autres registres, saveurs supplémentaires ou entrées conflictuelles sont refusés. Aucun environnement hôte ni vecteur auxiliaire Linux n’est hérité. Voir l’[architecture dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Dylibs externes, imports, rebases/chained fixups, constructeurs/destructeurs, sections TLS, arm64e/PAC, sous-types non pris en charge, chiffrement et commandes non modélisées sont refusés avant exécution. Les PIE sans fixups utilisent leurs adresses préférées, sans ASLR. Les blobs de signature sont des métadonnées, sans modèle AMFI ni politique d’entitlements.

## Services Darwin

ARM64 utilise X16, X0–X5 et `svc #0x80` ; x64 utilise la classe BSD `0x02000000`, RAX et RDI/RSI/RDX/R10/R8/R9. Une réussite efface carry ; une erreur le positionne et renvoie un errno positif. ARM64 efface X1 ; x64 efface RDX en cas de réussite et le conserve en cas d’erreur. Les registres modifiés par SYSCALL sont explicites. Le rapport utilise `result` et `error=true` pour une erreur BSD ; les requêtes sans retour ou non prises en charge n’ont aucun de ces champs. Les règles suivent les entrées XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) et [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), sans incorporation de code Apple.

Services : `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID valent 1000 et PPID vaut 1. Les descripteurs 1 et 2 capturent des octets, y compris NUL/non-UTF8 ; les descripteurs fermés ou en lecture seule donnent EBADF. Une copie partielle conserve les octets déjà lus mais renvoie EFAULT. Une longueur supérieure à `INT_MAX` donne EINVAL avant vérification du descripteur, du pointeur ou du budget, suivant [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

La mémoire accepte les mappings privés anonymes de données : `flags=0x1002`, descripteur -1 et offset zéro. Longueurs et adresses indicatives non fixes sont arrondies vers le haut à la page OS ; une indication occupée cherche plus haut, puis revient au placement par défaut. Le mmap brut historique de longueur zéro renvoie zéro sans allocation ; `MAP_UNIX03` est accepté et refuse la longueur zéro avec EINVAL. Unmap/protect exigent une adresse alignée. NONE/READ/WRITE sont pris en charge, WRITE implique READ. Les pages physiques appartiennent à chaque page OS : un unmap partiel libère son budget, puis les nouvelles pages sont à zéro. Un protect traversant un trou ou dépassant les droits maximaux échoue sans changement partiel. Référence : [services VM XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Les mappings partagés/fixes/JIT ou anonymes exécutables, traps Mach, appels indirects, threads, signaux, fichiers hôte/réseau, dyld, runtimes Objective-C/Swift et Foundation/UIKit sont hors contrat et arrêtent explicitement l’exécution. Ce profil ne constitue pas un OS Apple complet ni l’application iOS Simulator.

## Validation

Les fixtures C originales sont produites par Clang et `ld64.lld`, sans SDK Apple ni binaire propriétaire. Elles couvrent les cinq combinaisons plateforme/ISA, les enregistrements Mach-O malformés, les pages 4/16 KiB et la libération partielle sous budget plein. `NeverDProcessPublicTests` compare C API/CLI ; les variables `NEVERD_TEST_LIBNEVERD` et `NEVERD_TEST_DARWIN_FIXTURES` activent les mêmes cinq combinaisons dans le SDK Python.

## Fichiers et descripteurs explicites

`darwin_files` fournit aux trois profils un catalogue fermé de fichiers en lecture seule. Le champ obligatoire `files` contient un `path` invité absolu canonique et des `bytes_hex` hexadécimaux. `stdin_hex` est un flux fini facultatif : absent signifie inconnu et arrête une lecture non vide, une chaîne vide signifie EOF. Sans catalogue open s’arrête ; un catalogue explicitement vide renvoie ENOENT. Aucun fichier ni flux hôte n’est consulté.

Les services ajoutés sont `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`, avec les entrées nocancel de read/write/open/close/fcntl/pread. O_RDONLY/O_CLOEXEC et F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL sont pris en charge. Chaque open a sa position ; les duplications partagent la position mais gardent leurs propres indicateurs close-on-exec. pread ne déplace pas la position. Fermer ou remplacer 0/1/2 affecte les I/O suivantes ; une sortie dupliquée conserve sa destination et son budget.

Limites : 256 fichiers, 16 MiB cumulés pour chemins/NUL/fichiers/entrée, chemins de moins de 1024 octets et composants de 255 octets maximum. `descriptor_limit` est un plafond exclusif de 3–4096, défaut 256 ; JSON reste limité à 64 KiB. Les options invalides échouent avant chargement. read au-delà de INT_MAX donne EINVAL avant recherche du FD ; EOF ne touche pas la destination, une adresse invalide donne EFAULT. Un tampon partiellement inscriptible arrête avant copie ou changement de position. Les erreurs SET/CUR/END conservent la position. Chemins relatifs, ouverture de répertoires, écriture, ancien stat, seek creux et autres fcntl restent exclus. Un fichier utilisé comme ancêtre donne ENOTDIR. Le même objet est comparé au noyau macOS ; C/CLI/Python couvrent cinq combinaisons invitées, sans preuve sur appareil iOS.

Vérification Release du 2026-10-05 : 381 inscriptions, 177 réussites, 204 ignorées, aucun échec, et 51/51 cas ARM64 HVF obligatoires exécutés. Sept programmes macOS natifs, 35 tests publics C/CLI/rapports, cinq combinaisons Python et 66 tests du vérificateur ont également réussi. Les comptes se recoupent. Les nouveaux services n’ont pas de preuve native Intel HVF/KVM/WHP ; Intel HVF reste non validé et ses Actions suspendues. Le SDK iOS et la comparaison sur appareil manquent.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Mappings privés de fichiers

`mmap` accepte les fichiers réguliers du catalogue avec `MAP_PRIVATE` : `flags=0x2` ou `0x40002` avec `MAP_UNIX03`, à un offset aligné sur une page OS. Même une longueur courte conserve tous les octets du fichier dans la page ; la fin de la dernière page EOF est nulle. Une écriture privée ne change ni le fichier, ni les autres mappings, ni les métadonnées fixes, ni le curseur partagé. Le mapping survit à close et à la réutilisation du FD. Les mappings en lecture seule et PROT_NONE sont initialisés ; `mprotect` peut ensuite autoriser l’écriture.

Un débordement de fin de fichier, une longueur UNIX03 nulle ou un offset UNIX03 non aligné donnent EINVAL avant recherche du FD ; un FD invalide donne EBADF avant contrôle du budget. Le mode historique de longueur nulle vérifie néanmoins le FD. Offsets historiques non alignés, flux, pages de fichiers vides et pages entièrement au-delà d’EOF arrêtent avant allocation. macOS permet ces pages EOF mais leur accès produit SIGBUS : le modèle n’invente ni pages nulles lisibles ni livraison de signal. Les mappings partagés, fixes, exécutables et JIT restent exclus.

`DarwinFiles` résout les FD et les octets, `DarwinMemory` gère placement, droits, budget et annulation. Seul `darwin_files` fournit les données. Le programme commun `file-mapping` vérifie copies privées, close, curseurs, erreurs et réutilisation anonyme ; une comparaison native séparée couvre un offset non nul, la page entière et SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Vérification des mappings privés, 2026-10-05

Release Darwin : 438 identités uniques, 210 succès, 228 omissions, aucun échec ; 57/57 cas ARM64 HVF obligatoires exécutés et cinq combinaisons Unicorn. Neuf programmes macOS natifs, la comparaison de toute une page à offset non nul et SIGBUS dans un enfant isolé ont réussi. Les 36 contrôles API/rapport n’ont aucune omission ; Python couvre cinq combinaisons avec `file-mapping`, et 66 tests d’outils et 38 de provenance passent. Les comptes se recoupent. Preuves : `build-hvf-arm64/darwin-mmap-verified-evidence/`. Aucun nouveau résultat Intel HVF/KVM/WHP ou iOS physique ; Intel HVF Actions reste suspendu.

## Métadonnées explicites des fichiers

Une entrée peut ajouter `metadata` ; tous les champs ci-dessous sont alors obligatoires. Les chaînes décimales préservent toute la largeur ; les nombres JSON restent des entiers exacts dans ±(2^53−1). device est signé sur 32 bits, mode/link_count non signés sur 16, inode non signé sur 64, uid/gid/flags/generation non signés sur 32. size doit égaler le nombre d’octets ; blocks tient sur 64 bits signés, block_size sur 32 bits signés non négatifs. Les temps utilisent des secondes signées sur 64 bits et 0–999999999 nanosecondes. Seuls le type fichier ordinaire et les bits de permission sont admis.

`stat64` (338), `fstat64` (339) et `lstat64` (340) produisent le même enregistrement LP64 de 144 octets sur ARM64/x64. Ils partagent la résolution de open, suivent dup/close, sans allouer de FD ni modifier le curseur. rdev, remplissage et réserves sont nuls. Les observations sont fixes : read ne change pas les temps et mode ne modifie pas l’accès au catalogue. Métadonnées absentes, répertoires/flux, liens symboliques, ancien stat, stat-at et sécurité étendue restent exclus. Les erreurs de chemin/FD précèdent le pointeur de sortie ; une sortie partiellement accessible est refusée avant écriture. Le test natif compare tous les octets d’un fichier réel et les offsets du SDK ; le même programme original vérifie les trois appels.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### Vérification des métadonnées et suite (2026-10-05)

Après stat64 : 409 identités uniques, 193 réussites, 216 omissions, aucun échec ; les 54/54 cas ARM64 HVF obligatoires ont tourné, avec cinq combinaisons Unicorn. Comparaison SDK/enregistrement réel, huit programmes natifs, 36 cas API/rapport sans omission, cinq combinaisons Python et 66 tests des outils réussissent ; les comptes se recouvrent. Chaque cas natif utilise désormais son propre fichier de sortie, supprimant les octets résiduels après une sortie plus courte. Ces ajouts n’ont pas de preuve native Intel HVF/KVM/WHP ou iOS physique.

Suite : mappings partagés et défauts de page EOF, répertoires/chemins relatifs et écritures bornées (pages EOF, durée après close, ordre des erreurs), observations explicites temps/système, services Mach/threads requis, puis dépendances Mach-O, rebases/binds, initialiseurs et TLS. Objective-C/Swift et Foundation/UIKit nécessitent des programmes natifs de référence. iOS physique requiert SDK et appareil ; Intel HVF reste non validé et ses Actions suspendues.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La validation indépendante exige chacun des 57 cas natifs ARM64 ou 38 cas x64, dont `LC_MAIN` et `LC_UNIXTHREAD` sur chaque plateforme. Les cas obligatoires absents/ignorés ou un `ld64.lld` manquant font échouer la validation.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Sur Linux utilisez `kvm`, sur Windows `whp`. Le [workflow Darwin](../../.github/workflows/darwin-native.yml) exécute les deux transports x64 sans Unicorn et autorise une sélection isolée. La [référence noyau](../../.github/workflows/darwin-kernel-reference.yml) compare directement les programmes sur les deux ISA macOS, sans NeverD/LLVM ; `DarwinNativeCases.def` fixe modes, statuts et octets attendus. Seul l’exécutable de référence lie libSystem pour le vrai transfert dyld. Une ISA incorrecte, Rosetta, un dépassement de délai ou une divergence échoue. Cela ne valide pas le noyau d’un appareil iOS.

## Preuves et périmètre restant

Les lignes se recouvrent ; ne les additionnez pas. Résultats au 2026-10-03 :

| Transport | Source | Réussis | Échecs | Ignorés | Charges natives |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

Le [run Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) réconcilie 286 identités CTest et 32 processus avec XML original. Les 234 cas ignorés sont 65 cas Unicorn désactivés, 39 invités ARM64 et 130 autres plateformes hôtes. L’artefact `11267489438` a le SHA-256 vérifié `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. Les résultats [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) ont également été vérifiés indépendamment. La [référence noyau](https://github.com/NeverSight/NeverD/actions/runs/37064795867) a réussi 4/4 programmes sur chacune des deux ISA, avec statut 37, sortie exacte et stderr vide.

C API/CLI avec Unicorn : 138 réussites, 156 cas ignorés, aucun échec. Le SDK Python couvre les cinq combinaisons ; le moteur packagé correspond à 18 rapports CLI ARM64 et ses 186 images Mach-O passent les contrôles de signature. La configuration HVF/Unicorn OFF réussit 38 contrôles, en ignore 231 et ne lie pas Hypervisor.framework. Ce sont des preuves d’intégration, pas des exécutions natives supplémentaires. Le CPU Intel complet reste non vérifié ; voir [HVF](macos-hvf.md) et le [registre détaillé](../darwin-emulation.md#hosted-native-verification-2026-10-03).
