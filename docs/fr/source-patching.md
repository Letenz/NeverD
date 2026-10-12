**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 817e1308a48a9a751b9ef618b8094d4082fed99234c2b7381269f524196f5c80 -->

[← Documentation Index](README.md)

# Correctifs de source et objets natifs

NeverD compile une fonction de remplacement avec le compilateur choisi, puis l’installe à une entrée originale explicite. Le compilateur définit la sémantique C/C++ ; NeverD gère les adresses, les relocations, le stockage, les informations de déroulement et la publication. La réparation binaire et la réécriture du code levé gardent leurs pipelines. Le reste du programme n’est pas reconstruit.

<!-- i18n-section: compiler -->
## Choix du compilateur

Choisissez `--compiler=clang`, `--compiler=gcc` ou `--compiler=neverc`. Clang et le mode ordinaire de NeverC peuvent produire de l’IR LLVM ; `object` conserve les instructions natives. La CLI choisit les objets par défaut pour GCC. Le SDK Python exige `artifact="object"` pour GCC. Aucun autre compilateur n’est utilisé implicitement.

Remplacez les adresses des exemples par des entrées originales vérifiées. La déclaration doit respecter l’ABI machine originale, notamment la convention d’appel et la disposition des structures ; une adresse ne le prouve pas. Pour C++, utilisez `--language=c++` et le bon exécutable Clang++/G++. Préférez une entrée `extern "C"` ou indiquez son nom décoré exact. Les fonctions auxiliaires sont autorisées. `--entry` et `--func` sélectionnent explicitement le remplacement ; l’ancien `--from-c` respecte aussi l’adresse.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Profils vérifiés et limites

| Compilateur | Artefact | Profil d’exécution OS vérifié |
|---|---|---|
| Clang 22.1.6 | IR LLVM | ELF x64 C/C++ ; Mach-O x64/ARM64 C/C++ ; PE x64 C/C++ FH3 |
| Clang 22.1.6 | Objet natif | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | Objet natif | ELF x64 C/C++ avec un runtime Itanium compatible |
| NeverC 3389.1.5 mode ordinaire | IR LLVM | ELF x64 C ; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 mode ordinaire | Objet natif | ELF x64 C |

Ces profils vérifiés ne garantissent pas toutes les versions ni toutes les options. `dyncode` et NeverC C++ ne sont pas admis. GCC ne produit pas d’IR LLVM. Les objets natifs COFF/Mach-O ne sont pas admis ; l’installation d’objets exige ELF x64. Les relocations inconnues, TLS, constructeurs, LTO, symboles common/IFUNC et imports non résolus échouent explicitement. Le reçu JSON indique le compilateur, les arguments, les liaisons et les adresses installées ; les objets incluent aussi SHA-256.

La matrice CPU distincte couvre quatre architectures (`x86`, `x64`, `ARM32`, `ARM64`) et trois formats (`ELF`, `PE`, `Mach-O`), en vérifiant l’installation ou un refus explicite. L’exécution CPU isolée ne certifie ni le chargeur OS, ni le déroulement C++, ni une ABI quelconque. Consultez les profils OS ci-dessus pour ces conclusions.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` indique l’installation et l’exécution isolée de fonctions ; `REFUSED` indique un contrat d’installation manquant. ARM32 ELF exige l’enregistrement EHABI ; Mach-O 32 bits exige des contrats vérifiés de chargeur, frontend et déroulement ; l’installation source PE exige actuellement x64.

La matrice CPU utilise arithmétique, branches et fonctions auxiliaires autonomes. Elle ne certifie pas les références de données ELF x86/ARM64 ni les imports C++ ARM64 ; les sondes supplémentaires ont refusé leurs contrats de relocation non pris en charge.

<!-- i18n-section: references -->
## Références originales et stockage

Les liaisons nomment des déclarations externes : `function` exige une fonction originale ou un stub d’import vérifié ; `data` exige du stockage inscriptible ; `const` exige une déclaration IR constante. Les données des objets demandent une étendue non nulle. Celle-ci ne peut traverser les limites du stockage ni rendre RELRO inscriptible. Le rappel du corps original remplacé est refusé : ce profil n’a pas de trampoline conservant le prologue.

Le nouveau code reste RX et l’état mutable RW. Seuls les imports existants sont réutilisés ; les slots GOT/IAT et les stubs appelables sont distincts. Mach-O exige de la place pour les commandes de chargement, par exemple `-Wl,-headerpad,0x400` ; PE exige de la place pour les en-têtes de sections. Un espace insuffisant échoue. ELF `.data.rel.ro` et les protections CET/IBT originales exigent des contrats supplémentaires et sont actuellement refusés.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

La croissance des tables Mach-O exige aussi des espaces fichier et virtuels adjacents libres ; la réserve d’en-tête seule ne les fournit pas. Les fixtures distinguent ces deux exigences.

<!-- i18n-section: exceptions -->
## Exceptions C++

ELF utilise les FDE/LSDA du compilateur ; Mach-O utilise compact unwind/DWARF. PE x64 admet les tables FH3 avec un runtime MSVC existant compatible et des identités de types cohérentes. Les données type-info peuvent demander des liaisons explicites. FH4, les personalities personnalisées, la source SEH et les modes CFG/EHCont/XFG actifs sont refusés ; les protections originales sont préservées.

Les cas exécutés couvrent le retour normal, le catch interne, une nouvelle exception capturée par l’appelant original, une exception traversant le remplacement depuis un appelé original, le nettoyage RAII exactement une fois, la relance, la terminaison noexcept et les fonctions inchangées. L’appelant doit déjà permettre la propagation. Le correctif ne recrée pas les catches supprimés autour d’une fonction prouvée sans exception.

<!-- i18n-section: publication -->
## SDK et publication

L’API C additive est `neverd_patch_source` avec `neverd_source_patch_options_v1` ; `neverd_patch_source_report` renvoie le reçu. L’entrée objet fournit sa longueur en octets. L’ancienne API `neverd_patch_from_c` reste disponible.

Les entrées sont figées. Le candidat est préparé près de la destination, rechargé et vérifié pour chaque entrée avant publication exclusive. Les fichiers et alias symboliques existants ne sont pas écrasés. Une liaison manquante fait échouer toute la demande. `--mode=inplace` est refusé. Les transformations IR de session ne s’appliquent pas aux objets natifs. Mach-O vérifie et renouvelle strictement la signature ; une signature PE n’est pas supprimée silencieusement.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Compilation et vérification

Choisissez un compilateur compatible avec `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 vérifié). La compilation intégrée corrige une copie LLVM dans le dossier de compilation sans modifier le sous-module ni le cache partagé. Le défaut PLT x64 des précompilés r3/r4 peut provoquer un refus de correctif IR ; les objets natifs évitent ce chemin MC. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` exige une production PLT exacte et corrigée.

`NeverDSourcePatchTests` couvre C/API/objets/exceptions natifs ; `NeverDSourcePatchMatrixTests` vérifie toutes les combinaisons avec Unicorn. `SourcePatchPE` exécute sept scénarios C++ avec `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` et `NEVERD_TEST_WINDOWS_SDK_LIB`. Hors Windows, `NEVERD_TEST_WINDOWS_RUNNER` est aussi requis. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` transforme les entrées manquantes en échec. Le workflow Windows dédié impose l’exécution native et refuse les cas ignorés.

Les preuves Linux sur le Mac de développement utilisent Docker et un chargeur Linux explicite ; PE utilise Wine 11. Mach-O ARM64 s’exécute sur le Mac. Les résultats CI Windows natifs se vérifient séparément : configurer un job ne prouve pas sa réussite. Les comptes historiques, échecs de référence et limites des émulateurs figurent dans [les contrats détaillés et le journal de validation](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

La copie LLVM corrige aussi les références GOT de données Mach-O x64 : la table de catch typé de Clang se mesure depuis la fin du mot de relocation de quatre octets. r3/r4 la décale de quatre octets ; la vérification avant publication refuse ce résultat. Sur le Mac, x64 utilise Rosetta 2 et ARM64 s’exécute directement.
