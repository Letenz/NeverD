**Langues** : [English](../unpack.md) | [简体中文](../zh-CN/unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](unpack.md) | [Deutsch](../de/unpack.md) | [Español](../es/unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← Index de la documentation](README.md)

# Dépaquetage des exécutables compressés

`neverd unpack` récupère le programme qu'un exécutable compressé reconstruit dans son propre espace d'adressage. La commande exécute l'entrée comme un processus invité borné, observe l'endroit où le stub passe la main au code qu'il a produit, puis écrit cette image dans un nouveau fichier du même conteneur. Elle ne dévirtualise pas : les fonctions qu'un protecteur a virtualisées le restent. Compilez avec `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Entrées prises en charge

Le conteneur détermine comment un fichier est validé et reconstruit, le jeu d'instructions détermine comment un transfert est jugé, et les deux déterminent le profil de processus invité. Une entrée hors de ce tableau est rejetée nommément avant toute exécution.

| Conteneur (`format`) | Jeu d'instructions | Profil invité | Connaissance du stub |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | UPX |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | aucune ; observation seule |

## Utilisation

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

La commande affiche un rapport JSON. Le code de sortie 0 signifie que l'image a été écrite, 3 que l'exécution bornée s'est terminée avant qu'une entrée ne soit acceptée (`outcome` vaut `no_entry` et rien n'est écrit), et 1 qu'une entrée, une option ou la préparation est invalide. Le rapport indique le `format`, l'`architecture` et le `profile` réellement exécutés. Le point d'entrée C est `neverd_unpack_json` ; Python expose `Session.unpack`. Les options sont les [options de processus](process-emulation.md) plus `transfer`. Les valeurs par défaut diffèrent là où un stub a besoin de plus de ressources : 100000000 instructions, 600 secondes et 512 MiB, et `windows.defer_unmodeled` est activé.

## Comment l'entrée est établie

La génération zéro est l'image telle que le chargeur invité l'a mappée. Une instruction dont les octets diffèrent de cette image a été générée par le processus. Un transfert est la première exécution d'un code plus récent que celui qui s'exécutait ; `transfers` énumère chacun avec sa RVA, sa `generation` et indique si le pointeur de pile est égal à sa valeur à l'entrée du processus (`stack_balanced`).

1. Un transfert sur la pile d'entrée est l'entrée du programme : le stub a rendu la pile qu'il avait reçue. `entry_source` vaut `transfer`.
2. Un transfert sur une pile plus profonde est un appel que le stub fait vers le programme, par exemple un rappel TLS. Il est signalé, mais pas accepté. Lorsque le stub identifié nomme la cible de son saut final, l'image est reconstruite à cet appel, avant qu'aucun code du programme ne se soit exécuté, et l'adresse nommée est l'entrée. `entry_source` vaut `stub`.
3. `transfer` sélectionne explicitement un transfert de la liste par sa position, pour les protecteurs qui dépaquettent par étapes ou qui appellent leur programme.

Aucune entrée n'est déduite de la forme du code de démarrage d'un compilateur. Une exécution qui s'arrête avant signale `no_entry` avec le `stop_reason` du processus.

## L'image reconstruite

Les sections conservent leurs RVA et contiennent la mémoire observée ; chaque section a l'accès que ses pages avaient au moment du transfert. Une dernière section `.neverd` contient un nouveau répertoire d'importation posé sur les cellules par lesquelles le programme appelle déjà, de sorte qu'aucun code ni aucune donnée ne bouge. `imports` énumère chaque cellule avec son `origin` : les cellules `static` ont été liées par le chargeur à partir du répertoire de l'entrée, les cellules `runtime` ont été écrites par le stub. L'image est fixée à sa base observée : aucune relocalisation du contenu généré n'a été observée, le répertoire de relocalisation est donc supprimé et `IMAGE_FILE_RELOCS_STRIPPED` est positionné. Pour UPX, le répertoire TLS propre au programme est désigné à nouveau, car celui du fichier compressé n'atteint que le gestionnaire du stub.

La table des sections d'origine, la disposition du répertoire d'importation et la table de relocalisation ne sont pas reconstruites ; un compresseur ne les restaure pas en mémoire.

## Identification

`packer.kind` ne nomme un protecteur qu'à partir de preuves présentes dans le fichier. UPX en exige deux parmi `upx_section_names`, `upx_pack_header` (nombre magique, format, méthode et somme de contrôle) et `upx_entry_stub`. Une entrée non identifiée est tout de même dépaquetée par observation.

## Limites

Seuls les exécutables sont pris en charge ; les DLL ne sont pas exécutées. L'exécution vérifiée admet une instruction à la fois, de l'ordre de 10^5 par seconde, si bien qu'un stub qui exige des milliards d'instructions dépasse tout budget réaliste. L'exécution est suivie par page de 4 KiB : du code écrit dans une page qui exécute déjà du code de la même génération n'est pas signalé comme un transfert. Le code du programme qui s'exécute avant l'entrée, par exemple un rappel TLS qui appelle une API non modélisée, arrête l'exécution sauf si le stub déclare son entrée. Les chargeurs VMProtect ne sont pas encore pris en charge.

## Vérification

`NeverDUnpackTests` vérifie l'identification. `NeverDUnpackExecutionTests` dépaquette des échantillons UPX versionnés (NRV2B, NRV2D, NRV2E, LZMA et un programme utilisant la bibliothèque d'exécution C) sur Unicorn, KVM et WHP, compare chaque section à l'original, exécute l'image récupérée et exige des octets identiques de tous les backends. `UnpackGeneratedTests.cpp` compresse un programme au sein du test pour x86-64 et ARM64 et vérifie, par rapport au fichier lié, un chargeur qui part directement, un chargeur qui appelle d'abord le programme et un chargeur à deux étages. `NeverDUnpackPublicTests` couvre l'ABI C et la CLI. `unittests/unpack/fixtures/Makefile` régénère les échantillons UPX.
