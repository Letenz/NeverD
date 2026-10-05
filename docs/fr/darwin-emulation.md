**Langues**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 813c9673241230afbb295a950aab1e14478b4bd4fe9de2d2f2e27b6fbe34f588 -->

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

La mémoire accepte les mappings privés anonymes de données : `flags=0x1002`, descripteur -1 et offset zéro. Longueurs et adresses indicatives non fixes sont arrondies vers le haut à la page OS ; une indication occupée cherche plus haut, puis revient au placement par défaut. Le mmap brut historique de longueur zéro renvoie zéro sans allocation ; `MAP_UNIX03` est exclu. Unmap/protect exigent une adresse alignée. NONE/READ/WRITE sont pris en charge, WRITE implique READ. Les pages physiques appartiennent à chaque page OS : un unmap partiel libère son budget, puis les nouvelles pages sont à zéro. Un protect traversant un trou ou dépassant les droits maximaux échoue sans changement partiel. Référence : [services VM XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Les mappings fichier/partagés/fixes/JIT ou anonymes exécutables, traps Mach, appels indirects, threads, signaux, fichiers hôte/réseau, dyld, runtimes Objective-C/Swift et Foundation/UIKit sont hors contrat et arrêtent explicitement l’exécution. Ce profil ne constitue pas un OS Apple complet ni l’application iOS Simulator.

## Validation

Les fixtures C originales sont produites par Clang et `ld64.lld`, sans SDK Apple ni binaire propriétaire. Elles couvrent les cinq combinaisons plateforme/ISA, les enregistrements Mach-O malformés, les pages 4/16 KiB et la libération partielle sous budget plein. `NeverDProcessPublicTests` compare C API/CLI ; les variables `NEVERD_TEST_LIBNEVERD` et `NEVERD_TEST_DARWIN_FIXTURES` activent les mêmes cinq combinaisons dans le SDK Python.

## Fichiers et descripteurs explicites

`darwin_files` fournit aux trois profils un catalogue fermé de fichiers en lecture seule. Le champ obligatoire `files` contient un `path` invité absolu canonique et des `bytes_hex` hexadécimaux. `stdin_hex` est un flux fini facultatif : absent signifie inconnu et arrête une lecture non vide, une chaîne vide signifie EOF. Sans catalogue open s’arrête ; un catalogue explicitement vide renvoie ENOENT. Aucun fichier ni flux hôte n’est consulté.

Les services ajoutés sont `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`, avec les entrées nocancel de read/write/open/close/fcntl/pread. O_RDONLY/O_CLOEXEC et F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL sont pris en charge. Chaque open a sa position ; les duplications partagent la position mais gardent leurs propres indicateurs close-on-exec. pread ne déplace pas la position. Fermer ou remplacer 0/1/2 affecte les I/O suivantes ; une sortie dupliquée conserve sa destination et son budget.

Limites : 256 fichiers, 16 MiB cumulés pour chemins/NUL/fichiers/entrée, chemins de moins de 1024 octets et composants de 255 octets maximum. `descriptor_limit` est un plafond exclusif de 3–4096, défaut 256 ; JSON reste limité à 64 KiB. Les options invalides échouent avant chargement. read au-delà de INT_MAX donne EINVAL avant recherche du FD ; EOF ne touche pas la destination, une adresse invalide donne EFAULT. Un tampon partiellement inscriptible arrête avant copie ou changement de position. Les erreurs SET/CUR/END conservent la position. Chemins relatifs, ouverture de répertoires, écriture, stat, mappings de fichiers, seek creux et autres fcntl restent exclus. Un fichier utilisé comme ancêtre donne ENOTDIR. Le même objet est comparé au noyau macOS ; C/CLI/Python couvrent cinq combinaisons invitées, sans preuve sur appareil iOS.

Vérification Release du 2026-10-05 : 381 inscriptions, 177 réussites, 204 ignorées, aucun échec, et 51/51 cas ARM64 HVF obligatoires exécutés. Sept programmes macOS natifs, 35 tests publics C/CLI/rapports, cinq combinaisons Python et 66 tests du vérificateur ont également réussi. Les comptes se recoupent. Les nouveaux services n’ont pas de preuve native Intel HVF/KVM/WHP ; Intel HVF reste non validé et ses Actions suspendues. Le SDK iOS et la comparaison sur appareil manquent.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La validation indépendante exige chacun des 51 cas natifs ARM64 ou 34 cas x64, dont `LC_MAIN` et `LC_UNIXTHREAD` sur chaque plateforme. Les cas obligatoires absents/ignorés ou un `ld64.lld` manquant font échouer la validation.

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
