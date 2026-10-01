**Langues**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](interpreter-recovery.md) | [Deutsch](../de/interpreter-recovery.md) | [Español](../es/interpreter-recovery.md) | [Italiano](../it/interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Récupération de sources à partir d’un interpréteur

[← Index de la documentation](README.md)

L’étape expérimentale de spécialisation d’interpréteur élimine la distribution
résolue statiquement dans une fonction x64 déjà liée, tout en conservant ses
entrées d’exécution, effets mémoire, branches et boucles. Elle s’appuie sur la
sémantique des instructions, sans signatures de handlers ni table d’opcodes
propre à un logiciel de protection.

```sh
neverd decompile program --func vm_entry --devirtualize \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize \
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

<!-- i18n-section: control-discovery -->

## Découverte automatique de l’état de contrôle

La CLI et toutes les versions des API C de récupération activent la découverte automatique par défaut. L’API C++ indépendante du fournisseur conserve `SpecializationOptions::DiscoverControlState = false` ; l’appelant peut choisir `true`. Les indications manuelles `--vm-control` et `--vm-control-stack` restent des clés de contexte facultatives. Les champs automatiques ordinaires ne conservent que des relations conjointes finies et bornées, sans créer de clés de contexte ni fixer les entrées à des valeurs échantillonnées. Les compteurs restent dynamiques, sauf si le raffinement mémoire sélectif ci-dessous nécessite leurs constantes prouvées.

La découverte suit les dépendances de contrôle et d’adresse non résolues jusqu’aux entrées structurées des registres à l’entrée du nœud et à l’origine de création des entrées mémoire, y compris les plages d’octets étroites. Un emplacement du cadre n’est proposé que si cette origine identifie une mémoire inchangée depuis l’entrée du nœud, dans une plage exacte relative au cadre d’entrée de la fonction. Des lectures ultérieures de valeurs égales ou transmises depuis une écriture ne créent pas de nouvelles dépendances d’entrée ; les octets inconnus créés après une invalidation mémoire ne sont pas traités comme des emplacements d’entrée. Les historiques de lecture restent disponibles pour les autres analyses. Une dépendance manquante relance l’analyse depuis l’entrée de la fonction. Chaque relation conservée exige encore une preuve exhaustive du domaine fini des bits qu’elle contraint ; les écritures pouvant créer un alias invalident toujours les faits mémoire. La mémoire externe arbitraire et les relations de valeurs non bornées ne deviennent pas finies.

Les demandes aux producteurs conservent leurs masques de bits entre les nœuds sans les élargir à des octets entiers ; les opérations arithmétiques incluent toujours, par prudence, le préfixe de bits de poids faible pouvant transmettre une retenue vers un bit demandé.

Si des dépendances mémoire déjà suivies empêchent à plusieurs reprises une preuve d’adresse exacte, le raffinement peut promouvoir leurs constantes entrantes prouvées en clés de contexte. Il ne partitionne pas les tuples à plusieurs valeurs en nouvelles arêtes et n’ajoute pas de lectures de mémoire invitée pour choisir un contexte : une preuve de valeurs finies ne garantit pas la sûreté d’une lecture supplémentaire. Un état mémoire dynamique ou non borné peut donc encore arrêter la récupération dans les limites configurées.

Ce raffinement sélectif peut aussi distinguer les contextes par des valeurs complètes de registres de 64 bits ou des pointeurs sauvegardés sur la pile, prouvés égaux à la base du cadre d’entrée plus un déplacement exact. La clé conserve ce déplacement, sans deviner l’adresse numérique de l’appel. Les écritures partielles ou susceptibles d’alias invalident ce fait. Les conditions de branchement peuvent proposer des dépendances pour un raffinement borné avant la preuve finale du contrôle, notamment lorsqu’une jonction imprécise expose un successeur non pris en charge. Une proposition ne prouve pas qu’une condition est fausse. L’accessibilité et les cibles conservées exigent toujours des preuves complètes ; une opération non prise en charge restant accessible ou un budget nécessaire épuisé empêche la publication. Face à un successeur non pris en charge, la recherche sélectionne les conditions prédécesseures indécises les plus proches ; ce n’est pas une analyse rétrograde complète des dépendances.

Les demandes remontant vers les producteurs sont identifiées par l’entrée du nœud natif, le mode d’instruction, le type de champ et sa plage d’octets. Seule une arête dont le successeur demande ce champ développe ses dépendances, même si son domaine fini reste trop imprécis. Cela peut relancer l’analyse depuis l’entrée dans les budgets fixés, sans attribuer un rôle global unique à un registre physique réutilisé. La découverte des dépendances est bornée et incomplète ; la récupération sans indications manuelles n’est pas garantie pour tous les interpréteurs.

La projection des champs automatiques ordinaires est elle aussi limitée aux demandes du nœud de destination. Un champ non constant ne participe à la relation conjointe de valeurs finies d’une arête que si sa destination le demande. Les champs manuels et les champs automatiquement promus en clés de contexte restent projetés globalement. Les octets constants connus, les pointeurs exacts relatifs au cadre d’entrée et les faits de provenance sont conservés indépendamment des demandes. Cela évite que des champs sans rapport, utilisés à des étapes différentes des gestionnaires, multiplient les combinaisons de valeurs de la relation. Les budgets configurés et les preuves exigées pour les destinations de contrôle, les adresses mémoire et l’état au retour restent inchangés.

Pour les champs automatiques ordinaires, chaque colonne de tuple ne contraint que les bits demandés et porte le masque correspondant. Les jonctions ne conservent que les bits contraints sur tous les chemins entrants. Les autres bits du même octet restent des valeurs d’exécution : la plage de stockage d’un champ ne certifie pas un domaine fini complet sur toute cette plage. Un octet ne devient constant que lorsque ses huit bits sont prouvés constants.

Les champs manuels et ceux promus en clés de contexte tentent toujours d’abord une preuve sur toute leur largeur, globalement. Si cette preuve ne conclut pas, ils peuvent conserver une relation limitée aux bits demandés ; cela ne fournit jamais d’octets non prouvés à une clé de contexte et ne contourne pas un budget global épuisé.

Lors d’une reprise avec un nouveau graphe, les plages de stockage automatiques ordinaires entièrement contenues dans d’autres champs peuvent partager ces champs plus larges. Les emplacements de phase et les demandes de bits d’origine restent inchangés. Les champs manuels, les champs de contexte et ceux proposés directement par une adresse mémoire non résolue conservent leurs plages exactes. `MaxControlFields` limite les champs réellement conservés après cette normalisation. `DiscoveredControlFields` reste cumulatif et peut dépasser le nombre actif ; la limite de champs et les budgets globaux ne sont pas augmentés.

Un champ automatique ordinaire dont le domaine fini est entièrement prouvé n’ajoute pas immédiatement tous ses producteurs aux champs de contrôle. La découverte enregistre ces dépendances candidates et ne les active que si la récupération reste bloquée et que le raffinement immédiat ne produit aucun candidat. Les champs promus en clés de contexte continuent à développer immédiatement leurs producteurs. Les visites de découverte, les reprises et le travail de preuve conservent leurs budgets cumulés existants.

Après l’échec d’une tentative et la sélection habituelle des candidats, une liste de travail arrière bornée propage les demandes précises de bits en attente à travers les transferts natifs observés. Elle réutilise l’évaluateur scalaire commun uniquement pour les plages portées par les champs de contrôle actuels, sans ajouter de champs ni de contextes. L’ancien graphe sert à acheminer les candidats ; ses valeurs et la faisabilité de ses chemins ne deviennent pas de nouveaux faits. Le parcours des prédécesseurs, la réexécution et la découverte des dépendances partagent les budgets cumulés. Les demandes peuvent ainsi traverser de longues chaînes sans reprise complète à chaque phase native ; la publication exige toujours un nouveau point fixe entièrement prouvé.

Un domaine fini complet peut aussi prouver que certains octets sont constants, même si le mot entier varie. Par exemple, le domaine `{0, 0x100}` a un octet de poids faible constant. Seuls les octets égaux dans tous les tuples énumérés sont conservés, selon l’ordre des octets de la cible ; les autres restent dynamiques. Une énumération partielle, un résultat inconnu du solveur ou l’épuisement du budget de preuve ne fournissent aucun fait de ce type.

Les preuves de domaines finis peuvent être réutilisées au sein d’une exécution de récupération, reprises de raffinement comprises. Un cache borné compare le DAG ordonné complet des expressions, à un renommage cohérent des variables libres près ; le partage des variables, les largeurs, les bits constants, les paramètres des opérateurs et la limite de projection restent dans la clé. Seuls les domaines complets et les preuves qu’un domaine dépasse sa limite sont conservés. Les résultats inconnus ou partiels ne le sont pas ; un échec de recherche ou une capacité insuffisante emprunte la preuve ordinaire. Tous les budgets globaux restent appliqués, et `solverQueries` compte les appels réels au solveur.

Sous le même prédicat, le domaine complet d’une seule colonne variable et une valeur unique prouvée pour chacune des autres déterminent la relation conjointe exacte, si l’accessibilité est établie. Plusieurs colonnes variables exigent toujours une preuve conjointe. Une colonne masquée couvrant tout son domaine de bits ne peut être omise qu’après preuve de l’indépendance de ses bits d’entrée vis-à-vis du prédicat et de toutes les autres colonnes. Des entrées partagées, des résultats inconnus ou une énumération partielle ne justifient jamais de supposer un produit cartésien.

Les valeurs par défaut sont `MaxControlFields = 16` pour les champs manuels et automatiques réunis, `MaxControlRefinements = 16` et `MaxDiscoveryVisits = 65536`. Les reprises partagent les budgets globaux de nœuds (y compris synthétiques), opérations, évaluations, requêtes et visites de découverte. `contexts` compte les contextes natifs ; comme `evaluatedOperations`, `nodeEvaluations` et `solverQueries`, il cumule les tentatives. Les contextes par adresse, emplacements de retour natifs actifs, champs et tuples restent des limites structurelles par tentative. La limite de requêtes reste 4096. L’épuisement ne publie aucun résultat partiel ; `residualBlocks` décrit seulement le graphe résiduel final.

L’option CLI `--vm-max-refinements=N` exige un entier strictement positif et vaut 16 par défaut. En C, la même limite est accessible avec `neverd_devirtualize_source_v2()` ou `neverd_devirtualize_machine_source_v2()` : initialiser `neverd_devirtualize_options_v2` à zéro, définir `base.struct_size = sizeof(neverd_devirtualize_options_v2)`, puis `max_control_refinements` (zéro conserve la valeur par défaut de 16). Le membre intégré `base` contient les options v1 ; les deux membres reserved doivent rester nuls. Les dispositions et points d’entrée v1 existants ne changent pas et ignorent les extensions finales. Les autres budgets de travail et de preuve restent applicables.

`--vm-max-fields=N` et `--vm-max-queries=N` fixent les limites de champs de contrôle et de requêtes au solveur, toujours égales par défaut à 16 et 4096. Ces options et `--vm-max-refinements` exigent des entiers décimaux positifs sur 32 bits et `--devirtualize` ; zéro, signes, hexadécimal, texte final et dépassement sont rejetés. Les deux ABI et les deux moteurs source utilisent les mêmes limites. Augmenter les budgets autorise davantage d’analyse, sans fournir d’indications de contrôle, d’entrées d’exécution ni de nouvelle garantie de preuve. L’épuisement ne publie aucun C.

En C, utiliser `neverd_devirtualize_source_v3()` ou `neverd_devirtualize_machine_source_v3()`. Initialiser `neverd_devirtualize_options_v3` à zéro et définir `base.base.struct_size = sizeof(neverd_devirtualize_options_v3)`. Renseigner `max_control_fields`, `max_solver_queries` et éventuellement `base.max_control_refinements` ; zéro sélectionne la valeur par défaut inchangée. Les trois champs reserved doivent être nuls. v1/v2 ignorent la fin v3, y compris reserved ; v3 ignore les extensions futures. Le rapport consigne le travail réel et les limites effectives `maxControlFields`, `maxControlRefinements`, `maxSolverQueries`.

Le rapport JSON ajoute `discoverControlState`, `maxControlRefinements`, `maxDiscoveryVisits`, `discoveredControlFields`, `discoveredContextFields`, `controlRefinements` et `discoveryVisits` pour décrire l’activation, les limites et le travail effectué. Découvrir des champs ne prouve pas la réussite de la récupération.

<!-- i18n-section: execution-contract -->

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

Les temporaires LowIR sont propres à une seule instruction native relevée. Chaque octet lu doit avoir été défini auparavant dans cette instruction ; réutiliser un décalage d’une instruction précédente ou annuler algébriquement une valeur indéfinie ne prouve pas la validité du code source. Les constantes d’entrée ne peuvent lier que des registres physiques.

<!-- i18n-section: machine-state -->

## Récupération avec état machine explicite

```sh
neverd decompile program --func entry --devirtualize --vm-machine-state \
  --recovery-report machine-report.json -o machine.c
```

`--devirtualize --vm-machine-state` ou `neverd_devirtualize_machine_source_v1()` sélectionne une ABI distincte : un pointeur vers 17 mots `uint64_t` alignés (RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8 à R15, RFLAGS). Le résultat non signé de 64 bits doit être zéro. Un statut non nul ne restaure pas les écritures mémoire. L’état est capturé avant le dépilement du RET final. Son stockage ne doit pas chevaucher la mémoire invitée ; les adresses gardent leurs mappings et l’hôte doit être 64 bits petit-boutiste.

Les adresses originales du code et des données invités conservent leur valeur numérique exacte, y compris celles observées dans les registres de sortie. La génération de source ne doit pas les remplacer par des adresses d’objets globaux du programme généré. L’appelant fournit les mappings invités originaux nécessaires ; produire du C ne reloge pas l’espace d’adressage invité. Les preuves et rapports de récupération restent liés à l’image d’entrée originale.

Le profil impose CPL3/IOPL0, pile fantôme désactivée, aucune interruption asynchrone et exécution normale sans faute. Les flags d’entrée sont canoniques, TF/RF/VM/AC/VIF/VIP nuls ; POPFQ doit garder TF/AC nuls, vérifiés dans le code généré. PUSHFQ/POPFQ utilisent l’état explicite ; RDSSP conserve sa destination et INCSSP atteint est refusé. Les pointeurs de cadre sauvegardés entièrement sont propagés ; écritures partielles ou alias possibles invalident les faits. Les métadonnées d’exception sont admises uniquement pour le chemin normal, sans équivalence de distribution ou déroulement. `sourceABI` et `executionProfile` consignent le contrat ; l’ABI par défaut reste stricte.

Cette ABI d’état machine prend en charge les CALL near directs et indirects par registre dont l’ensemble fini de cibles est prouvé exhaustivement. Un appel indirect par registre capture le registre cible d’origine avant de modifier RSP et écrit exactement une fois l’adresse réelle de l’instruction suivante. Un RET near interne peut choisir parmi un ensemble fini entièrement prouvé. Le code résiduel conserve une lecture de la pile invitée, capture sa valeur avant d’incrémenter le pointeur et distribue selon cette valeur. Atteindre l’emplacement de retour d’entrée préservé reste une sortie externe, même après abandon de cadres internes. Les cibles inconnues, les ensembles contenant une cible absente ou non exécutable, les retours dépilant des arguments et les changements arbitraires de pile restent refusés.

Les CALL near indirects par mémoire sont aussi pris en charge uniquement sous cette ABI d’état explicite, avec pile fantôme désactivée et exécution normale sans faute. L’opérande mémoire doit utiliser un encodage canonique `r/m64` sans surcharge de segment, avec surcharge de taille d’adresse (`addr32`) et REX facultatifs. La sémantique x64 commune des adresses effectives et de LOAD lit la cible avant de modifier RSP et d’empiler l’adresse réelle de l’instruction suivante. Cela vaut même si l’empilement écrase l’emplacement cible ; son adresse ne remplace jamais l’appelé chargé. Les emplacements de pointeurs ou tables finies en lecture seule exigent des preuves de lecture immuable ; les valeurs de pile invitée dont l’initialisation est prouvée nécessitent toujours une preuve complète de l’ensemble fini de cibles. Les octets initiaux ou un instantané d’exécution d’un emplacement modifiable ne prouvent pas l’immuabilité. Les lectures externes non prouvées, faits de pile invalidés, cibles invalides, préfixes FS/GS ou d’autres segments, appels far et préfixes supplémentaires ou non canoniques restent refusés. L’ABI de source par défaut refuse toujours les appels natifs.

L’ABI source ordinaire reconstruit un cadre privé à l’invocation : toutes les plages LOAD/STORE d’origine externe, y compris les adresses calculées, doivent être disjointes du cadre natif privé et de son stockage source reconstruit. C’est une précondition explicite. La preuve partagée refuse les adresses de cadre échappées, les résultats ou branches qui en dépendent et les lectures de bytes privés non initialisés. L’ABI à état machine conserve les adresses invitées et n’utilise pas cette précondition de cadre privé.

Si des indicateurs indéfinis influencent le contrôle, les adresses ou des sorties définies, une preuve indépendante de non-interférence est nécessaire. Le rapport actuel ne fournit pas cette preuve et ne certifie pas ce comportement dépendant du processeur.

`modelInterpreterMachineStateX64` renvoie un `InterpreterMachineStateModel` avec le générateur commun à l’enveloppe source. Les octets de registre `[0, 136)` représentent les 17 mots bruts de l’état ; `RETURN` porte le statut séparément de RAX invité, et la mémoire invitée reste de la mémoire. Des drapeaux d’entrée invalides ou des écritures dynamiques refusées maintiennent un statut d’échec. Le stockage d’état doit être accessible, aligné et disjoint des accès invités. `MaxOperations` borne les métadonnées d’entrée et les opérations générées. Les enregistrements LowIR déterministes ne prouvent pas les sorties architecturalement indéfinies. L’appelant doit fournir les observations, les contrats d’entrée et de cadre, puis une nouvelle preuve `checkLowIRLoopRefinement` ou une preuve de raffinement sur chemins finis ; ni le modèle ni ses enregistrements ne certifient le C compilé.

<!-- i18n-section: limits -->

## Limites actuelles

Les adresses de bytecode dépendant des entrées et les relations entre états du décodeur sont prises en charge uniquement lorsque les domaines finis et les corrélations nécessaires peuvent être prouvés dans les limites configurées. Cela ne démontre pas la prise en charge de tous les schémas de décodage indirect. Les branches et boucles dynamiques peuvent être récupérées si chaque cible de distribution est prouvée ; la couverture des branches ordinaires ne suffit pas à établir cette propriété. Un contrôle non résolu ou l’épuisement d’un budget de preuve requis entraîne un échec, sans source récupéré ni remplacement partiel. Les appels auxiliaires natifs, frontières d’exception ou de réentrée, code modifiable et autres architectures restent hors du contrat de cet adaptateur.

<!-- i18n-section: implementation -->

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

<!-- i18n-section: evidence -->

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

La matrice étendue exige la récupération de trois autres formes indépendantes sans indications manuelles de contrôle : bytecode de pointeurs à enchaînement direct, pile logicielle CALL/RET bornée avec appels virtuels imbriqués, et boucle avec état rotatif de décodage des opcodes. L’exécution native SysV/Win64 et les sources HighC/LLVMC récupérées en O0/O2 doivent correspondre à des oracles mathématiques indépendants, sentinelles de sortie comprises. Curseurs de retour virtuel inconnus, clés de décodage non contraintes et budgets épuisés ne doivent publier aucun source.

Une matrice d’état machine distincte exige, en O0/O2, l’accord entre l’exécution native et les deux parcours C pour les 16 registres généraux, les flags définis et chaque octet de la pile invitée testée. Les appelés des CALL indirects par registre lisent et écrasent le registre cible ; la distribution par retours choisit des cibles connues. L’oracle vérifie l’écriture réelle de l’adresse suivante, la préservation des flags et le rétablissement de RSP. Les adresses attendues proviennent des symboles. Les ensembles inconnus ou invalides et les cibles mémoire non prouvées doivent échouer sans source. Ce sont des exigences de couverture pour des fixtures originales, pas une garantie pour toute machine virtuelle ou tout produit de protection. Ces fixtures originales ont passé les vérifications décrites localement sur x64 Linux.

Les fixtures publiques d’appels mémoire couvrent les pointeurs immuables relatifs à RIP, les tables finies en lecture seule sélectionnées par l’entrée, les emplacements initialisés de pile invitée à cibles finies et les emplacements écrasés par l’empilement du CALL lui-même. La validation locale sur x64 Linux vérifie chacune des quatre formes avec 1 024 états en exécution native, HighC et LLVMC sous O0/O2. L’oracle indépendant compare les 17 mots d’état et 128 octets de pile invitée, y compris l’adresse de retour observée par l’appelé. Les emplacements externes ou modifiables non prouvés, valeurs de pile détruites par alias, cibles invalides et l’ABI par défaut doivent empêcher la publication.

Une régression d’exécution distincte à adresse fixe fournit de la mémoire invitée modifiable à son adresse originale dans l’image d’entrée. Les deux backends C sous O0/O2 doivent lire et écrire ce mapping, conserver les adresses numériques invitées dans les registres de sortie et préserver les sentinelles voisines. Des objets globaux générés ne peuvent pas remplacer ce mapping selon l’oracle. Ces vérifications locales ne prouvent pas une prise en charge de toutes les VM.

Voir [testing.md](testing.md) pour les cibles de test ciblées.

<!-- i18n-section: loop-proposals -->

## Inférence automatique de candidats de preuve de boucle

L’exigence de conservation du prédicat de préfixe ci-dessous s’applique lorsque `GeneralizeEntryPrefix = false`.

`inferLowIRLoopRefinementPlan` propose des modèles bornés avec l’exécuteur symbolique partagé. Les points de coupure couvrent tous les cycles du CFG ; l’élargissement conserve les bits fixes prouvés et élimine les bornes non signées de préfixe qui ne sont pas préservées. Les compteurs unitaires observés et les phases inférées forment des rangs lexicographiques pour les boucles imbriquées croissantes ou décroissantes. `OriginalPrefix` et `CandidatePrefix` exigent `UseEntryPrefix`. Un point situé après d’autres coupures peut obtenir un préfixe apparié réalisable par une réexécution bornée depuis l’entrée réelle. Ce témoin ne couvre pas le domaine d’entrée : chaque arrivée doit impliquer son prédicat, et la couverture complète de l’entrée et des transitions reste obligatoire. `inferAndCheckBinaryLowIRLoopRefinement` exige une récupération complète et des origines natives uniques, puis relance indépendamment la vérification complète original/candidat. Les propositions et correspondances ne sont pas fiables ; seul `Refinement` peut contenir un certificat. Inférence et preuve gardent des budgets explicites séparés. Cette API C++ ne s’exécute pas automatiquement avec `--devirtualize`. Les préfixes inaccessibles, l’alignement arbitraire du contrôle, les rangs hors recherche, l’équivalence du backend C et les choix physiques des bits indéfinis restent non pris en charge.

`GeneralizeEntryPrefix` vaut `false` par défaut et exige `UseEntryPrefix`. Le mode par défaut préserve le prédicat du chemin capturé à chaque arrivée. Le mode explicite de généralisation traite les expressions du préfixe comme des fonctions totales de modèle non fiables hors de ce chemin. Il exige toujours un témoin apparié réalisable, la couverture complète des entrées et segments, l’égalité de tous les registres et du cadre, les projections, les conditions natives et une décroissance stricte du rang sur le domaine inductif élargi. L’inférence élargit une coupure seulement si un état entrant dépasse le domaine du témoin, puis reconstruit et vérifie toutes les transitions générales. Un premier témoin sans itération ne peut masquer une autre entrée qui boucle. La politique est liée à l’empreinte du certificat inductif.

L’inférence imbriquée peut proposer des bornes non signées strictes ou larges entre des compteurs à pas unitaire observés et des valeurs de préfixe inchangées référencées par les prédicats de contrôle, y compris les sorties par égalité. Elle vérifie d’abord les états concrets puis élimine de façon monotone les relations invalides sur les transitions générales entrantes. Parcours et requêtes restent dans les budgets explicites existants ; le vérificateur original/candidat prouve ensuite les modèles indépendamment.

L’inférence des boucles imbriquées propose aussi une égalité entre un opérande mis en cache et un compteur ou une entrée de contrôle inchangée lorsque leurs expressions de préfixe coïncident. Elle vérifie chaque état entrant concret, conserve les candidats pour les caches découverts lors des élargissements suivants et supprime toute relation violée par un état entrant général. Chaque mot garde son propre paramètre récupérable ; l’égalité est un prédicat vérifié. Les récurrences de copie peuvent accélérer la suppression de bits fixes accidentels, sans supposer de sémantique.

L’inférence imbriquée recherche aussi les comparaisons d’égalité ou d’inégalité mises en cache dans les champs ayant au plus 16 bits variables. Chaque relation utilise les valeurs courantes du compteur et de la borne ; le cache conserve son propre paramètre d’état récupérable. Une garde ou une initialisation simplifiée en constante peut masquer la comparaison jusqu’à un élargissement ultérieur : de nouveaux candidats sont alors possibles, sans réintroduire les candidats rejetés ou supprimés. Ils doivent tenir sur toutes les arrivées concrètes sauvegardées et toutes les transitions entrantes courantes. Les parcours de variables du DAG sont mis en cache par point de coupure et facturés au budget partagé des prédicats. La couverture native complète, l’égalité de tout l’état et la décroissance stricte du rang restent des exigences indépendantes. La découverte des compteurs continue après les transitions générales : un compteur externe masqué par le premier témoin de boucle interne peut encore recevoir des bornes et des copies d’opérandes vérifiées. Les relations rejetées ne sont pas restaurées et les recherches de pas unitaires respectent le budget de nœuds symboliques.

Les rejeux de préfixe d’entrée visitent les branches en attente avant de dérouler encore une boucle précédente. Un témoin atteignable court peut ainsi être trouvé même si une autre branche admet un nombre arbitraire d’itérations. L’inférence et le vérificateur original/candidat partagent cet ordre. Tout le travail reste imputé aux budgets existants ; un témoin de préfixe ne remplace ni la couverture complète des entrées, ni la préservation des invariants, ni les vérifications de terminaison.

`pairLowIRLoopRefinementPlans` combine deux propositions indépendantes de relation avec soi-même au moyen de `LowIRLoopCutpointPair` explicites. Chaque point de coupure doit être apparié exactement une fois. `SharedInputs` ajoute des prédicats d’égalité entre des entrées inductives de même largeur, en conservant leurs deux liaisons et projections pour le vérificateur. Les autres temporaires restent indépendants, les préfixes du candidat utilisent `CandidatePrefix`, les deux prédicats sont conservés et le rang original est choisi. Les plans doivent partager `UseEntryPrefix` ; `GeneralizeEntryPrefix` est activé si l’un le demande. Les définitions temporaires ne doivent pas se chevaucher et les utilisations doivent avoir une largeur exacte. `MaxMetadata`, fixé à 65536 par défaut, borne la construction avant copie. Le résultat reste une proposition non fiable : `checkLowIRLoopRefinement` doit vérifier indépendamment la couverture complète, les états et cadres mémoire, les observations et la progression stricte. Aucun certificat n’est délivré et les valeurs par défaut de la CLI restent inchangées.
