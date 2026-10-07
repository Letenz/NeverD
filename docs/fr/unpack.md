**Langues** : [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](unpack.md) | [Deutsch](../de/unpack.md) | [Español](../es/unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Index de la documentation](README.md)

# Dépaquetage des exécutables compressés

`neverd unpack` récupère le programme qu'un exécutable compressé reconstruit dans son propre espace d'adressage. La commande exécute l'entrée comme un processus invité borné, observe l'endroit où le stub passe la main au code qu'il a produit, puis écrit cette image dans un nouveau fichier du même conteneur. Elle ne dévirtualise pas : les fonctions qu'un protecteur a virtualisées le restent. Compilez avec `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Entrées prises en charge

Le conteneur détermine comment un fichier est validé et reconstruit, le jeu d'instructions détermine comment un transfert est jugé, et les deux déterminent le profil de processus invité. Une entrée hors de ce tableau est rejetée nommément avant toute exécution.

| Conteneur (`format`) | Jeu d'instructions | Profil invité | Preuve du point d’entrée |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | observation à l’exécution |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | observation à l’exécution |

## Utilisation

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

La commande affiche un rapport JSON. Le code de sortie 0 signifie que l'image a été écrite, 3 que l'exécution bornée s'est terminée avant qu'une entrée ne soit acceptée (`outcome` vaut `no_entry` et rien n'est écrit), et 1 qu'une entrée, une option ou la préparation est invalide. Le rapport indique le `format`, l'`architecture` et le `profile` réellement exécutés. Le point d'entrée C est `neverd_unpack_json` ; Python expose `Session.unpack`. Les options sont les [options de processus](process-emulation.md) plus `transfer`. Les valeurs par défaut diffèrent là où un stub a besoin de plus de ressources : 100000000 instructions, 600 secondes et 512 MiB, et `windows.defer_unmodeled` est activé.

## Comment l'entrée est établie

La génération zéro est l’image mappée par le chargeur invité. Un transfert commence l’exécution d’un code plus récent que le précédent. `transfers` donne le RVA, `generation`, l’égalité de pile (`stack_balanced`) et l’appartenance à l’invocation d’entrée du programme principal (`program_invocation`). Avant cette invocation, la comparaison utilise la pile du premier initialiseur.

1. L’entrée par défaut exige `stack_balanced` et `program_invocation` : le stub a rendu la pile de l’invocation principale. `entry_source` vaut `transfer`.
2. Les callbacks TLS et DLL invoqués par l’OS sont consignés mais ne deviennent jamais l’entrée par défaut, même avec une pile équilibrée. Les appels invités plus profonds continuent aussi. Aucun callback n’est sauté et aucune signature ne prédit l’entrée.
3. `transfer` sélectionne explicitement une position, y compris un callback d’initialisation ou un transfert intermédiaire.

Aucune entrée n'est déduite de la forme du code de démarrage d'un compilateur. Une exécution qui s'arrête avant signale `no_entry` avec le `stop_reason` du processus.

## L'image reconstruite

Les sections conservent leurs RVA, la mémoire observée (y compris les effets des initialisations déjà exécutées) et leurs droits de page. La nouvelle section `.neverd` contient le répertoire d’importation et les nouvelles cellules IAT nécessaires aux appels exportés et chargements d’adresses, sans réutiliser les zones nulles d’origine. `origin` dans `imports` distingue les cellules `static` liées depuis l’entrée et les cellules `runtime` écrites par l’invité ou ajoutées pour une réparation. Les relocalisations du code généré n’ayant pas été observées, l’image reste à sa base observée, le répertoire de relocalisation est supprimé et `IMAGE_FILE_RELOCS_STRIPPED` est positionné.

Les candidats IAT d’exécution existants doivent former un tableau contigu de pointeurs d’exports d’un même fournisseur, avec une terminaison nulle intacte dans la section d’origine. La terminaison du fournisseur voisin ne suffit pas.

La récupération TLS utilise l’identité d’allocation du chargeur, une liste complète de rappels terminée par zéro et les appels observés au code généré. Plusieurs candidats provoquent une erreur explicite. Aucun nom ni octet propre à un protecteur n’intervient. La preuve d’exécution d’un callback généré prime sur l’achèvement d’un initialiseur du chargeur lors du choix du répertoire.

`materialized_tls_callbacks` compte les callbacks TLS de l’image principale dont les appels d’attachement au processus, effectués par l’OS, sont revenus avant la capture de l’invocation principale. Leurs effets mémoire figurent déjà dans l’instantané. Les adaptateurs de la nouvelle section `.neverd` évitent uniquement cet appel répété et transmettent les autres notifications au callback original par appel terminal. Répertoire, table et adaptateurs utilisent un nouvel espace sans modifier les octets d’origine. La section devient exécutable avec les adaptateurs, et inscriptible seulement si de nouvelles cellules IAT l’exigent. Les callbacks internes sans preuve d’achèvement gardent leur comportement. Lors de l’attachement au processus, le premier adaptateur restaure aussi le TLS capturé du thread principal s’il diffère du modèle. Une preuve TLS absente ou de taille incohérente provoque un échec explicite. Le modèle original reste disponible pour les futurs threads ; les autres notifications ne restaurent jamais le bloc capturé.

Le chargement différé autorise des cibles exécutables de callback ou d’entrée dans une mémoire initialement nulle dont un initialiseur antérieur produit le code. Les tableaux de callbacks et les métadonnées d’allocation TLS exigent toujours des données de fichier validées ; le chargement strict conserve ses vérifications. Le modèle OS fournit la provenance des invocations et avertit l’observateur lorsqu’il prépare un appel ou restaure un appelant suspendu. Les surveillances sont réarmées à ces frontières, même si callback et entrée générée partagent une page.

`import_repair` couvre deux exécutions bornées supplémentaires après la capture de l’entrée : découverte des appels exportés, puis observation de l’état des routines. Chaque exécution a ses propres limites. Les compteurs d’instructions et d’événements sont leur somme saturée ; `stop_reason` et le diagnostic décrivent la dernière, et les appels observés comptent seulement la découverte. Sans adresse de continuation, l’observation est omise. Les identités d’export contradictoires interdisent la réécriture. Seuls les chemins atteints sont couverts ; `unpacked` ne certifie ni tous les imports ni le succès du programme.

L’ordre de liaison des imports peut changer les adresses d’export. La réparation conserve l’identité de l’exécution de preuve et observe aussi les exports résolus après l’entrée ; une adresse provenant d’une autre exécution n’autorise aucune réécriture.

La découverte observe la distribution des exports, y compris celui non modélisé qui arrête l’exécution, au lieu de dépendre des seuls journaux d’appels modélisés. Un retour ABI illisible ne peut établir une preuve de routine. Réparer un appel pur à un export opaque conserve l’arrêt explicite pour service non pris en charge.

L’observation admet au plus 256 débuts candidats dans les 256 octets précédant les continuations d’appels exportés réels. `CALL rel32`, éventuellement précédé d’un PUSH/POP GPR, identifie seulement une frontière ; les octets suivants ne sont pas une signature. Une fenêtre de six à huit octets devient `call [rip+IAT]` seulement si l’entrée API a exactement une adresse de retour empilée, tous les autres registres (drapeaux et SIMD compris), mappings et RAM persistante inchangés, sans appel OS intermédiaire. Des NOP initiaux préservent l’adresse de retour exacte. Une fenêtre de sept ou huit octets devient `mov r64, [rip+IAT]` si elle renvoie un export connu dans un seul GPR, avec pile équilibrée et les mêmes garanties. Le registre résultat est déduit de l’état : tout GPR sauf RSP, dont R8-R15 ; huit octets laissent un NOP final. Seul l’espace temporaire sous le SP appelant est exclu ; sa pile est comparée. Une invocation ultérieure impure, non résolue ou inachevée invalide la preuve. Le début exécuté est requis ; aucun préfixe REX n’est déduit de l’octet précédent, et les débuts chevauchants partageant un retour sont refusés. `observed_loads` et `repaired_loads` comptent les chargements séparément.

La table des sections d'origine, la disposition du répertoire d'importation et la table de relocalisation ne sont pas reconstruites ; un compresseur ne les restaure pas en mémoire.

## Identification

Les champs de compatibilité `packer.kind` et `packer.evidence` renvoient `unidentified` et un tableau vide. Le registre de protecteurs, l’analyse des en-têtes de compression et les signatures de stub sont supprimés. L’ancienne API valide seulement le conteneur.

## Limites

Les EXE sont exécutés, pas les DLL. L’exécution vérifiée procède instruction par instruction. x64 Unicorn/KVM/WHP fournit aussi `direct-user-x64-v1`, borné par le temps et les événements sans compter les instructions. Le suivi porte sur des pages de 4 KiB : une réécriture dans une page déjà exécutée de la même génération ne produit pas de transfert. L’initialisation avant l’entrée peut créer un état externe non portable, et rejouer les rappels TLS peut avoir des effets supplémentaires. Les API non modélisées arrêtent explicitement l’exécution. La réparation couvre les fenêtres d’appel x64 validées de six à huit octets et les chargements d’adresses de sept ou huit octets ; les autres formes et chemins non parcourus restent non résolus. Le code virtualisé le reste.

## Vérification

`NeverDUnpackTests` vérifie les conteneurs, l’allocation sûre des IAT, les conflits et les refus TLS. `NeverDUnpackExecutionTests` utilise le même chemin générique sur Unicorn/KVM/WHP pour UPX NRV2B/NRV2D/NRV2E/LZMA et CRT, compare les sections au programme lié indépendant après ses propres initialisations et exige des sorties identiques des backends disponibles. `UnpackGeneratedTests.cpp` génère des programmes x86-64/ARM64 indépendants et vérifie les transferts, le chargement par étapes, les imports et l’exécution directe x64 Unicorn/KVM/WHP. Les cas natifs obligatoires ne peuvent pas être ignorés en CI. `NeverDUnpackPublicTests` couvre l’ABI C et la CLI. `unittests/unpack/fixtures/Makefile` régénère les échantillons UPX.
