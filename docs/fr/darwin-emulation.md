**Langues**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 41b69637eb61adb006df941e3612df0726c8573ac4f5d79cb9f052de1ce96817 -->

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

Les appels BSD sur ARM64 utilisent X16, X0–X5 et `svc #0x80` ; x64 utilise la classe BSD `0x02000000`, RAX et RDI/RSI/RDX/R10/R8/R9. Une réussite efface carry ; une erreur le positionne et renvoie un errno positif. ARM64 efface X1 ; x64 efface RDX en cas de réussite et le conserve en cas d’erreur. Les registres modifiés par SYSCALL sont explicites. Le rapport utilise `result` et `error=true` pour une erreur BSD ; les requêtes sans retour ou non prises en charge n’ont aucun de ces champs. Les règles suivent les entrées XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) et [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), sans incorporation de code Apple.

Services : `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `mmap`, `mprotect`, `munmap`. PID/UID/GID valent 1000 et PPID vaut 1. Les descripteurs 1 et 2 capturent des octets, y compris NUL/non-UTF8 ; les descripteurs fermés ou en lecture seule donnent EBADF. Une copie partielle conserve les octets déjà lus mais renvoie EFAULT. Une longueur supérieure à `INT_MAX` donne EINVAL avant vérification du descripteur, du pointeur ou du budget, suivant [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

La mémoire accepte les mappings privés anonymes de données : `flags=0x1002`, descripteur -1 et offset zéro. Longueurs et adresses indicatives non fixes sont arrondies vers le haut à la page OS ; une indication occupée cherche plus haut, puis revient au placement par défaut. Le mmap brut historique de longueur zéro renvoie zéro sans allocation ; `MAP_UNIX03` est accepté et refuse la longueur zéro avec EINVAL. Unmap/protect exigent une adresse alignée. NONE/READ/WRITE sont pris en charge, WRITE implique READ. Les pages physiques appartiennent à chaque page OS : un unmap partiel libère son budget, puis les nouvelles pages sont à zéro. Un protect traversant un trou ou dépassant les droits maximaux échoue sans changement partiel. Référence : [services VM XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Les mappings partagés/fixes/JIT ou anonymes exécutables, autres traps Mach, appels indirects, threads, signaux, fichiers hôte/réseau, dyld, runtimes Objective-C/Swift et Foundation/UIKit sont hors contrat et arrêtent explicitement l’exécution. Ce profil ne constitue pas un OS Apple complet ni l’application iOS Simulator.

## Validation

Les fixtures C originales sont produites par Clang et `ld64.lld`, sans SDK Apple ni binaire propriétaire. Elles couvrent les cinq combinaisons plateforme/ISA, les enregistrements Mach-O malformés, les pages 4/16 KiB et la libération partielle sous budget plein. `NeverDProcessPublicTests` compare C API/CLI ; les variables `NEVERD_TEST_LIBNEVERD` et `NEVERD_TEST_DARWIN_FIXTURES` activent les mêmes cinq combinaisons dans le SDK Python.

## Fichiers et descripteurs explicites

`darwin_files` fournit aux trois profils un catalogue fermé de fichiers en lecture seule. Le champ obligatoire `files` contient un `path` invité absolu canonique et des `bytes_hex` hexadécimaux. `stdin_hex` est un flux fini facultatif : absent signifie inconnu et arrête une lecture non vide, une chaîne vide signifie EOF. Sans catalogue open s’arrête ; un catalogue explicitement vide renvoie ENOENT. Aucun fichier ni flux hôte n’est consulté.

Les services ajoutés sont `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`, avec les entrées nocancel de read/write/open/close/fcntl/pread. O_RDONLY/O_CLOEXEC et F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL sont pris en charge. Chaque open a sa position ; les duplications partagent la position mais gardent leurs propres indicateurs close-on-exec. pread ne déplace pas la position. Fermer ou remplacer 0/1/2 affecte les I/O suivantes ; une sortie dupliquée conserve sa destination et son budget.

Limites : 256 fichiers, 16 MiB cumulés pour chemins/NUL/fichiers/entrée, chemins de moins de 1024 octets et composants de 255 octets maximum. `descriptor_limit` est un plafond exclusif de 3–4096, défaut 256 ; JSON reste limité à 64 KiB. Les options invalides échouent avant chargement. read au-delà de INT_MAX donne EINVAL avant recherche du FD ; EOF ne touche pas la destination, une adresse invalide donne EFAULT. Un tampon partiellement inscriptible arrête avant copie ou changement de position. Les erreurs SET/CUR/END conservent la position. Écriture, ancien stat, seek creux et autres fcntl restent exclus. Un fichier utilisé comme ancêtre donne ENOTDIR. Le même objet est comparé au noyau macOS ; C/CLI/Python couvrent cinq combinaisons invitées, sans preuve sur appareil iOS.

Vérification Release du 2026-10-05 : 381 inscriptions, 177 réussites, 204 ignorées, aucun échec, et 51/51 cas ARM64 HVF obligatoires exécutés. Sept programmes macOS natifs, 35 tests publics C/CLI/rapports, cinq combinaisons Python et 66 tests du vérificateur ont également réussi. Les comptes se recoupent. Les nouveaux services n’ont pas de preuve native Intel HVF/KVM/WHP ; Intel HVF reste non validé et ses Actions suspendues. Le SDK iOS et la comparaison sur appareil manquent.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Répertoires et chemins relatifs

`directories` accepte des entrées `path` absolues canoniques, éventuellement avec `metadata` complet, y compris des répertoires vides. Racine et ancêtres sont implicites ; les métadonnées ne créent aucun chemin absent. Mode : `0x4000` plus permissions ; size : observation explicite dans [0, INT64_MAX]. `working_directory` doit désigner un répertoire existant ; son omission laisse CWD inconnu, sans héritage du hôte. Maximum : 256 chemins déclarés, ancêtres avec métadonnées inclus ; chemins/NUL/contenu/entrée/CWD totalisent 16 MiB.

`openat` (463), `openat_nocancel` (464), `chdir` (12), `fchdir` (13) et `fstatat64` (470) partagent le résolveur. Les chemins relatifs utilisent un FD de répertoire ou `AT_FDCWD=-2` ; les absolus ignorent le FD. Séparateurs répétés, `.`, `..` et slash final contrôlent chaque ancêtre : `/file/..` donne ENOTDIR, `/missing/..` ENOENT. Un échec ou la fermeture, réutilisation ou substitution du FD initial conserve CWD. `F_GETPATH=50` copie chemin canonique et NUL, même après dup, sans toucher la suite.

read/pread de répertoire donne EISDIR même à longueur zéro ; un offset pread négatif donne d’abord EINVAL. SET/CUR partagent le curseur, END exige size explicite ; mmap donne EINVAL. fstatat64 accepte 0, `AT_SYMLINK_NOFOLLOW=0x20`, `AT_SYMLINK_NOFOLLOW_ANY=0x800` et `AT_FDONLY=0x400` (chemin ignoré). Bits invalides : EINVAL ; `AT_REALDEV=0x200` reste exclu. Identité des flux inconnue, permissions sans modèle de contrôle d’accès ; les mutations restent à réaliser. Le même `directories` compare le noyau natif et cinq invités ; stat compare fichiers et répertoires réels. Intel HVF Actions reste suspendu.

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

Validation des répertoires (2026-10-05, Release) : 467 cas enregistrés, 227 succès, 240 omissions, aucun échec ; 60/60 obligations ARM64 HVF exécutées. Succès pour 10 programmes macOS natifs, 37 tests C/CLI/rapports sans omission, cinq invités Python et 66 tests des outils. Comptes chevauchants. Preuves : `build-hvf-arm64/darwin-directory-verified-evidence/`. Autres transports natifs et iOS physique non validés.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Instantanés explicites de répertoire

`getdirentries64` (344) parcourt le `contents` immuable facultatif d’un élément `directories` existant ; C++ utilise `DarwinFileOptions::DirectoryContents`. `entries` décrit dans l’ordre explicite tous les enfants directs, `.` et `..` compris. Sans instantané, même un répertoire vide reste inconnu. Aucun chemin, stat ou accès hôte n’est déduit.

Chaque entrée exige `name`, un `inode` non nul, `type` (0 inconnu, 4 répertoire, 8 fichier), `next_offset` et `seek_offset`. Type et chemin concordent ; les inodes d’un même chemin résolu concordent entre instantanés et métadonnées. `next_offset` est non nul, unique dans ce répertoire, <=INT64_MAX, sans ordre croissant requis ; zéro rembobine. `seek_offset` est l’observation d_seekoff distincte sur 64 bits non signés ; les zéros répétés sont permis. Les entiers suivent les chaînes décimales sans perte de stat.

`contents.minimum_buffer_size` est obligatoire : minimum de charge utile de 1–128 MiB, EOF inclus. Le `minimum_buffer_size` facultatif d’une entrée (défaut 0) s’applique au démarrage à cette position. L’exemple observe APFS : 64 octets pour les deux points initiaux, 1 à EOF ; ailleurs un enregistrement complet doit tenir. Le format LP64 est aligné sur huit octets, taille `roundUp(25 + nameBytes, 8)`. Maximum total : 4096 entrées ; leurs octets comptent dans les 16 MiB. Les chemins ancêtres déclarés uniquement par métadonnées/instantané comptent une fois dans les 256 chemins. JSON reste limité à 64 KiB.

Les open indépendants ont leurs curseurs, dup les partage. Seuls zéro et les valeurs fournies permettent la reprise ; une position inconnue arrête explicitement. Chaque appel retourne le plus grand préfixe d’enregistrements entiers. Une longueur >=1024 réserve les quatre derniers octets demandés à EOF (1 à la fin, sinon 0) ; seule la charge est plafonnée à 128 MiB. L’adresse des indicateurs conserve le calcul non signé original, débordement compris. Ordre : données, avance du curseur, copie de la position initiale, indicateurs. Un EFAULT tardif conserve les effets précédents ; EOF omet la copie vide. Une copie partiellement accessible s’arrête avant cette copie, sans annuler les effets antérieurs.

`directory-entries` compare les champs, dup/rembobinage, petites lectures, EOF et ordre des copies au noyau macOS. Un test séparé compare tous les octets natifs capturés, noms longs compris, au SDK. Les cookies fixes ne reproduisent pas les générations dynamiques APFS. L’ancien `getdirentries` (196), les mutations, les autres transports natifs et iOS physique restent hors de cette validation.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Validation de l’énumération (2026-10-05, Release) : 498 cas Darwin, 246 réussites, 252 omissions pour backend indisponible, zéro échec ; 63/63 cas ARM64 HVF obligatoires exécutés. Les 11 programmes macOS natifs, 40 contrôles C/CLI/rapport sans omission, cinq combinaisons Python avec huit scénarios de fichiers chacune et 66 tests des outils ont réussi. Comptages superposés. Preuves : `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions reste suspendu ; autres transports natifs et iOS physique non validés.


## Mappings privés de fichiers

`mmap` accepte les fichiers réguliers du catalogue avec `MAP_PRIVATE` : `flags=0x2` ou `0x40002` avec `MAP_UNIX03`, à un offset aligné sur une page OS. Même une longueur courte conserve tous les octets du fichier dans la page ; la fin de la dernière page EOF est nulle. Une écriture privée ne change ni le fichier, ni les autres mappings, ni les métadonnées fixes, ni le curseur partagé. Le mapping survit à close et à la réutilisation du FD. Les mappings en lecture seule et PROT_NONE sont initialisés ; `mprotect` peut ensuite autoriser l’écriture.

Un débordement de fin de fichier, une longueur UNIX03 nulle ou un offset UNIX03 non aligné donnent EINVAL avant recherche du FD ; un FD invalide donne EBADF avant contrôle du budget. Le mode historique de longueur nulle vérifie néanmoins le FD. Offsets historiques non alignés, flux, pages de fichiers vides et pages entièrement au-delà d’EOF arrêtent avant allocation. macOS permet ces pages EOF mais leur accès produit SIGBUS : le modèle n’invente ni pages nulles lisibles ni livraison de signal. Les mappings partagés, fixes, exécutables et JIT restent exclus.

`DarwinFiles` résout les FD et les octets, `DarwinMemory` gère placement, droits, budget et annulation. Seul `darwin_files` fournit les données. Le programme commun `file-mapping` vérifie copies privées, close, curseurs, erreurs et réutilisation anonyme ; une comparaison native séparée couvre un offset non nul, la page entière et SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Vérification des mappings privés, 2026-10-05

Release Darwin : 438 identités uniques, 210 succès, 228 omissions, aucun échec ; 57/57 cas ARM64 HVF obligatoires exécutés et cinq combinaisons Unicorn. Neuf programmes macOS natifs, la comparaison de toute une page à offset non nul et SIGBUS dans un enfant isolé ont réussi. Les 36 contrôles API/rapport n’ont aucune omission ; Python couvre cinq combinaisons avec `file-mapping`, et 66 tests d’outils et 38 de provenance passent. Les comptes se recoupent. Preuves : `build-hvf-arm64/darwin-mmap-verified-evidence/`. Aucun nouveau résultat Intel HVF/KVM/WHP ou iOS physique ; Intel HVF Actions reste suspendu.

## Métadonnées explicites des fichiers

Une entrée peut ajouter `metadata` ; tous les champs ci-dessous sont alors obligatoires. Les chaînes décimales préservent toute la largeur ; les nombres JSON restent des entiers exacts dans ±(2^53−1). device est signé sur 32 bits, mode/link_count non signés sur 16, inode non signé sur 64, uid/gid/flags/generation non signés sur 32. size doit égaler le nombre d’octets ; blocks tient sur 64 bits signés, block_size sur 32 bits signés non négatifs. Les temps utilisent des secondes signées sur 64 bits et 0–999999999 nanosecondes.

`stat64` (338), `fstat64` (339) et `lstat64` (340) produisent le même enregistrement LP64 de 144 octets sur ARM64/x64. Ils partagent la résolution de open, suivent dup/close, sans allouer de FD ni modifier le curseur. rdev, remplissage et réserves sont nuls. Les observations sont fixes : read ne change pas les temps et mode ne modifie pas l’accès au catalogue. Métadonnées absentes, flux, liens symboliques, ancien stat, et sécurité étendue restent exclus. Les erreurs de chemin/FD précèdent le pointeur de sortie ; une sortie partiellement accessible est refusée avant écriture. Le test natif compare tous les octets d’un fichier réel et les offsets du SDK ; le même programme original vérifie les trois appels.

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

Suite : mappings partagés et défauts de page EOF, écritures bornées (pages EOF, durée après close, ordre des erreurs), observations explicites temps/système, services Mach/threads requis, puis dépendances Mach-O, rebases/binds, initialiseurs et TLS. Objective-C/Swift et Foundation/UIKit nécessitent des programmes natifs de référence. iOS physique requiert SDK et appareil ; Intel HVF reste non validé et ses Actions suspendues.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La validation indépendante exige chacun des 69 cas natifs ARM64 ou 46 cas x64, dont `LC_MAIN` et `LC_UNIXTHREAD` sur chaque plateforme. Les cas obligatoires absents/ignorés ou un `ld64.lld` manquant font échouer la validation.

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

## Observations temporelles explicites

`ProcessOptions::DarwinTime` / `darwin_time` fournit des observations fixes à l’appel brut `gettimeofday` (116), y compris sa troisième sortie `mach_absolute_time`, pour tous les profils Darwin. `time_of_day`, `timezone` et `mach_absolute_time` sont facultatifs : l’absence signifie inconnu, zéro explicite est une valeur. Un objet vide ne crée aucune horloge par défaut. Le modèle ne consulte pas l’horloge hôte, ne déduit pas le fuseau, ne fait pas avancer le temps et ne convertit pas les ticks absolus.

Chaque enregistrement fourni exige tous ses membres. `seconds` est non signé sur 32 bits, `microseconds` vaut [0, 999999], `minutes_west` / `dst_time` sont signés sur 32 bits, les ticks sont non signés sur 64 bits. JSON suit les règles entières sans perte ; les valeurs hors plage sûre sont des chaînes décimales. Champs inconnus, dépassements et profils non Darwin sont rejetés avant le chargement.

Le `timeval` LP64 fait 16 octets : secondes étendues par zéro à 0, microsecondes sur 32 bits à 8, quatre octets nuls à 12. Le fuseau contient deux champs signés de 32 bits, les ticks huit octets. Les temps civil et absolu sont échantillonnés ensemble avant toute copie ou vérification de pointeur : chaque observation demandée doit donc exister. Les copies suivent l’ordre timeval, timezone, absolute ticks. Un fuseau manquant ou un EFAULT ultérieur conserve les écritures précédentes ; les alias suivent le même ordre. Une sortie individuellement partiellement accessible provoque un arrêt explicite avant cette copie, sans annuler les précédentes. Tous les pointeurs nuls réussissent sans configuration ; une requête sélective exige uniquement ses valeurs.

Le programme original `time` vérifie le comportement natif ; `time-values` émet les 32 octets configurés via C/CLI/Python sur les cinq combinaisons invitées. Un oracle SDK compare chaque octet aux trois sorties d’un seul appel brut natif. Horloges évolutives, conversion, compteurs commpage, temporisateurs et objets horloge Mach/IPC restent absents, comme les travaux dyld, threads, Objective-C/Swift et Foundation/UIKit. Intel HVF Actions reste suspendu ; aucune validation native Intel ou iOS physique n’est ajoutée.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Validation temporelle (2026-10-06, Release) : 538 cas Darwin, 274 réussites, 264 ignorés pour backend indisponible, aucun échec ; 66/66 cas ARM64 HVF obligatoires exécutés. Les 12 programmes macOS natifs et la comparaison SDK d’un seul échantillon passent. C/CLI/rapports : 43/43, sans omission. Python passe sur les cinq combinaisons, avec les octets temporels exacts et les huit modes de fichiers existants. Les 66 tests des outils, la traduction, les capacités et le format passent. Comptages superposés. Preuves : `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Temps Mach et conventions de retour

`darwin_time.timebase` fournit `numerator` et `denominator`, deux entiers non signés de 32 bits non nuls. Le rapport reste exact, sans réduction ni conversion. `mach_timebase_info_trap`, indice 89, utilise ARM64 X16=-89 ou x64 RAX=0x01000059. Il écrit huit octets little-endian (numérateur, dénominateur) et renvoie zéro, même si toute l’adresse de sortie est invalide. Une sortie partiellement accessible arrête l’exécution avant copie ; les erreurs du transport se propagent. Une configuration absente arrête avant la vérification du pointeur, même nul.

ARM64 X16=-3 et X16=-4 renvoient les 64 bits non signés de `mach_absolute_time` et `mach_continuous_time`. Chaque appel exige uniquement sa propre valeur ; zéro explicite est valide. Les entrées natives x64 correspondantes déclenchent EXC_SYSCALL et restent non prises en charge. Horloges évolutives, commpage, temporisateurs et objets horloge Mach/IPC restent absents.

La résolution utilise les 32 bits bas du numéro ; le rapport conserve les 64 bits originaux. Les nombres négatifs ARM64 sélectionnent Mach ; x64 utilise 0x01000000 pour Mach et 0x02000000 pour BSD. BSD 3/4 restent read/write ; numéros inconnus et classes étrangères arrêtent explicitement. La liaison résolue détermine la convention : Mach conserve les flags et X1/RDX, BSD garde ses règles carry ; x64 met toujours RCX/R11 à jour. Les retours Mach contiennent `result` et omettent `error`, même avec carry initialement actif.

`mach-time` compare flags, résultat secondaire, bits hauts, pointeurs invalides et transitions BSD au noyau ARM64 natif. `mach-timebase-values` vérifie les octets exacts sur cinq invités, `mach-clock-values` sur ARM64 ; le SDK vérifie disposition et rapport capturé. Intel HVF Actions reste suspendu ; les essais logiciels et syntaxiques x64 ne valident ni Intel natif ni iOS physique.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Validation Mach (2026-10-06, Release) : 569 cas Darwin, 293 réussis, 276 ignorés pour backend indisponible, aucun échec ; 69/69 cas ARM64 HVF obligatoires exécutés. Les 13 programmes natifs et les deux oracles temporels SDK ont réussi lors du passage final. C/CLI/report : 100/100 sans omission ; Python couvre cinq invités. Les comparaisons publiques sont isolées par plateforme et scénario, avec un budget invité explicite de 10 secondes ; valeurs produit et régressions de délai restent inchangées. Les comptes se recoupent.

Les premiers démarrages natifs dépassaient la limite existante de 5 secondes : mesure indépendante de 6.056 secondes, puis 0.010 à la réutilisation. Le même binaire a ensuite réussi les 13 cas sous la limite originale ; les échecs sont conservés. La vérification séquentielle séparée a réussi après des délais dépassés sous charge hôte. Preuves : `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`, sur l’arbre avant commit. ARM64 MRS/MSR NZCV reste absent ; le test utilise des instructions entières pour les quatre flags. Cette lacune CPU, fichiers modifiables, informations système, dyld/runtimes/frameworks et iOS physique restent ouverts.
