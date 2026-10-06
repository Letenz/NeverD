**Langues**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 8828ab2cc1f47bed24529d8d47a05ee929790480d47f96fcb54d6ebc101ae2ce -->

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

`darwin_files` fournit aux trois profils un catalogue fermé de fichiers initialement en lecture seule. Le champ obligatoire `files` contient un `path` invité absolu canonique et des `bytes_hex` hexadécimaux. `stdin_hex` est un flux fini facultatif : absent signifie inconnu et arrête une lecture non vide, une chaîne vide signifie EOF. Sans catalogue open s’arrête ; un catalogue explicitement vide renvoie ENOENT. Aucun fichier ni flux hôte n’est consulté.

Les services ajoutés sont `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`, avec les entrées nocancel de read/write/open/close/fcntl/pread. O_RDONLY/O_CLOEXEC et F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL sont pris en charge. Chaque open a sa position ; les duplications partagent la position mais gardent leurs propres indicateurs close-on-exec. pread ne déplace pas la position. Fermer ou remplacer 0/1/2 affecte les I/O suivantes ; une sortie dupliquée conserve sa destination et son budget.

Limites : 256 fichiers, 16 MiB cumulés pour chemins/NUL/fichiers/entrée, chemins de moins de 1024 octets et composants de 255 octets maximum. `descriptor_limit` est un plafond exclusif de 3–4096, défaut 256 ; JSON reste limité à 64 KiB. Les options invalides échouent avant chargement. read au-delà de INT_MAX donne EINVAL avant recherche du FD ; EOF ne touche pas la destination, une adresse invalide donne EFAULT. Un tampon partiellement inscriptible arrête avant copie ou changement de position. Les erreurs SET/CUR/END conservent la position. Ancien stat et autres fcntl restent exclus. Un fichier utilisé comme ancêtre donne ENOTDIR. Le même objet est comparé au noyau macOS ; C/CLI/Python couvrent cinq combinaisons invitées, sans preuve sur appareil iOS.

Vérification Release du 2026-10-05 : 381 inscriptions, 177 réussites, 204 ignorées, aucun échec, et 51/51 cas ARM64 HVF obligatoires exécutés. Sept programmes macOS natifs, 35 tests publics C/CLI/rapports, cinq combinaisons Python et 66 tests du vérificateur ont également réussi. Les comptes se recoupent. Les nouveaux services n’ont pas de preuve native Intel HVF/KVM/WHP ; Intel HVF reste non validé et ses Actions suspendues. Le SDK iOS et la comparaison sur appareil manquent.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Modification des fichiers existants

Le booléen strict `"writable":true` ou `DarwinFileOptions::WritableFiles` autorise les modifications locales au processus. Absent/false conserve la lecture seule ; une autorisation inconnue arrête le service. Aucun fichier hôte ni octet initial fourni n’est modifié. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) et O_TRUNC partagent le contenu ; open garde une position indépendante, dup partage position et état, et le contenu survit au dernier close. L’extension remplit de zéros ; la troncature conserve les positions, même avec O_RDONLY|O_TRUNC.

F_SETFL ne modifie que O_APPEND et conserve accès, close-on-exec et FWASWRITTEN. F_GETFL expose 0x10000 après un transfert non vide, y compris pwrite et la sortie capturée. pwrite ignore append et conserve la position. INT_MAX est vérifié avant FD ; pwrite à -1 donne EINVAL encore plus tôt. INT64_MAX donne EFBIG avant le cas vide ; la longueur est réduite avant de choisir EOF.

ftruncate réussi, même sans changement de taille, marque FWASWRITTEN sur la description appelée et ses dup. O_TRUNC marque la nouvelle description, même O_RDONLY ; truncate par chemin ne marque aucune description existante.

Une entrée partiellement lisible arrête avant tout effet. EFAULT intégral conserve les octets, mais append non vide avance à EOF. Une panne du transport ne valide aucun contenu ni position. Sans `mutation_policy`, écriture non vide, troncature et EFAULT intégral non vide invalident l’observation stat complète ; les requêtes suivantes s’arrêtent avant copie. Une écriture vide la conserve. Les 16 MiB comptent chemins/NUL, entrée, enregistrements, CWD, contenu actuel et références de chemins inscriptibles. Réduire remplace le stockage et récupère la capacité ; entrée initiale et tampon de remplacement borné sont supplémentaires. Alias inode connus et flags immutable/append-only sont refusés.

DarwinMemory garde des baux jusqu’au dernier unmap, même pour PROT_NONE ou après close ; les mutations sont refusées jusque-là. Les échecs et anciens mmap de longueur zéro ne gardent aucun bail. Les nouveaux mappings voient les octets actuels. O_WRONLY avec READ/WRITE donne EACCES ; PROT_NONE peut ensuite gagner lecture/écriture par mprotect.

Les programmes originaux normal/nocancel comparent le noyau natif ; tests 4K/16K et C/CLI/Python couvrent cinq combinaisons. Contrôle des droits, suppression de répertoires, renommage entre parents, liens physiques, métadonnées du système de fichiers natif, cohérence des mappings et SIGBUS EOF restent incomplets. Ni environnement complet, ni appareil iOS, ni Intel HVF ne sont validés ; les Actions Intel restent suspendues.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Métadonnées modifiables explicites

Un fichier peut ajouter mutation_policy à `writable: true` et aux metadata complètes ; C++ utilise `DarwinFileOptions::MutationPolicies`. Ce contrat virtuel d’allocation creuse est explicite : aucune allocation APFS ni heure hôte n’est déduite. Sans politique, les métadonnées après mutation restent inconnues.

allocation_unit, mutation_time et seconds/nanoseconds sont obligatoires, avec les règles entières sans perte existantes. L’unité est une puissance de deux entre512 octets et16 MiB, indépendante de block_size et des pages VM. Il faut des permissions ordinaires sans set-id/sticky, flags=0, link_count=1 et une allocation initiale dense : blocks=ceil(size/allocation_unit)*(allocation_unit/512). Les octets nuls n’impliquent aucun trou. La référence du chemin compte dans les16 MiB logiques ; le registre d’allocation ne sert pas à inventer ENOSPC.

Toute unité touchée par une écriture est allouée, même pour écrire des zéros dans un trou. Agrandir par truncate ajoute des zéros sans allocation ; réduire supprime les unités au-delà d’EOF arrondi vers le haut, conservant l’unité finale partielle. Agrandir ensuite ne restaure pas les unités supprimées. Une écriture non vide réussie ou tout truncate réussi, même de même taille ou O_TRUNC vide, met à jour size/blocks et fixe mtime/ctime au temps fourni. Les autres champs et entrées restent inchangés ; read n’avance pas atime. Stat par chemin, open indépendants, dup et réouverture partagent le nœud.

Écriture vide, refus de budget/mapping, entrée partielle refusée et panne du transport préservent l’état connu. EFAULT intégral non vide le rend inconnu ; un succès ultérieur ne le reconstruit pas. Un échec de copie stat ne change pas le nœud. virtual-file-metadata vérifie144 octets sur cinq profils et C/CLI/Python : c’est un test de politique, pas une équivalence APFS. Les programmes natifs vérifient séparément flags, positions et erreurs. Espace de noms, cohérence native, Mach et chargement dynamique restent incomplets.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```



## Positionnement dans les fichiers creux

Avec mutation_policy et une allocation encore connue, lseek accepte SEEK_HOLE=3 et SEEK_DATA=4 sur les fichiers ordinaires et lit le même registre que stat. L’entrée initiale est dense, même avec des zéros. Dans une unité du type demandé, il renvoie la position fournie ; sinon le début de l’unité suivante correspondante. Le trou terminal est EOF. Une position négative donne EINVAL ; à/après EOF, même fichier vide, ou sans données suivantes, ENXIO=6. L’erreur conserve la position ; le succès ne change que la description et ses dup. Les open indépendants gardent leur position, la réouverture voit l’allocation actuelle. Métadonnées, flags et octets restent inchangés ; les bits hauts de whence sont ignorés.

Sans politique, pour un répertoire ou après EFAULT intégral rendant l’allocation inconnue, le service reste exclu. Zéros et modifications refusées ne créent aucune allocation supposée. Le programme original sparse-file-seek compare erreurs, octets écrits, EOF et durée des descriptions natifs/invités sans présumer les limites antérieures propres au FS. virtual-file-metadata vérifie séparément la géométrie exacte de la politique ; C/CLI/Python couvrent cinq profils.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Suppression des noms de fichiers ordinaires

`mutable:true` par répertoire (C++ `MutableDirectories`) autorise explicitement les changements de noms immédiats, indépendamment de `writable`. Sans autorisation, arrêt explicite. L’admission refuse les flags connus non nuls, les permissions spéciales du parent, link_count≠1 pour un enfant et les alias connus du parent/enfant, en combinant stat et inodes des snapshots. Des périphériques explicitement distincts restent distincts ; les chemins consomment le budget existant.

`unlink(10)` / `unlinkat(472)` retirent les noms ordinaires existants. unlinkat accepte les 32 bits bas 0 ou `0x800` ; bits inconnus : EINVAL avant chemin/FD ; autres modes connus de suppression restent non pris en charge. Résolution commune : ENOENT, ENOTDIR après un fichier suivi de `/`, EPERM pour répertoire ordinaire, EBUSY pour racine. Les suffixes `.`/`..` ont aussi été vérifiés nativement.

Les anciens FD/dup/ouvertures indépendantes gardent données, positions et flags ; F_GETPATH conserve l’ancien chemin capturé. Les nouvelles ouvertures échouent, parents implicites et CWD subsistent. L’autorisation d’écriture appartient à l’objet ; son budget de données actuelles est récupéré après le dernier descripteur et mapping, via close/dup2 ou la prochaine mutation. Les coûts initiaux des chemins restent comptés. Contrôle des droits, renommage entre parents, liens physiques et suppression de répertoires restent à faire.

Les observations stat/readdir/SEEK_END du parent deviennent inconnues pour tous les FD et chemins, avant copie/déplacement. read/pread restent EISDIR ; SET/CUR/F_GETPATH/fchdir/résolution relative continuent. Une politique connue fixe nlink=0 et ctime, sans restaurer nlink=1 aux écritures suivantes. Sans politique/après EFAULT, métadonnées inconnues. Les échecs préservent l’état. `unlinked-file` compare les règles natives de noms/FD ; temps et invalidation sont des règles explicites du modèle.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Création de fichiers ordinaires

O_CREAT=0x200 crée un fichier vide dans un parent direct explicitement mutable, via open/openat normal ou nocancel. Le nouvel objet est inscriptible ; les objets existants conservent leur autorisation WritableFiles. Un FD en lecture seule peut créer, mais pas écrire. Sans politique de création explicite, stat64 et recherche de trous restent inconnus. Les metadata/mutation_policy d’un ancien objet homonyme ne sont jamais hérités.

Avec O_CREAT, O_EXCL=0x800 renvoie EEXIST sur fichier/répertoire existant avant troncature ; seul, il est sans effet. O_CREAT en lecture seule ouvre un répertoire existant. Ordre : mode d’accès invalide, disponibilité FD, EINVAL pour O_CREAT|O_DIRECTORY, puis chemin. Seul le dernier composant original absent peut être créé ; ancêtre absent et terminaisons `/`, `//`, `/.`, `/..` donnent ENOENT. Une création O_TRUNC ne marque pas FWASWRITTEN, contrairement à la troncature d’un objet existant.

Seule l’insertion invalide les observations du parent. Objets homonymes ancien/nouveau gardent données, FD, métadonnées et baux de mapping distincts. La limite de 256 compte les entrées initiales non-fichiers et objets vivants ; chemins canoniques/NUL dynamiques et octets courants comptent dans 16 MiB. Après unlink, le dernier FD/mapping libère les coûts dynamiques ; les coûts initiaux restent réservés. Budget épuisé ou chemin canonique de 1024 octets arrête explicitement sans inventer ENOSPC ou erreur native de chemin ; aucun nom/FD n’est publié. created-file compare le natif aux cinq profils, avec limites 4K/16K. Contrôle des droits, renommage entre parents, liens et mutation des répertoires restent à compléter.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Métadonnées de création explicites et umask du processus

Le `darwin_files.umask` facultatif (C++ `InitialUmask`) définit le masque initial entre 0 et 07777 octal, indépendamment du droit de création. `umask(60)` renvoie l’ancien masque et conserve les bits bas 07777, sans mémoire invitée ni FD libre. L’omission signifie inconnu, sans valeur hôte ou valeur par défaut supposée. L’initialisation est unique ; les changements concernent uniquement les créations futures, sans modifier l’entrée. L’exemple utilise 18 décimal, soit 0022 octal.

Le `darwin_files.creation_policy` facultatif (C++ `CreationPolicy`) fournit les métadonnées complètes des nouveaux objets. L’objet strict contient exactement `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy` ; temps et politique de mutation utilisent les formats existants. Il faut un umask explicite, au moins un parent mutable et des metadata complètes pour chaque parent autorisé. block_size est dans 1..INT32_MAX, generation est uint32 ; l’unité d’allocation est une puissance de deux de 512 à 16 MiB, indépendante du bloc/de la page VM, et les nanosecondes sont dans [0,1000000000). first_inode est un uint64 positif supérieur à tous les inode stat/instantanés, y compris d’autres périphériques. Les chaînes décimales préservent les entiers hors du domaine exact de JSON.

Seule l’insertion réussie d’un nouvel objet consomme la suite globale d’inode. UINT64_MAX l’épuise définitivement ; close/unlink/réutilisation de nom/umask/recherches ne la réinitialisent pas. Refus d’exclusivité, FD, chemin, nombre d’entrées ou budget d’octets ne publient ni nom/FD ni incrément ; O_CREAT existant ne consomme rien. Le nouveau stat64 utilise device/GID du parent direct, UID=1000 de l’utilisateur effectif invité fixe, mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 et size/blocks/flags=0. Bloc, generation et quatre temps initiaux fixes viennent de la politique. Après invalidation du stat/énumération complet du parent, device/GID restent utilisables sans restaurer tout le relevé.

Chaque nœud possède ses métadonnées/allocations, sans héritage de l’ancien homonyme. write/truncate/unlink partagent la politique et préservent inode/mode/birthtime et nlink=0 après unlink ; un EFAULT intégral laisse l’état définitivement inconnu. Les nœuds existants ne changent pas rétroactivement. `created-file-metadata` compare droits, ancien masque, UID effectif, périphérique/groupe parent et durée de vie natifs dans cinq profils ; `virtual-created-metadata` compare séparément les 144 octets. Les quatre temps natifs peuvent différer. Temps fixes/allocation creuse sont des règles virtuelles ; contrôle des droits, changement d’identité, ACL et comportement APFS natif restent à faire.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Renommage de fichiers ordinaires dans un même parent

`rename(128)`, `renameat(465)` et `renameatx_np(488)` renomment ou remplacent un fichier dans son parent immédiat explicitement modifiable. Même une opération sans effet sur le même nom exige cette autorisation et conserve les observations. Les 32 bits bas des flags acceptent 0 ou `RENAME_NOFOLLOW_ANY=0x10` ; bits inconnus et EXCL+SWAP donnent EINVAL avant les chemins, les autres flags connus sont non pris en charge. Le résolveur commun conserve priorité de la source, FD et vérifications des composants d’origine. Une source répertoire est immédiatement refusée ; un point/double point final résolu donne EINVAL avant montage et autorisation, même pour des parents imbriqués ou distincts. Les erreurs d’ancêtres absents/non répertoires restent prioritaires. Une cible répertoire ordinaire dans le parent admis donne EISDIR.

Les anciens FD, open indépendants et dup suivent le nouveau nom source avec `F_GETPATH`. La cible remplacée conserve son dernier chemin, ses octets, positions, flags et mappings ; ses droits d’écriture et métadonnées ne passent pas à la source. Chaque politique connue modifie son ctime, et nlink=0 pour la cible, en préservant identité, propriétaire, naissance et allocation. Sans politique ou après EFAULT complet, les métadonnées restent inconnues. Seul un déplacement réel invalide stat/énumération du parent.

Aucun nouvel inode, entrée ou FD libre n’est nécessaire. Le chemin/NUL remplace le coût dynamique source, les coûts initiaux restent réservés. Seule une cible sans ancien FD/mapping finance immédiatement la capacité, récupérée une seule fois ; un unmap partiel conserve le coût complet. Chemin de 1024 octets ou budget de 16 MiB dépassé : arrêt avant modification. Changement de parent, devices connus contradictoires, répertoires, swap/exclusive/seclude et permissions restent incomplets. Même device stat ne prouve pas le même montage : aucun EXDEV inventé. `renamed-file` compare identité, chemins, remplacement et mappings natifs/invités ; temps et budgets sont des règles virtuelles.


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

`stat64` (338), `fstat64` (339) et `lstat64` (340) produisent le même enregistrement LP64 de 144 octets sur ARM64/x64. Ils partagent la résolution de open, suivent dup/close, sans allouer de FD ni modifier le curseur. rdev, remplissage et réserves sont nuls. Les entrées donnent les métadonnées initiales, puis la politique facultative régit les modifications ; read ne change pas les temps et mode ne modifie pas l’accès au catalogue. Métadonnées absentes, flux, liens symboliques, ancien stat, et sécurité étendue restent exclus. Les erreurs de chemin/FD précèdent le pointeur de sortie ; une sortie partiellement accessible est refusée avant écriture. Le test natif compare tous les octets d’un fichier réel et les offsets du SDK ; le même programme original vérifie les trois appels.

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

La validation indépendante exige chacun des 96 cas natifs ARM64 ou 64 cas x64, dont `LC_MAIN` et `LC_UNIXTHREAD` sur chaque plateforme. Les cas obligatoires absents/ignorés ou un `ld64.lld` manquant font échouer la validation.

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

Les premiers démarrages natifs dépassaient la limite existante de 5 secondes : mesure indépendante de 6.056 secondes, puis 0.010 à la réutilisation. Le même binaire a ensuite réussi les 13 cas sous la limite originale ; les échecs sont conservés. La vérification séquentielle séparée a réussi après des délais dépassés sous charge hôte. Preuves : `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`, sur l’arbre avant commit. À cette révision, ARM64 MRS/MSR NZCV étaient absents du contrat checked ; le test observait donc les flags avec des instructions entières. Le changement ci-dessous comble cette lacune CPU.

## Registre des flags de condition ARM64

Le contrat ARM64 checked partagé admet les encodages exacts `MRS Xt, NZCV` et `MSR NZCV, Xt` en EL0/EL1. La lecture ne renvoie que les bits 31–28 ; l’écriture sélectionne ces quatre bits d’entrée et ignore les autres. Lire vers `XZR` abandonne le résultat ; écrire depuis `XZR` efface les flags sans lire SP. Chaque backend exécute les instructions originales. La validation du setter hôte et les limites FPCR/FPSR restent inchangées ; les registres système voisins non déclarés restent non pris en charge.

`NeverDAArch64NZCVTests` compare toutes les combinaisons aux instructions hôtes et vérifie l’état scalaire/vectoriel complet, la mémoire, les registres limites, l’arrêt/l’échec des observateurs, la reprise du contexte et les budgets partagés. ARM64 `mach-time` utilise désormais de vrais MSR/MRS autour de SVC pour vérifier la conservation Mach et la transition vers BSD. Les exigences HVF natives comprennent les six méthodes aux deux privilèges et l’oracle hôte. ARM64 KVM/WHP et iOS physique restent non validés. Fichiers modifiables, informations système, horloges progressives, Mach IPC/threads, dyld/runtimes/frameworks et validation sur appareil restent à réaliser.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Validation des fichiers modifiables (2026-10-06) : Release Darwin, 610 inscriptions, 322 réussites, 288 ignorées pour backend indisponible, zéro échec ; 72/72 obligations ARM64 HVF exécutées. La vérification finale, avec les nouvelles assertions EFAULT/métadonnées, compte 102 réussites et 12 ignorées sur 114. Les 15 programmes natifs et 111 tests publics C/CLI/rapports passent aussi. Les comptes se recoupent. Le premier essai natif a révélé FWASWRITTEN, corrigé avant réussite ; son échec est conservé. Aucun délai changé. CI GitHub complète et appareil iOS restent séparés ; Actions Intel suspendues.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python a d’abord dépassé cinq secondes dans trois cas de répertoire ARM64. L’observation sans changer les arguments a réussi les dix nouveaux cas inscriptibles ; un répertoire iOS a expiré à 5,005 s réelles pour 1,263 s CPU. Les trois reprises isolées, même limite, passent en 2,43–3,17 s : 10 941 instructions, sortie65. Charge54–70 pour16 CPU logiques : indice de pression de planification, pas une garantie de latence ; échecs conservés.

La méthode Python finale inchangée a réussi les cinq combinaisons en41,118 s, avec cinq secondes par processus. Les échecs et diagnostics précédents restent séparés.


Validation des métadonnées (2026-10-06) : Release ciblé148=124 réussites/24 ignorés. Darwin complet645=343 réussites/300 ignorés/2 délais dépassés dans l’énumération ARM64 HVF existante. Reprise identique20=8 réussites/12 ignorés,3.818/3.949s pour les cas concernés, limite initiale5s. Les75 identités HVF obligatoires ont des observations réussies ; le premier échec reste conservé. Public C/CLI/rapports117/117 dont73 Darwin, Python cinq profils27.359s, natif15/15, runners66/66 réussis. Allocation virtuelle, pas preuve APFS ; aucun délai modifié. CI complète, Intel, iOS physique et environnement complet restent à valider.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Validation du positionnement sparse (2026-10-06) : Release Darwin, 671 cas, 359 réussis, 312 ignorés pour backends indisponibles, aucun échec. Les 78 cas ARM64 HVF obligatoires ont été exécutés ; Unicorn couvre cinq profils. Vérifications ciblées : 123 réussites sur 147, 24 ignorés. Les 16 programmes natifs, 122 contrôles C/CLI/report (78 comparaisons Darwin), cinq profils Python (12.344 s) et 66 tests du runner passent. Comptages recoupés, délais inchangés, échecs historiques conservés. Preuves : `build-hvf-arm64/sparse-seek-validation-summary.json`. La géométrie relève de la politique virtuelle explicite, sans équivalence APFS. CI complète et iOS physique restent à valider ; Intel HVF Actions reste suspendu.

Validation unlink (2026-10-06) : Release Darwin 708 cas, 384 réussis, 324 ignorés pour backends indisponibles, aucun échec ; 81 ARM64 HVF obligatoires exécutés. Ciblés : 137/156 réussis, 19 ignorés. Natifs17/17, C/CLI/report128/128 (Darwin83), Python cinq profils16.268s, runner66/66 réussis. Revue indépendante sans blocage restant. Comptages recoupés, délais inchangés, aucune reprise nécessaire. Preuves : `build-hvf-arm64/unlink-validation-summary.json`. Invalidation et temps fixes sont des règles du modèle ; système de fichiers/runtime complet et iOS physique restent à valider. Intel HVF Actions suspendu, CI complète distincte.

### Validation de la création, 2026-10-06

Release Darwin : 748 cas, 412 réussis, 336 ignorés faute de backend, aucun échec ; 84 obligations ARM64 HVF exécutées. Ciblés162 :150 réussis/12 ignorés. C/CLI/rapports133/133 (Darwin88), Python5 profils9.982s, natif18/18, runners66/66 réussis. Le premier ARM64 refusait correctement les rebases de la table de pointeurs du test ; son remplacement par des octets intégrés corrige le fixture sans assouplir le chargeur. Échecs/binaires initiaux conservés, inventaire attendu27→28. Revue indépendante sans blocage, avec conservation des observations parentales après refus de budget. Comptages chevauchants, délais inchangés. CI complète et iOS physique séparés ; Actions Intel HVF suspendues.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Validation des métadonnées de création, 2026-10-06

Release Darwin : 787 cas, 439 réussis, 348 ignorés pour backends indisponibles, aucun échec ; les 87 ARM64 HVF obligatoires exécutés. Ciblés : 139/151 réussis, 12 ignorés. C/CLI/rapport : 145/145, dont 98 comparaisons d’entrées Darwin ; méthode Python inchangée avec cinq profils en 12.211 secondes. Natifs 19/19 et scripts 66/66 réussis. Revue indépendante sans blocage ; nouveaux cas : parents device/GID distincts et inode global, unlink avant première écriture, umask sans FD libre/entrée utilisable. Comptages recoupés, délais inchangés, aucune reprise après échec nécessaire. Temps fixes de création/mutation et allocation restent des politiques virtuelles. GitHub CI complète et iOS physique restent distincts ; Intel HVF Actions suspendu.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Vérification du renommage, 2026-10-06

Release Darwin : 835 inscriptions, 474 succès, 360 indisponibles et un dépassement existant macOS ARM 64 HVF de métadonnées virtuelles (5.087 s). Même méthode, paramètres et limite de 5 s : 8 succès et 12 ignorés, identité concernée 0.113 s. Les 90 identités ARM 64 HVF obligatoires ont une observation réussie sur ces exécutions ; le premier gate final reste enregistré en échec. Ciblés 42/54 réussis,12 ignorés ; C/CLI/rapport 150/150 dont 103 comparaisons Darwin ; Python inchangé, cinq profils en 18.478 s ; natifs 20/20, scripts 66/66. La revue indépendante a trouvé le classement incorrect du point imbriqué, reproduit en 4 K/16 K puis corrigé. Une ancienne attente de test ftruncate en lecture seule a été corrigée vers EINVAL. Échecs et versions des sondes conservés, comptes recouvrants, délais inchangés. GitHub CI complet et iOS physique restent distincts ; Actions Intel HVF suspendues.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Observations système explicites

`ProcessOptions::DarwinSystem` / `darwin_system` fournit des observations fixes à `sysctl(202)` et au `sysctlbyname(274)` brut pour chaque profil Darwin. Chaque champ est facultatif ; une valeur absente ou une clé non répertoriée reste non prise en charge. Aucune interrogation de l’hôte ni version ou modèle implicite. La validation JSON stricte et C++ rejette les valeurs incorrectes et les profils non Darwin avant le chargement.

`os_revision` est signé sur 32 bits ; `cpu_count` va de 1 à INT32_MAX ; `memory_size` conserve les 64 bits non signés. Les autres champs sont des chaînes de 1023 octets maximum sans NUL interne ; la chaîne vide explicite est valide. La sortie inclut le NUL final. Ces rapports ne changent ni l’ordonnancement ni le budget mémoire.

| Champ JSON | Nom sysctl | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |

`hw.pagesize` utilise la politique mémoire du guest : huit octets normalement, quatre si la sortie non nulle a une capacité exactement égale à quatre. Le MIB historique `[6,7]` et `hw.pagesize_compat` renvoient toujours quatre octets. L’OID numérique dynamique de `hw.pagesize` reste non pris en charge. À capacité quatre, `hw.memsize` se réduit seulement si son motif 64 bits est l’extension signée d’un entier 32 bits ; sinon ERANGE34 conserve sortie et longueur.

Le nombre MIB utilise les 32 bits bas et doit être 2–12 ; la longueur du nom utilise 64 bits et reste inférieure à 1024. Tous les octets fournis sont vérifiés avant le premier NUL et le retrait d’un point final. Un nom vide donne ENOENT ; une entrée partiellement lisible reste non prise en charge. Un `oldlenp` non nul exige huit octets entièrement lisibles et modifiables avant tout effet. Les sondes natives de pointeurs de longueur invalides n’ont pas terminé dans leur délai : ce cas reste explicitement hors périmètre. `oldlenp` nul signifie capacité zéro ; `oldp` nul demande seulement la taille. Un tampon court donne ENOMEM12 sans changer les données et écrit une longueur zéro. EFAULT sur les données conserve la longueur. Entrée et capacité sont capturées avant les données, puis la longueur est copiée en dernier, avec conservation des alias et des copies déjà effectuées en cas d’erreur de transport ultérieure.

Une écriture exige `newp` et `newlen` non nuls. Les nœuds sélectionnés renvoient EPERM1 pour l’identité fixe non root avant les contrôles de valeur ou de sortie, y compris `kern.osversion`, modifiable nativement avec privilèges. Une longueur nouvelle nulle ignore le pointeur. Aucun ENOENT n’est inventé pour les clés, arbres ou OID dynamiques inconnus.

Le programme original `system-info` compare l’ABI native macOS et guest ; `virtual-system` compare les octets configurés via C++, C/CLI et Python. Un oracle SDK capture les neuf observations de l’hôte comme entrées explicites de test et compare sorties nommées et numériques. Cela ne valide ni iOS physique ni Intel HVF.

```json
{"darwin_system":{"os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man 3/sysctl.3.html).

### Validation des requêtes système, 2026-10-06

Release Darwin : 881 inscriptions, 509 succès, 372 indisponibles ignorés, aucun échec ; les 93 identités ARM64 HVF obligatoires ont été exécutées. Ciblés : 37/49 réussis et 12 ignorés. C/CLI/rapport : 163/163, dont 113 comparaisons Darwin. Méthode Python inchangée : cinq profils en 15.302 s ; programmes natifs 21/21, scripts 66/66. Revue indépendante sans blocage ; combinaisons supplémentaires de priorité et oracle SDK réussis. Une compilation du nouvel oracle a échoué faute de StringExtras, puis réussi après ajout ; source et journal conservés. Les sondes natives de longueur invalide restent conservées et hors périmètre. Après les tests, seuls deux commentaires d’en-tête ont été normalisés, puis la reconstruction a réussi. Comptes recouvrants, délais inchangés, aucune répétition après échec d’exécution nécessaire. GitHub CI complet et iOS physique restent distincts ; Actions Intel HVF suspendues.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## Entrées-sorties vectorielles et capture

`readv`/`writev`, `preadv`/`pwritev` et leurs entrées nocancel partagent les traitements scalaires des fichiers et de la capture, sans nouvelle option ni accès hôte. Chaque iovec LP64 contient une adresse et une longueur de huit octets. Les 32 bits bas signés de iovcnt doivent valoir 1–1024. Le tableau entier est copié avant la recherche du descripteur, donc les alias de sortie ne modifient pas la demande. Un tableau partiellement lisible reste non pris en charge.

Les droits et la possibilité de positionner un flux précèdent les longueurs : chacune et leur somme doivent tenir dans INT64_MAX, avec une limite supplémentaire INT_MAX pour fichiers et répertoires. Le stdin fini est limité aux octets disponibles ; la capture garde son budget. pwritev refuse tout décalage négatif avant le tableau ; preadv vérifie le décalage après le descripteur et les longueurs. Les éléments vides ignorent leur adresse, mais gardent les contrôles de descripteur, type et position. EOF évite les éléments inutilisés. Les appels positionnés préservent le curseur et pwritev ignore l’ajout. L’ajout ordinaire borne toute la demande une seule fois avec le curseur initial, puis choisit EOF.

Un élément ultérieur entièrement invalide renvoie EFAULT et conserve les octets précédents, le curseur ordinaire et FWASWRITTEN après écriture d’au moins un octet. Une écriture non vide admise qui renvoie EFAULT sur un tampon de données invalide les métadonnées complètes ; erreurs d’arguments, refus du modèle et erreurs backend les préservent. Une destination de lecture partiellement accessible provoque UnsupportedService sans copier cet élément, en conservant les copies antérieures. Une source de fichier partiellement lisible reste non prise en charge avant tout effet sur le fichier. Autorisation, baux de mapping et budget total précèdent l’écriture ; les erreurs de précontrôle ou lecture du backend ne publient aucun octet de fichier ou capture.

La capture contrôle d’abord le budget commun stdout/stderr. Un élément franchissant la limite d’adresse utilisateur ne contribue aucun octet ; les précédents restent acquis. Les autres préfixes lisibles sont capturés avec EFAULT. La priorité scalaire de l’erreur de plage sur le budget est conservée. Les descripteurs dupliqués ou redirigés gardent leur destination. Le programme original `vectored-io` vérifie les huit entrées sur macOS natif, les cinq combinaisons invitées et C/CLI/Python. Aucun ajout de cancellation, pipes, threads ou validation iOS physique.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Vérification des E/S vectorielles, 2026-10-06

Release Darwin :937 inscriptions,553 réussites,384 sauts de backend indisponible, aucun échec ;96 identités ARM64 HVF obligatoires exécutées. Ciblés :45/57 réussis,12 sauts. C/CLI/report :168/168, dont118 comparaisons Darwin ; Python couvre cinq combinaisons en 20.397s. Natif :22/22 ; scripts :66/66. La revue indépendante a ajouté un défaut d’écriture positionnée creuse vérifiant curseur, EOF réel, refus des métadonnées et capacité restante exacte. La première compilation référençait une requête interne supprimée dans un ancien test, désormais remplacée par la vérification de la sortie réelle. Une erreur optional<bool> dans la nouvelle assertion d’événement a fait échouer huit exécutions invitées pourtant réussies ; correction et contrôles suivants réussis. Sources et journaux des deux échecs conservés. Comptages recoupés, délais inchangés. GitHub CI complète et iOS physique séparés ; Intel HVF Actions suspendu.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.
