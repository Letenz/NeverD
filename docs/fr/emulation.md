**Langues** : [English](../emulation.md) | [简体中文](../zh-CN/emulation.md) | [繁體中文](../zh-TW/emulation.md) | [日本語](../ja/emulation.md) | [한국어](../ko/emulation.md) | [Français](emulation.md) | [Deutsch](../de/emulation.md) | [Español](../es/emulation.md) | [Italiano](../it/emulation.md) | [Русский](../ru/emulation.md) | [العربية](../ar/emulation.md)

<!-- i18n-source: a3e64122b77a690dd856d02f5b2af53973d3bf1affd973bea5f9735aa9dd6722 -->

[← Index de la documentation](README.md)

# Exécution CPU et environnements invités

<!-- i18n-section: backends -->

## Moteurs CPU et charges invitées

L’exécution CPU sépare admission ISA, mémoire invitée, transport du moteur et politique OS. `NEVERD_ENABLE_CPU_EMULATION` active la couche CPU x64/ARM64 ; `NEVERD_ENABLE_DRIVER_EMULATION` ajoute l’environnement Windows WDM/KMDF x64 borné. `linux-elf64-v1` exécute les processus Linux ELF pris en charge. Voir [Exécution CPU](cpu-execution.md), [Émulation de processus invités](process-emulation.md) et [Émulation des pilotes Windows](driver-emulation.md).

Pour les contrats natifs pris en charge, `auto` choisit KVM sous Linux, WHP sous Windows ou [HVF sous macOS](macos-hvf.md), avec la même ISA pour le guest et l’hôte. Les ISA différentes utilisent Unicorn ; `software-cpu-v1` et l’API V1 originale conservent l’exécution logicielle. Un backend explicitement choisi mais indisponible échoue sans repli. L’exécution native vérifie les instructions admises, adresses et effets avant l’entrée. La virtualisation de l’hôte ne choisit pas l’OS invité : les [profils Darwin](darwin-emulation.md) modélisent séparément macOS, iOS et iOS Simulator. HVF nécessite le droit `com.apple.security.hypervisor`.

`driver-strict` / `checked-x64-v1` couvre une exécution x64 bornée ; le chargement des pilotes Windows reste x64. `checked-aarch64-v1` et `checked-user-aarch64-v1` incluent ARM64 FP32/FP64 borné, SIMD fixe et l’état complet FPCR/FPSR/vectoriel. Le guide Mac consigne l’acceptation native ARM64 HVF ; la validation des charges ARM64 KVM/WHP et l’acceptation complète Intel HVF restent à terminer. Le support CPU n’implique pas la compatibilité avec tout pilote ou application.

<!-- i18n-section: windows-processes -->

## Processus et modules Windows

`windows-pe64-v1` prend en charge des processus console Windows x64/ARM64 bornés avec PEB/TEB, TLS statique et dynamique, `DllMain`, API Win32 nommées et graphes DLL explicites sans cycle. Les modules invités acceptent les imports de code/données par nom ou ordinal, DIR64, les exports redirigés et les véritables listes du chargeur. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` et `GetProcAddress` utilisent le catalogue configuré. CRT/GUI, SEH utilisateur ARM64 fondé sur les cadres de pile, threads et compatibilité Windows générale restent inachevés ; les preuves natives ARM64 KVM/WHP manquent encore.

`WindowsSystemModules` construit des images modèles PE64 bornées pour `ntdll.dll`, `kernelbase.dll` et `kernel32.dll` sur les deux ISA. Les recherches ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` et `GetProcAddress` partagent leurs bases mappées ; PEB/LDR et `MEM_IMAGE` décrivent les mêmes images. Imports statiques, recherches nommées et redirections invitées partagent les portes API et le résolveur. Les fournisseurs restent résidents, sans rappel invité d’initialisation, et ne bloquent pas le retour du point d’entrée après déchargement des DLL invitées ordinaires. Toute modification des en-têtes ou métadonnées d’export arrête la recherche. Noms système non modélisés et ordinaux non nuls arrêtent explicitement l’exécution ; une différence de casse d’un nom modélisé ou un nom vide renvoie 127, une requête NULL renvoie 87. Octets et adresses générés relèvent du modèle ; dispositions propres aux versions Windows, ordinaux natifs et alias entre fournisseurs ne sont pas reconstruits. `WindowsSystemTests.cpp` compare des EXE originaux x64/ARM64 à Windows natif, avec huit observations indépendantes du retour du thread initial.

<!-- i18n-section: environment-memory -->

## Environnement et mémoire

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` partagent le bloc invité courant des paramètres de processus du PEB. Les noms ASCII ignorent la casse ; les valeurs sont en UTF-16. Les modifications valident les entrées, la capacité et les droits d’écriture avant publication. Les instantanés restent indépendants des modifications et libèrent leur mémoire invitée. Le modèle limite le bloc à 64 KiB ; chaînes et expansions sont bornées et vérifient l’échéance. La propriété inconnue des pointeurs, les blocs mal formés, les pages de codes ANSI et le chevauchement des tampons d’expansion restent non pris en charge. `WindowsEnvironmentTests.cpp` compare des fixtures originales x64/ARM64 sur les backends disponibles ; la CI exige un oracle Windows natif indépendant.

`WindowsProcessHeap` centralise allocation, `HeapReAlloc`, libération et taille du tas du processus. Le redimensionnement conserve les octets retenus ; `HEAP_ZERO_MEMORY` initialise les octets ajoutés et `HEAP_REALLOC_IN_PLACE_ONLY` interdit le déplacement. Un redimensionnement échoué conserve le bloc et renvoie NULL avec `ERROR_NOT_ENOUGH_MEMORY` (8), conformément aux observations natives. Les pages indépendantes restituent leur capacité lors des réductions et libérations ; croissance préparée et copies bornées vérifient l’échéance. Tas personnalisés, indicateurs générant des exceptions, propriété inconnue et plages inaccessibles arrêtent explicitement l’exécution. `WindowsHeapTests.cpp` couvre les deux ISA, déplacement imposé, réutilisation du budget et atomicité des échecs ; CI exécute aussi le même EXE original sur Windows natif.

La mémoire virtuelle Windows ajoute `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` et `FlushInstructionCache` pour le processus courant. La couche OS possède les réservations ; `AddressSpace` reste la référence pour les pages validées, les permissions et leur stockage. Les tests couvrent la réécriture de code, les défauts d’accès et la réutilisation du budget mémoire.

`WriteProcessMemory` suit le comportement des pages engagées observé sur x64/ARM64 pour les écritures de 4 Kio au maximum dans le processus courant. Il conserve la protection de chaque région, les préfixes copiés, les nombres d'octets et LastError, y compris `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` et le succès après un préfixe RX. `WindowsMemoryWriteTests.cpp` contrôle les 25 paires de protections ; `check_windows_memory_write.py` vérifie le même exécutable original dans la CI Windows native. Les destinations non engagées restent explicitement non prises en charge.

<!-- i18n-section: vectored-exceptions -->

## Exceptions vectorisées et continuation

`WindowsProcessExceptions` implémente `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` et `RaiseException` sur le même CPU et budget de processus. Les gestionnaires ordonnés peuvent modifier les inscriptions, lever des exceptions imbriquées, appeler les API modélisées, charger des DLL et terminer le processus. Les violations de données x64/ARM64 et divisions entières x64 reprennent après validation des modifications de `CONTEXT`, en conservant registres généraux, SIMD et état FP pris en charge. Les exceptions logicielles reprennent via une véritable instruction de retour du fournisseur modélisé. Limites : 128 inscriptions conservées et 16 cadres imbriqués. Dispositions invalides, pointeurs modifiés, champs non pris en charge et dépassements échouent explicitement. SEH ARM64 et déroulement fondés sur la pile, débogage et fautes d’exécution/de garde restent non pris en charge. `WindowsExceptionTests.cpp` compare des EXE/DLL originaux à Windows natif ; les preuves ARM64 KVM/WHP natives restent à obtenir. Les enregistrements des exceptions logicielles portent `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), indépendamment du drapeau de non-continuation fourni par l’appelant ; l’exécutable Windows original vérifie les valeurs exactes des drapeaux des exceptions logicielles et matérielles.

`AddVectoredContinueHandler` et `RemoveVectoredContinueHandler` gèrent une liste ordonnée distincte, avec une limite commune de 128 inscriptions conservées avec les gestionnaires d’exception. Après acceptation de la reprise par un gestionnaire d’exception vectorisé, les callbacks de continuation voient le même enregistrement modifiable et le même `CONTEXT`. La validation finale suit ces callbacks, y compris les exceptions imbriquées et les notifications DLL. Un handle ne peut pas être retiré par l’autre famille de gestionnaires. `WindowsContinuationTests.cpp` compare des EXE originaux à Windows natif pour l’ordre, l’arrêt anticipé, les modifications de liste, la réparation du contexte, les appels imbriqués, le chargement et la sortie du processus. Le chemin vectorisé Windows x64 testé autorise la reprise avec `EXCEPTION_NONCONTINUABLE` ; cela ne prouve pas le comportement SEH fondé sur la pile. L’exécution ARM64 native reste non vérifiée.

<!-- i18n-section: caller-context -->

## Contexte de l’appelant

`RtlCaptureContext` est disponible via `kernel32.dll` et `ntdll.dll` pour x64 et ARM64. `WindowsProcessContext` et `IntegerABI` partagés enregistrent PC/SP de l’appelant sans modifier le CPU ni LastError. Les observations Windows natives établissent les indicateurs x64 `0x10000f`, la conservation des zones home/débogage/vecteurs non écrites et les adresses x87 historiques sur 32 bits ; ARM64 copie LR vers PC et met X0/LR à zéro dans le résultat. Registres, SIMD et contrôles flottants proviennent de l’invité ; sélecteurs x64 et masque de capacités MXCSR suivent le CPU invité configuré. Les destinations invalides, non alignées ou partiellement inaccessibles échouent avant publication. `WindowsContextTests.cpp` couvre imports directs, recherche par fournisseur, rappels VEH, sorties traversant une page et atomicité des échecs. `scripts/check_windows_context.py` exécute le programme original sous Windows x64/ARM64, avec un oracle natif distinct pour un état x87 non vide. Ces observations ARM64 ne prouvent pas l’exécution native KVM/WHP. Restauration du contexte, parcours de pile et tables de fonctions dynamiques restent à implémenter séparément. `WindowsProcessServices.def` déclare les restrictions exactes par module : la recherche dans `kernelbase.dll` renvoie `ERROR_PROC_NOT_FOUND` (127), conformément aux observations natives, sans inventer un export. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

<!-- i18n-section: structured-exceptions -->

## Gestion structurée des exceptions

`WindowsProcessSEH` utilise le `X64SEH` partagé dans `os/windows/exception/` (`NeverDEmulationWindowsException`, disponible sans pilotes) pour x64 `__C_specific_handler` et UNWIND_INFO V1. Après VEH, il gère filtres, finally, transfert non local, exceptions imbriquées, collisions de déroulement et cadres EXE/DLL relocalisés, avec conservation des GPR/XMM non volatils. Une continuation par filtre exécute VCH avec le même `CONTEXT`. `WindowsSEHTests.cpp` compare 23 scénarios originaux à Windows natif ; KVM/WHP/Unicorn partagent cette sémantique. Le budget du processus couvre la revalidation des générations, en-têtes, octets de déroulement/portées, régions du gestionnaire de langage et liaisons IAT. Métadonnées modifiées ou images retenues déchargées provoquent un échec explicite. SEH ARM64 par cadres, C++ EH, tables de fonctions dynamiques, RtlUnwind/NtContinue généraux et déroulement traversant des callbacks du chargeur/VEH/VCH restent exclus.

Pour `EXCEPTION_NONCONTINUABLE`, un filtre x64 renvoyant `EXCEPTION_CONTINUE_EXECUTION` déclenche `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, indicateurs `0x81`, enregistrement lié nul) avec un nouveau contexte. VEH est relancé avant la recherche dans la pile logique conservée, avec le même ordre finally, les mêmes identités de cadres EXE/DLL et les mêmes budgets de profondeur et d’exécution. Les 23 scénarios natifs comprennent 21 exécutions réussies et deux terminaisons : accepter la continuation de cette exception secondaire dans VEH/VCH laisse l’exception non traitée, même après restauration du `CONTEXT` initial. Le modèle signale un échec d’exécution. L’adresse d’une exception logicielle égale le PC sauvegardé ; les adresses du répartiteur interne et la disposition des registres relèvent du modèle. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

<!-- i18n-section: processor-state -->

## Instructions et état du processeur

Le profil x64 vérifié inclut `MOVS/STOS/LODS` sur RAM ordinaire et `CLD/STD`, avec reprise, arrêt et validation des pages par élément. Les bits hauts à compte nul propres au CPU et les opérandes de périphérique STOS/LODS restent hors contrat.

Le profil x64 vérifié prend aussi en charge `CMPS/SCAS` sur RAM ordinaire avec `REPE/REPNE`, indicateurs arithmétiques, fin anticipée, arrêts par élément et reprise après défaut. Les comparaisons de périphériques restent exclues.

Les sondes natives x64 et ARM64 valident une exécution complète bornée sous bail mémoire exclusif. Les paquets XSAVE et les caches de tables identifiés par ISA ont une autorité unique.

Les champs x64 natifs `FOP/FIP/FDP` suivent les règles de sauvegarde/restauration hôte : AMD peut effacer les métadonnées x87 inactives. Les sondes de démarrage les valident avec une exception non masquée en attente.
