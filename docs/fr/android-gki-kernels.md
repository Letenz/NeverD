# Contrats des noyaux Android GKI publiés

NeverD privilégie les branches Android GKI publiées de 5.10 à 6.18 avant les autres variantes Linux. La requête sélectionne explicitement une branche :

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Le contrat API 28 du profil Android natif décrit les imports Bionic sans sélectionner le noyau. Le choix GKI contrôle les contrats implémentés de `pidfd_open`, de sortie vectorielle, d’horloges CPU de processus encodées et de `ppoll` de descripteurs de processus sans attente. Il ne certifie ni ne démarre un noyau complet et ne déduit aucun périphérique, espace de noms, droit ou inventaire de processus. Les services non pris en charge s’arrêtent explicitement. Voir la [politique GKI officielle](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Révisions sources figées

`LinuxGKIKernels.def` suit ces étiquettes officielles `r1`, vérifiées le 2026-10-07. Les commits immuables fournissent `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c` et `fs/read_write.c`. Les drapeaux proviennent de l’UAPI et de la validation des appels, pas du niveau API Android ou du noyau hôte. Les sources CPU figées figurent dans le tableau ci-dessous.

| Branche demandée | Étiquette publiée | Commit source figé | Drapeaux admis | Import iovec |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [Copier toutes les métadonnées en premier](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copier toutes les métadonnées en premier](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copier toutes les métadonnées en premier](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copier toutes les métadonnées en premier](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copier toutes les métadonnées en premier](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Chemin à tampon unique](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` et `PIDFD_THREAD` (`0x80`) | [Chemin à tampon unique](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` et `PIDFD_THREAD` | [Chemin à tampon unique](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

Ces révisions définissent le contrat. Une nouvelle version ou un rétroportage exige une vérification des sources et des régressions. GKI associé à une observation déclarant `pidfd_open` absent est rejeté avant chargement.

## Sous-ensemble des descripteurs de processus

Les traps x64/AArch64 et `syscall` de Bionic partagent `LinuxServices` et la table de descripteurs de la charge. Seuls les 32 bits bas des PID/drapeaux comptent ; drapeaux inconnus et PID signés non positifs renvoient `EINVAL` avant allocation. Le tableau facultatif `tasks` est un catalogue fixe et fermé des autres tâches invitées vivantes :

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

Le chef de groupe courant PID 1000 est implicite, même avec un tableau vide. Sans catalogue, la recherche de cibles étrangères reste non prise en charge ; un PID positif valide hors catalogue déclaré renvoie `ESRCH` avant allocation. Sans `PIDFD_THREAD`, un non-chef vivant renvoie `EINVAL` en 5.10–6.12 et `ENOENT` dans [le `pidfd_prepare` 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c). Avec le drapeau accepté, 6.12/6.18 ouvrent les non-chefs déclarés. Les drapeaux sont toujours vérifiés avant la cible.

Chaque entrée exige une `id` entière de 1..2147483647 et un booléen `group_leader`, avec 4096 entrées au maximum. Doublons, champs supplémentaires, PID 1000 non-chef et catalogue sans GKI sont rejetés. Les priorités doivent désigner une tâche déclarée ou le processus courant. Le catalogue fixe est incompatible avec les threads Android coopératifs (`thread_limit > 1`) ; création, destruction, droits et traduction des espaces de noms nécessitent leur propre gestion de durée de vie.

`linux_files` est requis : fichiers et pidfds partagent propriétaires et limite. Le plus petit numéro libre est alloué ; saturation : `EMFILE`, `close` libère le numéro, deuxième fermeture : `EBADF`. Un flux standard fermé peut être réutilisé. Aucun pidfd, système de fichiers ou processus hôte n’est consulté.

Sur un pidfd valide, `read`/`write` renvoient `EINVAL` avant accès aux données ; `lseek` renvoie `ESPIPE` après validation de l’origine. `writev` importe et valide d’abord les métadonnées et plages utilisateur : `EFAULT` peut précéder le `EINVAL` de l’écriture absente. Aucune donnée n’est lue ou capturée. stdout/stderr utilisent le même importateur versionné ; voir l’[ordre VFS](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

5.10/5.15/6.1 copient tout le tableau iovec avant les longueurs : une longueur négative suivie de métadonnées inaccessibles donne `EFAULT`. Les étendues originales sont vérifiées avant plafonnement, même pour un vecteur. 6.6/6.12/6.18 vérifient successivement et donnent ici `EINVAL` ; le chemin à tampon unique plafonne avant contrôle, le chemin multivecteur conserve les contrôles originaux. Ces règles viennent de `copy_iovec_from_user`, `__import_iovec` et `import_ubuf`. Sans GKI, la politique existante à tampon unique reste inchangée, sans version supposée.

Bionic convertit les erreurs négatives brutes en `-1` et `errno` local au thread ; le succès préserve `errno`. Les métadonnées de `fstat` manquent. Polling bloquant, notifications de sortie, signaux via pidfd, `pidfd_getfd`, `fcntl`, ioctl pidfs et tâches non observées restent non pris en charge. Ni ordonnanceur ni durée de vie ne sont déduits d’un pidfd.

## Sous-ensemble de polling sans attente

Avec GKI sélectionné, les appels bruts x64/AArch64 `ppoll` et le `syscall` variadique Bionic admettent une timespec explicitement nulle et un masque temporaire de signaux null. `linux_files.descriptor_limit` fournit la borne invitée RLIMIT_NOFILE ; les 32 bits non signés de poids faible de `nfds` ne peuvent la dépasser. La table commune n’indique aucune disponibilité pour les pidfd vivants observés, ignore les descripteurs négatifs et renvoie `POLLNVAL` (`0x20`) pour les descripteurs fermés. Chaque entrée non nulle compte séparément, doublons compris. La disponibilité non observée des autres types provoque un arrêt explicite.

La copie et la validation du délai précèdent masque et descripteurs. Un masque non null fait vérifier sa taille sur toute sa largeur et son étendue lisible avant la limite non prise en charge ; un masque null ignore la taille. Toutes les métadonnées des descripteurs sont importées avant sélection et sortie. Seuls les champs `revents` de 16 bits sont écrits dans l’ordre des entrées ; une faute ultérieure conserve les écritures antérieures. Les accès mixtes au sein d’un champ gardent la limite commune des copies partielles non prises en charge. Même un tableau vide subit la vérification finale de l’étendue utilisateur. La timespec nulle n’est pas réécrite et peut être lisible en lecture seule. L’appel ne lit ni ne fait avancer l’horloge murale.

Les règles ont été vérifiées aux huit versions immuables dans `fs/select.c` : `ppoll`, `do_sys_poll`, `do_pollfd`, `poll_select_set_timeout` et `poll_select_finish`. La disponibilité des pidfd vivants suit `pidfd_poll`, dans `kernel/fork.c` pour les six premières versions et pidfs pour les deux dernières :

[5.10 fs/select.c](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/fs/select.c) · [6.18 fs/select.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/select.c) · [6.6 kernel/fork.c](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/fork.c) · [6.12 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/pidfs.c) · [6.18 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/pidfs.c)

Les notifications de sortie et de récupération diffèrent selon les versions et restent hors du sous-ensemble de tâches vivantes fixes. Les attentes bloquantes, masques temporaires, signaux et wrappers Bionic `ppoll` nommés exigent leurs propres contrats.

## Sous-ensemble des horloges CPU de processus

Avec un GKI explicite, `clock_gettime` accepte les identifiants négatifs encodés PROF, VIRT et SCHED, en interprétant les 32 bits bas signés. Le PID et le type désignent un échantillon explicite de `linux_time`. Le processus courant est implicite ; les autres doivent être déclarés comme chefs de groupe vivants dans le catalogue fermé avant de fournir leur échantillon.

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

`-16006` désigne SCHED pour le PID 2000. PROF et VIRT sont indépendants. Pour le processus courant, SCHED 2, -6 (PID zéro) et -8006 (PID 1000) partagent un échantillon ; les alias PROF sont -8/-8008 et VIRT -7/-8007. Les doublons sont refusés même à valeur égale. Les secondes CPU sont positives ou nulles, les nanosecondes normalisées. L’avancement au repos ne modifie que les horloges murales 0, 1 et 7 ; les échantillons CPU restent fixes. L’exécution ne déduit aucune consommation CPU.

Le TID propre à la tâche courante désigne aussi son groupe, y compris avec les threads Android coopératifs sans catalogue étranger. Un PID étranger absent du catalogue fermé ou vivant sans être chef renvoie `EINVAL` avant tout accès de sortie. Catalogue omis ou échantillon absent d’un groupe connu : arrêt non pris en charge avant la copie. Un type invalide renvoie `EINVAL` ; un échantillon valide peut rencontrer `EFAULT` dans la copie utilisateur. Les traps bruts conservent les erreurs négatives ; Bionic seul actualise errno et renvoie -1.

Les règles suivent `pid_for_clock`, `posix_cpu_clock_get`, le répartiteur et les définitions d’identifiants de chaque révision figée :

| Branche demandée | Source des horloges CPU de processus |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

Les versions fixées de `init/Kconfig` activent les temporisateurs POSIX par défaut ; les defconfig GKI ne les désactivent pas. Voir [6.18 init/Kconfig](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/init/Kconfig).

La distinction FD et le routage CPU suivent aussi le [répartiteur 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c) et les [définitions des identifiants](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h) figés. Les horloges FD et les horloges CPU encodées par thread restent non prises en charge. Le catalogue est une observation invitée fixe ; droits, espaces de noms, durée de vie et comptabilité CPU nécessitent leurs propres contrats.

## Validation et couverture future

`LinuxPIDFDTests.cpp` exécute des ELF x64/AArch64 indépendants O0/O2 pour les huit branches et transports disponibles : drapeaux, table partagée, limites/réutilisation, ordre des erreurs, métadonnées inaccessibles, plafonnement/étendues originales, catalogues omis/fermés, non-chefs et recherche avant saturation. `AndroidSyscallTests.cpp` répète propriété raw/Bionic, recherche et errno sur six profils O0/O2 avec relocations ordinaires, Android packed et RELR. Sources et exécutions prouvent ce sous-ensemble ; aucun démarrage natif de chaque image GKI n’est établi. Étendre Linux service par service en conservant versions, configurations et observations.

Les cas CPU vérifient identité, ordre de sortie, types indépendants, observations explicites et séparation du repos mural. `AndroidTimeTests.cpp` vérifie sorties nommées/brutes et sentinelles ; le syscall coopératif vérifie l’alias du TID courant non chef.

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` vérifie les appels bruts O0/O2 des huit GKI : descripteurs vivants, négatifs ou fermés, doublons, réduction des arguments, ordre délai/masque, timespec nulle en lecture seule, import complet avant sélection et conservation des premiers `revents` lors d’une faute ultérieure. `ZeroTimeoutPollKeepsUnobservedBoundaries` maintient les limites inconnues du noyau, des ressources, masques, attentes et disponibilités. `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` vérifie sous Android la table commune et errno dans six profils de compactage.
