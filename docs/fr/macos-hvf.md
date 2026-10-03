**Langues**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](../it/macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 26699fc7c6123371ff1bdf772f3c7091876e02768c97f805a3b9d0d19ca3a5db -->

[← Index de la documentation](README.md)

# Exécution CPU native sous macOS (HVF)

NeverD utilise [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor) comme équivalent macOS de KVM et WHP. `--backend hvf` le sélectionne explicitement ; `auto` l’utilise si le contrat autorise l’exécution native et si les ISA de l’hôte et de l’invité correspondent : ARM64 sur Apple Silicon, x86-64 sur Intel. Le profil `software-cpu-v1` et l’exécution inter-ISA automatique utilisent Unicorn. Un exécutable traduit par Rosetta est refusé.

Le transport nécessite macOS 11 ou ultérieur et la virtualisation matérielle. Les autres dépendances peuvent exiger une version plus récente. Un backend natif explicitement sélectionné signale son indisponibilité sans repli silencieux. [Virtualization.framework](https://developer.apple.com/documentation/virtualization) gère des machines virtuelles complètes ; NeverD a besoin du contrôle des vCPU, registres, mappings et exceptions fourni par Hypervisor.framework.

## Compilation et signature

Activez `NEVERD_ENABLE_CPU_EMULATION=ON` ou l’émulation des pilotes. `NEVERD_EMULATION_BACKEND_HVF` vaut `ON` par défaut et ne lie le framework que sous macOS. Avec `OFF`, le nom `hvf` reste reconnu, mais l’API de capacités indique `build_disabled`.

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

La [validation CPU Intel complète](https://github.com/NeverSight/NeverD/actions/runs/37106679688) reste non vérifiée : à 08:24 UTC le 2026-10-03, elle n’avait renvoyé ni résultat final ni artefact CPU après son délai configuré. La compilation et les précontrôles ne remplacent pas ce résultat. La référence du noyau macOS n’est pas une preuve sur appareil iOS.

Le petit benchmark ARM64 comparable a donné 73.9 ms pour Unicorn contre 95.1 ms pour HVF, soit environ 29 % de temps supplémentaire ; aucun gain de vitesse n’est établi. Chaque instruction ordinaire nécessite six entrées natives. Les mesures sous forte charge ne prouvent pas un débit stable. Voir le [détail des preuves](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03) et le [contrat Darwin](darwin-emulation.md), qui n’inclut pas l’ensemble des frameworks Apple.
