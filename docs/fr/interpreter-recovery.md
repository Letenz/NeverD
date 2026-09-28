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

La récupération PE exige toutes les métadonnées de l’image, y compris les relocations globales et les enregistrements d’exceptions. La CLI les charge avant d’appliquer `--func` ; les appelants de l’API C ne doivent pas restreindre d’abord la session avec `neverd_session_restrict_function()`. L’adaptateur refuse les images chargées avec un ensemble limité de fonctions, car des métadonnées omises ne prouvent pas l’absence de corrections ni d’arêtes d’exception.

Seules des plages complètes en lecture seule, adossées au fichier et dépourvues
de mappages superposés ou de corrections du chargeur, peuvent fournir des
lectures constantes de l’image. Les tables modifiables, relocations non résolues
et instantanés d’exécution ne prouvent pas l’immuabilité des lectures. Les relocations COPY et les répertoires d’exceptions structurellement
incomplets sont refusés. Si le répertoire PE et les plages de fonctions sont
complets, un gestionnaire inconnu dans une autre fonction ne bloque pas
l’analyse de l’entrée choisie ; atteindre son code arrête la récupération. Le domaine de validité exige des retours ABI ordinaires : la plage
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

Les formes x64 exactes de `PUSHFQ`/`POPFQ` restent dans le programme résiduel. L’analyse traite chaque instantané des drapeaux machine comme une valeur d’exécution inconnue ; le lifter y combine séparément les drapeaux arithmétiques modélisés. La restauration des drapeaux reste un effet d’exécution. Une adresse dérivée de drapeaux inconnus ne peut pas bénéficier du contrat de non-recouvrement du pointeur externe avec l’adresse de retour ; un dispatch dérivé dont les cibles ne sont pas bornées provoque toujours un échec.

Avant une capture complète des drapeaux, chaque drapeau arithmétique ou de direction modélisé doit être défini dans la fonction reconstruite. Toute lecture directe d’un drapeau exige aussi une définition sur chaque chemin prédécesseur atteignable, même si la simplification symbolique annule sa valeur. Sinon, la récupération refuse de produire du C contenant un piège « registre inconnu ».

## Limites actuelles

Les adresses de bytecode dépendant des entrées et les relations entre états du décodeur sont prises en charge uniquement lorsque les domaines finis et les corrélations nécessaires peuvent être prouvés dans les limites configurées. Cela ne démontre pas la prise en charge de tous les schémas de décodage indirect. Les branches et boucles dynamiques peuvent être récupérées si chaque cible de distribution est prouvée ; la couverture des branches ordinaires ne suffit pas à établir cette propriété. Un contrôle non résolu ou l’épuisement d’un budget de preuve requis entraîne un échec, sans source récupéré ni remplacement partiel. Les appels auxiliaires natifs, frontières d’exception ou de réentrée, code modifiable et autres architectures restent hors du contrat de cet adaptateur.

## Implémentation partagée

`SpecializationProvider` fournit des instructions intégralement liftées et les
preuves justifiant les lectures immuables. `NeverDInterpreterSpecialization`
utilise la sémantique existante de `SymExec` pour évaluer partiellement les
opérations entières et de contrôle. L’adaptateur binaire gère le mappage et le
décodage ; il n’implémente pas un second évaluateur d’instructions.

Pour une adresse de lecture symbolique finie, le solveur de vecteurs de bits intégré énumère les adresses candidates sous les contraintes courantes. L’ensemble n’est accepté qu’après un résultat UNSAT final prouvant qu’aucune autre adresse n’est possible, et chaque adresse doit disposer d’un certificat complet de lecture immuable sans faute mémoire. Une telle lecture certifiée peut être remplacée dans LowIR par une capture de l’adresse et une chaîne exacte de SELECT ; les lectures ordinaires non certifiées restent dynamiques. Un échantillon d’adresses ne remplace jamais l’ensemble complet. Les registres de contrôle et emplacements du cadre d’entrée sélectionnés peuvent conserver des tuples conjoints bornés entre les nœuds, notamment la relation entre un curseur et sa clé de décodage. Jonctions et élargissements restent conservatifs. Des modèles SAT partiels ou un résultat inconnu ne prouvent pas l’exhaustivité des adresses ou des cibles. Ce mécanisme ne nécessite pas le backend Z3 facultatif.

Un nœud est identifié par son curseur natif, le mode d’instruction et les constantes sélectionnées des registres de contrôle et des emplacements du cadre d’entrée. Les autres faits à l’échelle de l’octet se combinent par intersection. Lorsqu’un fait entrant s’affaiblit, le nœud est réévalué. Ainsi, les boucles du programme restent des boucles au lieu de développer chaque itération observée. Toutes les valeurs accessibles d’une destination indirecte doivent appartenir à un ensemble borné dont l’exhaustivité est prouvée ; les cibles retenues deviennent des comparaisons résiduelles explicites et des arêtes du CFG.

Les opérations dynamiques et les lectures/écritures ordinaires restent dans
LowIR. Les
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

Les ensembles finis d’adresses de lecture, les tuples conjoints de contrôle et le nombre de champs de contrôle ont aussi des limites explicites. Un plafond global de requêtes au solveur et des plafonds par requête sur les portes, conflits, propagations et visites de littéraux surveillés bornent le travail de preuve ; le nombre de nœuds symboliques borne la croissance des expressions. Le rapport JSON contient ces budgets ainsi que `solverQueries` et `relationalWidenings`.

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

Des fixtures originales supplémentaires utilisent des enregistrements en lecture seule choisis par l’entrée, des champs de contrôle curseur/clé liés et un même handler à plusieurs positions virtuelles. Elles couvrent les branches avec jonctions et les boucles dont le choix d’enregistrement dépend de l’état courant du programme. Leur oracle natif indépendant utilise les conventions SysV et Win64 ; les deux parcours C récupérés sont vérifiés en O0/O2 avec pièges de comportement indéfini et sentinelles de sortie. Des certificats incomplets ou des budgets de preuve insuffisants ne doivent publier aucun résultat partiel.

Voir [testing.md](testing.md) pour les cibles de test ciblées.
