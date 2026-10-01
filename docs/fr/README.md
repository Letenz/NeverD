**Langues**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 62a52f8c335f25f75bf285bd21350fb9d787a00b7b9a357f399c3df72c4c52cc -->

[← Projet NeverD](project.md)

# Documentation NeverD

L’aperçu du projet, la compilation et le CLI se trouvent dans le README du dépôt. Les références de conception et de test destinées aux contributeurs sont regroupées ici.

**Mobile (CLI expérimentale) :** `neverd mobile` récupère Java depuis les APK, DEX et smali [Android](android.md), et le C natif ainsi que les sources Objective-C/Swift prises en charge depuis les IPA, `.app` et Mach-O [iOS](ios.md). Les rapports JSON décrivent résultats et couverture. Commencez par la [vue d’ensemble mobile (anglais)](../mobile.md), puis consultez les commandes et limites des guides de plateforme.

Les guides anglais se trouvent directement dans `docs/`. Les traductions sont regroupées dans `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` et `zh-TW/`. Chaque répertoire contient l’index `README.md`, la présentation `project.md`, les guides thématiques, `CONTRIBUTING.md`, `ATTRIBUTION.md` et `roadmap.md`. Les images partagées restent dans `assets/`.

L’exécution CPU sépare admission ISA, mémoire invitée, transport du moteur et politique OS. `NEVERD_ENABLE_CPU_EMULATION` active la couche CPU x64/ARM64 ; `NEVERD_ENABLE_DRIVER_EMULATION` ajoute l’environnement Windows WDM/KMDF x64 borné. `linux-elf64-v1` exécute les processus Linux ELF pris en charge. Voir [Exécution CPU](cpu-execution.md), [Émulation de processus invités](process-emulation.md) et [Émulation des pilotes Windows](driver-emulation.md).

`driver-strict` / `checked-x64-v1` accepte KVM sur un hôte Linux x64 compatible et WHP sur un hôte Windows x64 compatible ; `auto` sélectionne ce transport natif, et les ISA différentes utilisent Unicorn. Unicorn explicite et l’API V1 conservent le profil logiciel portable. L’exécution native vérifie les adresses canoniques et les effets avant l’entrée ; le matériel indisponible provoque un échec sans repli. Instructions et comportements OS non pris en charge échouent explicitement. Les preuves natives ARM64/WHP restent manquantes ; aucune compatibilité universelle des pilotes ou Android/Darwin n’est établie.

`checked-aarch64-v1` et `checked-user-aarch64-v1` fournissent FP32/FP64 et SIMD fixes bornés, avec état FPCR/FPSR/vectoriel complet. Les hôtes Linux ARM64 correspondants utilisent KVM, Windows ARM64 utilise WHP et une autre ISA utilise Unicorn. Les preuves natives ARM64 restent attendues ; les pilotes Windows sont chargés uniquement en x64.

| Document | Description |
|----------|-------------|
| [README (français)](project.md) | Aperçu, démarrage rapide, compilation, SDK, CLI |
| [Contribution](CONTRIBUTING.md) | Environnement, profils de compilation, workflow, style et exigences de PR |
| [Architecture](architecture.md) | Parcours IR, frontières, lifting strict, profondeur de support et points de modification |
| [Tests](testing.md) | Suites, fixtures générées, allers-retours Unicorn et commandes incrémentales |
| [Atelier de bureau (anglais)](../gui.md) | Interface Qt Quick facultative, worker séparé, ABI C, annotations et workflows MCP |
| [Validation du bureau (anglais)](../gui-qualification.md) | Mesures GUI, limites du packaging et validations de plateforme restantes |
| [Récupération de sources à partir d’un interpréteur](interpreter-recovery.md) | Spécialisation expérimentale `--devirtualize`, contrôles CLI, contrat d’exécution, preuves et limites; propositions de preuve pour boucles imbriquées; budgets de découverte explicites et API C versionnée; API de preuve exacte du natif vers LLVM |
| [Reconstruction des exceptions Windows](windows-exception-reconstruction.md) | Matrice de support SEH/C++, contrat IR, règles de patch natif et validation PE |
| [Exécution CPU](cpu-execution.md) | Configuration, capacités, disponibilité des backends et résultats typés |
| [Preuves bitvector](solver.md) | Preuves Z3 facultatives, synthèse vérifiée, tests indépendants et export |
| [Émulation de processus invités](process-emulation.md) | Profil Linux ELF, démarrage, services, limites et tests |
| [Émulation des pilotes Windows](driver-emulation.md) | Cycle WDM/KMDF x64 borné, requêtes, scénarios matériels, SEH, sous-ensembles PnP, choix du moteur et limites |
| [Audit et chasse de sûreté mémoire](memory-safety.md) | Analyse de durée de vie du tas et de débordement de copie : contrat d’identité par format, catalogue puits/sources, verdicts, budgets et schéma JSON |
| [Plugins natifs](plugins.md) | ABI de descripteur en C pur, callbacks et événements, procédure de compilation/liaison, découverte et règles de compatibilité |
| [Plugins Python](python-plugins.md) | Création, API de session et d’événements, isolation, tests et publication |
| [Vue d’ensemble mobile (anglais)](../mobile.md) | CLI expérimentale Android/iOS, entrées, sorties, rapports et limites |
| [Reconstruction Java pour Android](android.md) | APK (y compris multidex), DEX, fichiers/répertoires smali → Java ; CLI, rapport JSON, dépannage, limites et vérification |
| [Récupération des sources iOS](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → C natif et sources Objective-C/Swift prises en charge ; dispositions, CLI/export, couverture JSON, limites et tests exécutés |
| [Décompilation EVM](evm.md) | Entrées, hardforks, IR par étapes, ABI host C/LLVM, reconstruction Solidity et limites |
| [Décompilation Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, sorties C/Rust, vérification et limites connues |
| [Feuille de route](roadmap.md) | État : formats natifs, EVM et Solana SBF implémentés |
| Documentation traduite | Les liens de langue ci-dessus ouvrent l’index et la présentation de chaque langue |
