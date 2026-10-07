# Contrats des noyaux Android GKI publiés

NeverD privilégie les branches Android GKI publiées de 5.10 à 6.18 avant les autres variantes Linux. La requête sélectionne explicitement une branche :

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

Le contrat API 28 décrit les imports Bionic, sans choisir de noyau. GKI ne contrôle actuellement que `pidfd_open` et les sorties vectorielles implémentées. Il ne certifie pas un noyau complet, ne charge aucune image et ne déduit ni périphériques, ni espaces de noms, ni droits, ni inventaire des processus. Les services non pris en charge échouent explicitement. Voir la [politique GKI officielle](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Révisions sources figées

`LinuxGKIKernels.def` suit ces étiquettes officielles `r1`, vérifiées le 2026-10-07. Les commits immuables fournissent `kernel/pid.c`, `include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`, `kernel/fork.c`, `lib/iov_iter.c` et `fs/read_write.c`. Les drapeaux proviennent de l’UAPI et de la validation des appels, pas du niveau API Android ou du noyau hôte.

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

Bionic convertit les erreurs négatives brutes en `-1` et `errno` local au thread ; le succès préserve `errno`. Les métadonnées de `fstat` manquent. Polling, notifications de sortie, signaux via pidfd, `pidfd_getfd`, `fcntl`, ioctl pidfs et tâches non observées restent non pris en charge. Ni ordonnanceur ni durée de vie ne sont déduits d’un pidfd.

## Validation et couverture future

`LinuxPIDFDTests.cpp` exécute des ELF x64/AArch64 indépendants O0/O2 pour les huit branches et transports disponibles : drapeaux, table partagée, limites/réutilisation, ordre des erreurs, métadonnées inaccessibles, plafonnement/étendues originales, catalogues omis/fermés, non-chefs et recherche avant saturation. `AndroidSyscallTests.cpp` répète propriété raw/Bionic, recherche et errno sur six profils O0/O2 avec relocations ordinaires, Android packed et RELR. Sources et exécutions prouvent ce sous-ensemble ; aucun démarrage natif de chaque image GKI n’est établi. Étendre Linux service par service en conservant versions, configurations et observations.
