**Langues**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](interpreter-recovery.md) | [Deutsch](../de/interpreter-recovery.md) | [Español](../es/interpreter-recovery.md) | [Italiano](../it/interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Récupération de sources à partir d’un interpréteur

[← Index de la documentation](README.md)

L’étape expérimentale de spécialisation d’interpréteur élimine la distribution
résolue statiquement dans une fonction x64 déjà liée, tout en conservant ses
entrées d’exécution, effets mémoire, branches et boucles. Elle s’appuie sur la
sémantique des instructions, sans signatures de handlers ni table d’opcodes
propre à un logiciel de protection.

```sh
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --llvm -o recovered-llvm.c
```

`--vm-control` sélectionne des registres généraux entiers qui distinguent les
contextes de l’interpréteur. L’option est répétable et ne fournit aucune valeur
concrète. Sélectionnez, par exemple, un curseur de bytecode dont la valeur est
établie par le code d’entrée. Une entrée d’exécution utilisée comme compteur
doit rester dynamique. Une séparation insuffisante des contextes peut arrêter
la récupération lorsque des valeurs distinctes du curseur se rejoignent ; le
moteur ne doit pas compenser en devinant une destination. Pour un curseur stocké
sur la pile, `--vm-control-stack=-16:8` sélectionne huit octets à RSP d’entrée
moins 16. Ce décalage est relatif à l’entrée de la fonction, et non au pointeur
de pile après ses ajustements.

L’API C est `neverd_devirtualize_source_v1()`, déclarée dans
`neverd/sdk/NeverDCAPIDevirtualize.h`. Elle exécute une transaction distincte sans
modifier le cache de décompilation ordinaire de la session. En cas d’échec, elle
ne renvoie aucun source, mais peut fournir un diagnostic JSON. Les deux chaînes
allouées se libèrent avec `neverd_free_string()`.

## Contrat d’exécution

L’adaptateur binaire accepte actuellement les images x64 ELF et PE déjà liées,
à leurs adresses de mappage. Mappages, octets et permissions doivent rester
fixes, sans modification concurrente. Le lifting strict à la demande suit le
code machine accessible. Les instructions non prises en charge, appels,
opérations opaques, accès mémoire ordonnés, contrôles non résolus et traitements
des exceptions du langage arrêtent la récupération.

Seules des plages complètes en lecture seule, adossées au fichier et dépourvues
de mappages superposés ou de corrections du chargeur, peuvent fournir des
lectures constantes de l’image. Les tables modifiables, relocations non résolues
et instantanés d’exécution ne prouvent pas l’immuabilité des lectures. Les
relocations COPY et métadonnées d’exceptions incomplètes sont refusées par
prudence. Le domaine de validité exige des retours ABI ordinaires : la plage
cible de toute écriture issue de données externes doit être disjointe de
l’emplacement de l’adresse de retour à l’entrée. C’est une précondition explicite
de l’appelant et de l’environnement, y compris pour les adresses calculées à
partir d’entiers externes ; l’absence de provenance liée au cadre de pile ne
prouve pas l’absence de recouvrement numérique. Les adresses d’écriture dérivées
du cadre doivent prouver cette disjonction, et le pointeur de pile original doit
être restauré au retour. La provenance survit aux sauvegardes sur la pile et
aux jonctions ; perdre une expression affine ne la transforme pas en pointeur
externe. Les pivots de pile, retours où l’appelé dépile les arguments et
distributions fondées sur RET sont actuellement refusés. L’adaptateur impose
la sémantique x64 petit-boutiste.

Cette fonction produit du source et de l’IR pour l’analyse. Elle ne prouve pas
la sûreté des relocations, du déroulement de pile, des exceptions asynchrones
ou du remplacement binaire. Le mode patch refuse cette option. La prise en
charge de tous les interpréteurs ou de toutes les configurations de protection
n’est pas garantie.

## Limites actuelles

Les adresses de bytecode dépendant des entrées et les relations entre états du
décodeur ne sont pas résolues dans le cas général. Une branche ou une boucle
dynamique peut être récupérée lorsque ses cibles de distribution sont prouvées,
mais la couverture des branches ordinaires ne démontre pas la prise en charge
de tous les schémas de décodage indirect. Un contrôle non résolu ou un budget
épuisé constitue un échec : aucun source récupéré ni remplacement partiel n’est
publié. Les appels à des fonctions auxiliaires natives, frontières d’exception
ou de réentrée, code modifiable et autres architectures restent hors du contrat
d’exécution de cet adaptateur initial.

## Implémentation partagée

`SpecializationProvider` fournit des instructions intégralement liftées et les
preuves justifiant les lectures immuables. `NeverDInterpreterSpecialization`
utilise la sémantique existante de `SymExec` pour évaluer partiellement les
opérations entières et de contrôle. L’adaptateur binaire gère le mappage et le
décodage ; il n’implémente pas un second évaluateur d’instructions.

Un nœud est identifié par son curseur natif, le mode d’instruction et les
constantes sélectionnées des registres de contrôle et des emplacements du cadre
d’entrée. Les autres faits à l’échelle de l’octet se combinent par intersection.
Lorsqu’un fait entrant s’affaiblit, le nœud est réévalué. Ainsi, les boucles du
programme restent des boucles au lieu de développer chaque itération observée.
Toutes les feuilles d’une destination indirecte doivent former un ensemble
exact et borné de constantes ; les cibles retenues deviennent des comparaisons
résiduelles explicites et des arêtes du CFG.

Les opérations dynamiques et les lectures/écritures ordinaires restent dans
LowIR. Seules les lectures immuables certifiées deviennent des constantes. Les
constantes scalaires, pointeurs affines relatifs au cadre d’entrée et octets du
cadre prouvés constants peuvent traverser les nœuds ; les autres expressions
sont abandonnées plutôt que développées sans limite. La mémoire du cadre utilise
l’invalidation conservative des alias de l’état symbolique existant. Une
écriture via un pointeur inconnu susceptible d’aliaser invalide les faits en
conflit. Cela ne suppose pas qu’un emplacement de pile est privé et ne supprime
pas ses effets au nom d’une absence d’alias non prouvée.

Des étiquettes d’instructions synthétiques uniques distinguent les contextes
clonés. Les limites des instructions originales restent dans une table de
provenance distincte ; les certificats originaux de relocation, d’exception ou
de table de sauts ne sont pas copiés sur les nouvelles occurrences. LowIR
récupéré passe par la conversion ordinaire LowIR-vers-MedIR avant la séparation
HighC/LLVM, partageant le traitement des registres, de la pile, du CFG, de SSA
et de l’ABI. Le parcours HighC exige également une vérification MedIR réussie.

Des budgets limitent les nœuds, contextes par adresse, opérations, évaluations
de nœuds et cibles finies. Un budget épuisé ou une sémantique non prise en charge
ne publie aucune fonction résiduelle. Un graphe de contrôle complet ne signifie
pas que l’émission du source a réussi ; l’API publique vérifie et distingue les
deux résultats.

## Preuves et tests

Le rapport JSON local facultatif contient le hachage de l’entrée, les contrôles
choisis, les budgets, l’état, les compteurs de travail, le nombre de blocs
résiduels, les emplacements des instructions originales et les octets immuables
utilisés. Il contient des informations dérivées de l’entrée et n’est écrit
qu’au chemin local demandé.

Les tests publics utilisent des machines originales à distribution par
registres et par pile, chacune avec des programmes arithmétiques, des branches
avec jonctions et des boucles à l’exécution. Un oracle indépendant non signé
vérifie retours, écritures mémoire, retenues/emprunts et sentinelles de sortie.
Les sources HighC et LLVMC récupérées sont compilées en O0/O2 avec détection
piégeante des comportements indéfinis, puis exécutées face à cet oracle. Les cas
négatifs couvrent la distribution non résolue ou modifiable, un ordre des octets
incompatible, les métadonnées d’exceptions et les budgets.

Voir [testing.md](testing.md) pour les cibles de test ciblées.
