**Langues**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 13c30cb15cacba497163d2d7027684dbbfed986b61111754b7f61993bcbaccbe -->

[← Index de la documentation](README.md)

# Exécution CPU native sous macOS (HVF)

NeverD utilise [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor) comme équivalent macOS de KVM et WHP. `--backend hvf` le sélectionne explicitement ; `auto` l’utilise si le contrat autorise l’exécution native et si les ISA de l’hôte et de l’invité correspondent : ARM64 sur Apple Silicon, x86-64 sur Intel. Le profil `software-cpu-v1` et l’exécution inter-ISA automatique utilisent Unicorn. Un exécutable traduit par Rosetta est refusé.

Le transport nécessite macOS 11 ou ultérieur et la virtualisation matérielle. Les autres dépendances peuvent exiger une version plus récente. Un backend natif explicitement sélectionné signale son indisponibilité sans repli silencieux. [Virtualization.framework](https://developer.apple.com/documentation/virtualization) gère des machines virtuelles complètes ; NeverD a besoin du contrôle des vCPU, registres, mappings et exceptions fourni par Hypervisor.framework.

## Compilation et signature

Activez `NEVERD_ENABLE_CPU_EMULATION=ON` ou l’émulation des pilotes. `NEVERD_EMULATION_BACKEND_HVF` vaut `ON` par défaut et ne lie le framework que sous macOS. Avec `OFF`, le nom `hvf` reste reconnu, mais l’API de capacités indique `build_disabled`.

La liaison du framework et la signature hypervisor sont réservées à la cible de compilation macOS (`CMAKE_SYSTEM_NAME=Darwin`). Les cibles mobiles Apple, dont iOS, ne reçoivent ni cette dépendance ni ce droit. Un profil invité iOS peut toujours utiliser HVF lorsque NeverD s’exécute sur un Mac avec la même ISA.

L’**exécutable du processus** doit porter le droit [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor). Signer uniquement `libneverd.dylib` ne suffit pas. CMake signe le CLI, le worker et les tests avec `resources/macos/neverd-hypervisor.entitlements`. `NEVERD_HVF_SIGN_IDENTITY` vaut `-` par défaut, pour une signature ad hoc ; une identité existante peut être choisie. Le packaging réapplique puis vérifie ce droit après réparation des dépendances Mach-O.

Une application intégratrice signe son propre exécutable ; NeverD ne modifie pas l’interpréteur Python installé. `cpu-capabilities --configuration=JSON --probe-host` teste le processus réel. Le worker autonome est signé par défaut ; utilisez `NEVERD_WORKER_SIGN_HVF=OFF` seulement s’il n’a pas besoin de HVF.

## Responsabilités et exécution

`backends/hvf/HvfExecutor` possède une VM par processus et un vCPU, créés, utilisés et détruits sur un thread dédié. Les CPU logiques partagent cet exécuteur et entrent séquentiellement. Un changement de CPU retire les deux régions physiques de l’ancien propriétaire avant de mapper les nouvelles. La destruction d’un CPU inactif ne retire pas les mappings d’un autre. Les liaisons sont détachées avant la libération de la RAM ; un échec partiel est annulé. Si le retrait échoue, la VM est détruite avant de libérer la mémoire ; un échec irrécupérable de destruction arrête le processus.

Les mappings hôte respectent la taille de page hôte, notamment 16 KiB sur Apple Silicon ; les tables architecturales et le budget CPU invité restent à 4 KiB. L’admission des instructions, les permissions, l’état CPU, les transactions mémoire et les services OS appartiennent à leurs couches existantes. Le worker natif n’appelle pas les observateurs invités et ne prend pas les verrous mémoire du cœur appelant.

ARM64 exécute pas à pas cinq instructions immuables de maintenance TLB/I-cache, puis l’instruction admise. `PSTATE.D` ne masque pas les exceptions de débogage dirigées vers EL2. La capture inclut registres scalaires, TLS et FP/SIMD. Intel négocie les contrôles VMCS, utilise le monitor trap, invalide les TLB et transfère l’état XSAVE complet. RIP/RFLAGS passent directement par VMCS, y compris après recréation du vCPU. CR0/CR4 respectent les masques du framework et les bits matériels obligatoires. Les sorties authentifiées de lecture CR8 sont terminées par la couche ISA ; les autres accès de contrôle échouent. Chaque vCPU initialise un `IA32_KERNEL_GS_BASE` privé et géré ; les accès MSR invités restent piégés, et MSR/SWAPGS non pris en charge restent refusés.

L’attente dans la file respecte le jeton d’arrêt et l’échéance d’origine. Une même allocation de temps couvre préparation, maintenance, entrée et capture. `RunDeadline` attend l’acquittement des interruptions avant de rendre la main ; l’annulation recrée le vCPU pour isoler les réveils tardifs. Les interruptions hôte Intel sans rapport reprennent dans la même génération d’annulation. Les erreurs de capture et exceptions authentifiées gardent priorité sur un arrêt concurrent ; aucun état annulé ordinaire n’est publié. Il s’agit d’une annulation coopérative, sans garantie temps réel stricte.

## Validation reproductible

Utilisez Release avec CMake, Ninja, Python 3, Clang, `ld.lld`, `lld-link`, `ld64.lld` et `codesign`. Les trois linkers LLVM produisent les fixtures ELF, PE et Mach-O. Une fixture native obligatoire manquante fait échouer la validation.

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon peut ajouter `-DNEVERD_LLVM_PREBUILT=ON` ; Intel compile la révision LLVM épinglée. Le workflow [HVF](../../.github/workflows/hvf.yml) accepte les runners `self-hosted, macOS, ARM64/X64, hvf` et l’option `hosted-intel` sur `macos-15-intel`. Il vérifie d’abord la création/destruction réelles d’une VM et d’un vCPU. `validation=probe` n’établit pas l’exécution d’instructions ; `transport` ne couvre que le transport ; `darwin` exige chaque charge Darwin native ; `full` exige les deux validations complètes CPU et Darwin.

Le transport exige 12 cas sur ARM64 ou 10 sur Intel ; la validation CPU complète en exige respectivement 16 ou 14. La couverture comprend états complets, privilèges, permissions, franchissement de pages, alias, changement de CPU, rollback, annulation et reprise. Intel vérifie CR8 avant la grande compilation. Les artefacts conservent inventaire, révision, hôte, résultats et tentatives distinctes. [GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) considère la virtualisation imbriquée comme expérimentale ; un Mac natif dédié reste préférable pour une validation répétable.

Sur Intel hébergé, `--execution-methods` exécute séquentiellement chaque méthode GoogleTest avec tous les paramètres du registre CTest, les mêmes drapeaux, environnement et répertoire. Les propriétés inconnues sont refusées. Le délai total par méthode est plafonné à 120 secondes, sans délai séparé par paramètre ; le groupe de processus est ensuite récupéré dans un délai borné. XML brut, correspondances de noms et états de sortie sont conservés. Un délai dépassé ou XML incomplet produit un échec partiel ; toute obligation native manquante ou ignorée échoue. Les runners dédiés gardent les processus et délais CTest par cas.

## Résultats et limites

Les lignes se recouvrent et ne s’additionnent pas. État documenté le 2026-10-03 :

| Périmètre | Source | Réussis | Échecs | Ignorés | Obligatoires natifs |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 CPU complet, 20 cibles | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| ARM64 Darwin | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel Darwin | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| Cible FP Intel complète | `3e01cda5c` | 35 | 0 | 36 | 12 cas HVF |

ARM64 a réconcilié 6,842 inscriptions ; Intel en compte 6,840 dans les mêmes 20 cibles. Les 253 cas natifs d’exception, division et transition d’état Intel ont aussi réussi. Le [résultat Darwin Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) vérifie 286 identités et 32 processus, plus dix cas de transport, 100 reprises et CR8. Les 124 contrôles du collecteur et des audits ont réussi. Les vérifications d’intégration couvrent le CLI, les SDK, le worker et la signature de 186 images Mach-O ; les dépendances de ce bundle exigent macOS 15.0.

La validation CPU Intel complète reste ouverte. L’[exécution antérieure](https://github.com/NeverSight/NeverD/actions/runs/37106679688) s’est terminée le 2026-10-03 à 08:35 UTC ; GitHub a signalé la perte de communication du runner, sans artefact CPU. La compilation et les précontrôles ne remplacent pas le résultat complet. La référence du noyau macOS n’est pas une preuve sur appareil iOS.

Le petit benchmark ARM64 comparable a donné 73.9 ms pour Unicorn contre 95.1 ms pour HVF, soit environ 29 % de temps supplémentaire ; aucun gain de vitesse n’est établi. Chaque instruction ordinaire nécessite six entrées natives. Les mesures sous forte charge ne prouvent pas un débit stable. Voir le [détail des preuves](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03) et le [contrat Darwin](darwin-emulation.md), qui n’inclut pas l’ensemble des frameworks Apple.

## Inventaire Intel complet réparti en lots

L’exécution complète `37106679688` s’est terminée le 2026-10-03 à 08:35 UTC avec une annotation GitHub signalant la perte de communication du runner. Les quatre jobs de `37116327329` ont également perdu la connexion, sans produire de XML CPU. Ces faits ne permettent pas d’identifier une instruction invitée fautive. Le mode Intel hébergé `full` utilise quatre jobs, dont deux au maximum en parallèle. Chacun exécute quatre lots successifs : seize partitions au total, numérotées `job + 4 × batch`, conservant la charge initiale de chaque job. Chaque lot compile et vérifie d’abord l’inventaire CTest complet des vingt cibles. `--hvf-shard INDEX/COUNT` garde chaque méthode entière et tous ses paramètres ensemble, même si leurs propriétés d’exécution diffèrent.

Avant l’exécution CPU, `scripts/prepare_hvf_batches.py` enregistre l’inventaire complet, les sélections et les plans de méthodes ; le workflow téléverse ces diagnostics séparément. Une action composite locale exécute les quatre lots et téléverse immédiatement, après chacun, le XML original, les correspondances d’identités, les états de processus et la liste autorisée des variables nécessaires. Un délai externe unique de 30 minutes couvre les quatre lots et leurs téléversements. Un échec empêche l’exécution des lots suivants. Après échec ou expiration, un téléversement diagnostique distinct dispose de deux minutes tant que le runner reste joignable ; en cas de perte de connexion, seules les preuves déjà téléversées subsistent. Les plans et diagnostics incomplets ne comptent pas comme partitions réussies.

Un job Linux distinct exécute `scripts/audit_hvf_shards.py` et redérive les cibles et obligations natives depuis les sources extraites. Le workflow ne télécharge que les artefacts CPU de la tentative courante ; l’audit exige les seize partitions d’une même révision sans modification locale, la bonne ISA macOS et des contrats normalisés identiques. Les résultats doivent être disjoints et couvrir exactement l’inventaire complet ; tous les processus doivent terminer correctement et chaque obligation native réussir. Partition absente, filtre modifié, résumé incohérent, XML incomplet ou obligation native ignorée font échouer le contrôle. Chaque job natif conserve transport, reprise, CR8 et validation Darwin indépendante. Les runners dédiés gardent CTest sans répartition. Les lots ne prouvent pas à eux seuls l’acceptation Intel. Un nouvel essai doit relancer tous les jobs natifs ; les artefacts des tentatives précédentes ne sont pas combinés.

Le premier lot CPU vérifié de l’[exécution `37123148209`](https://github.com/NeverSight/NeverD/actions/runs/37123148209), source propre `f5f29a484`, est la partition `1/16` : 476 résultats enregistrés dans 31 processus de méthodes, dont 82 réussis, 0 échec et 394 ignorés ; son unique cas natif obligatoire a réussi. Le SHA-256 de l’artefact `11274755752` a été vérifié ; le XML original, les états de sortie, l’inventaire et le plan des méthodes ont été rapprochés du plan préalable. Cette preuve Intel partielle ne clôt pas la validation CPU complète.

## Dernière validation native locale

Sources sans modification locale `4ce0b8247`, le 2026-10-03 UTC. L’inventaire ARM64 complet a réussi dans 503 processus de méthode ; chaque résultat XML original, contrat d’exécution et arrêt de processus enfant a été vérifié indépendamment. La validation Darwin indépendante a également réussi. Les lignes se recouvrent et ne s’additionnent pas.

| Périmètre | Inscrits | Réussis | Échecs | Ignorés | Obligatoires natifs |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU, inventaire complet | 7,003 | 867 | 0 | 6,136 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-current-4ce0-full-evidence/summary.json` · `build-hvf-native/hvf-current-4ce0-darwin-evidence/summary.json`

## Diagnostic Intel indépendant

Le [workflow de diagnostic Intel](../../.github/workflows/hvf-intel-diagnostic.yml), déclenché manuellement, extrait séparément le contrôleur et les sources testées. `source-ref` exige un SHA complet ; `shards` choisit parmi les seize partitions d’origine. `first-method` commence à zéro, `method-count=0` sélectionne les méthodes restantes et `case-index` ne peut sélectionner un paramètre d’origine que si `method-count=1`. L’inventaire complet des vingt cibles est découvert avant la sélection. Le build Release, les commandes, les paramètres et les exigences natives d’origine sont conservés.

`intel-image` choisit `macos-15-intel` par défaut ou `macos-26-intel` pour une comparaison contrôlée, comme dans le workflow complet. Le titre de l’exécution indique l’image choisie et le contrôle de disponibilité exige toujours un hôte x86-64 natif. Changer d’image modifie le système, le SDK et la chaîne d’outils ; cela n’isole pas un changement du noyau.

La compilation dispose de 120 minutes dans un job limité à 180 minutes : le premier build complet sur macOS 26 a pris 76 minutes. Chaque méthode native reste limitée à 120 secondes et l’action diagnostique à 30 minutes.

Avant chaque méthode, l’action téléverse son plan immuable et un instantané de l’hôte. Après l’exécution, elle conserve le XML original, la terminaison des processus, l’état du contrôleur et un second instantané. Ceux-ci indiquent mémoire, swap, charge, disque et identifiants, état, CPU, RSS et noms d’exécutables des processus, sans arguments ni environnement. Les erreurs de collecte restent visibles. Un échec d’exécution ou de téléversement arrête les méthodes suivantes. Chaque méthode conserve sa limite de 120 secondes ; un hôte injoignable peut empêcher nettoyage et téléversement final. Seules les preuves déjà téléversées subsistent alors. Ces diagnostics partiels ne satisfont ni la validation CPU complète ni la validation Darwin indépendante. Le dernier marqueur de départ situe une limite d’exécution, sans identifier l’instruction fautive ou la cause.

Le workflow complet `Native macOS HVF` accepte aussi un `source-ref` facultatif. Par défaut, il utilise le commit du workflow ; une valeur explicite doit être un SHA complet. Les jobs natifs et l’audit agrégé extraient et vérifient les mêmes sources. L’audit confronte les preuves au commit testé, même si le contrôleur utilise une autre révision.

Pour `hosted-intel`, le workflow complet accepte `intel-image=macos-15-intel` (par défaut) ou `macos-26-intel`, tous deux répertoriés dans les [images officielles des runners](https://github.com/actions/runner-images). Cela permet de comparer explicitement les environnements hôtes avec le même `source-ref` ; l’image modifie aussi le système, le SDK et les outils. Les exigences VM/vCPU, transport natif, CR8, CPU complet et Darwin restent identiques. Le choix d’une image ne prouve à lui seul ni la stabilité ni une correction à l’exécution.

`sample-active-child=true` conserve facultativement un instantané scellé cinq secondes après avoir observé l’enregistrement du processus enfant natif : une seconde d’échantillonnage des piles du processus enfant natif dont l’identité a été vérifiée, au plus 1 MiB de la fin de son journal courant et l’état de l’hôte. La valeur par défaut est `false`. L’échantillonnage accepte au plus 166 méthodes par job pour respecter la limite d’artifacts ; sa commande est limitée à vingt secondes et son rapport à 1 MiB. Les échecs d’identification et de collecte sont enregistrés, y compris un code de sortie nul sans rapport de pile. Le téléversement utilise un répertoire immuable distinct ; son échec annule le processus enfant natif et fait échouer l’action. L’échantillonnage modifie l’ordonnancement et constitue une preuve partielle instrumentée. Il ne réinitialise pas le délai initial de la méthode et ne remplace pas la validation complète.

Le workflow complet accepte aussi `recovery-repetitions=1000` pour une investigation ciblée des interruptions et de la récupération ; la valeur par défaut reste `100`. Ce choix porte le budget de l’étape de répétition de trois à dix minutes. Chaque test natif conserve son délai initial, ses assertions et l’arrêt au premier échec. Le titre de l’exécution indique ce nombre accru de répétitions. Celles-ci ne remplacent pas les validations complètes CPU ou Darwin.

Le [workflow de diagnostic de récupération Intel](../../.github/workflows/hvf-intel-recovery.yml) séparé ne construit que `NeverDHvfTests` et exécute le filtre original `HvfExecutor.Native*` dans un seul processus, avec `repetitions=100` ou `1000`. Il exige un `source-ref` exact, une vérification native Intel VM/vCPU, Release, HVF activé et Unicorn désactivé. Le contrôleur, les sources testées et l’outil officiel de téléversement des artifacts sont extraits séparément.

L’action téléverse le plan avant l’exécution. Elle conserve ensuite un marqueur de processus initial, la progression après au moins 25 répétitions supplémentaires terminées et, au plus une fois, un instantané de stagnation si une sortie nouvelle n’a pas encore été sauvegardée. Elle regroupe la progression pendant les téléversements, limite les artifacts de progression à 42 et chaque copie de journal à 1 MiB, et ne téléverse que des répertoires figés. Les téléversements ne suspendent pas les répétitions et ne réinitialisent pas leurs temporisateurs ; ils influencent toutefois l’ordonnancement de l’hôte. Ces preuves restent donc partielles et instrumentées.

Le succès exige des itérations consécutives de 1 au nombre demandé, les enregistrements RUN/OK/PASSED appariés du test natif exact à chaque itération, aucun échec ni saut, un code de sortie nul et la confirmation de l’arrêt du processus. Un XML écrasé à chaque répétition ne suffit pas. L’exécution native dispose de trois ou dix minutes au total, puis de 30 secondes supplémentaires pour le nettoyage du contrôleur ; l’action est limitée à 20 minutes. Un échec de téléversement annule l’exécution ; l’annulation termine le groupe de processus natif et l’outil de téléversement. Les preuves finales sont envoyées si l’hôte reste joignable. Les instantanés incomplets ou tronqués ne valident jamais l’ensemble CPU ou Darwin.
