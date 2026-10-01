**Lingue**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 62a52f8c335f25f75bf285bd21350fb9d787a00b7b9a357f399c3df72c4c52cc -->

[← Progetto NeverD](project.md)

# Documentazione NeverD

Panoramica, build e CLI sono nel README del repository. I riferimenti di design e test per i contributor sono raccolti qui.

**Supporto mobile (CLI sperimentale):** `neverd mobile` recupera Java da APK, DEX e smali [Android](android.md), e C nativo e sorgenti Objective-C/Swift supportati da IPA, `.app` e Mach-O [iOS](ios.md). I report JSON descrivono risultati e copertura. Iniziare dalla [panoramica mobile (inglese)](../mobile.md), poi consultare le guide di piattaforma per comandi e limiti.

Le guide inglesi sono direttamente in `docs/`. Le traduzioni sono raggruppate in `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/` e `zh-TW/`. Ogni directory contiene l’indice `README.md`, la panoramica `project.md`, guide tematiche, `CONTRIBUTING.md`, `ATTRIBUTION.md` e `roadmap.md`. Le immagini condivise sono in `assets/`.

L’esecuzione CPU separa ammissione ISA, memoria guest, trasporto del backend e politiche OS. `NEVERD_ENABLE_CPU_EMULATION` attiva il livello CPU x64/ARM64; `NEVERD_ENABLE_DRIVER_EMULATION` aggiunge l’ambiente Windows WDM/KMDF x64 limitato. `linux-elf64-v1` esegue processi Linux ELF supportati. Vedere [Esecuzione CPU](cpu-execution.md), [Emulazione dei processi guest](process-emulation.md) e [Emulazione dei driver Windows](driver-emulation.md).

`driver-strict` / `checked-x64-v1` supporta KVM su host Linux x64 compatibili e WHP su host Windows x64 compatibili; `auto` sceglie quel trasporto nativo, mentre ISA diverse usano Unicorn. Unicorn esplicito e la precedente API V1 mantengono il profilo software portabile. L’esecuzione nativa verifica indirizzi canonici ed effetti prima dell’ingresso; hardware assente produce un errore senza ripiego. Istruzioni e comportamento OS non supportati falliscono esplicitamente. Mancano prove native ARM64/WHP; non è stabilita la compatibilità universale dei driver o Android/Darwin.

`checked-aarch64-v1` e `checked-user-aarch64-v1` offrono ARM64 FP32/FP64 e SIMD fissi limitati, con stato FPCR/FPSR/vettoriale completo. Linux ARM64 corrispondente usa KVM, Windows ARM64 usa WHP e ISA diverse usano Unicorn. Le prove native ARM64 restano pendenti; il caricamento di driver Windows resta x64.

| Documento | Descrizione |
|-----------|-------------|
| [README (italiano)](project.md) | Panoramica, avvio rapido, build, SDK, CLI |
| [Contribuire](CONTRIBUTING.md) | Ambiente, profili di build, workflow, stile e requisiti PR |
| [Architettura](architecture.md) | Percorsi IR, confini dei componenti, lifting strict, profondità del supporto e punti di modifica |
| [Test](testing.md) | Suite, fixture generate, roundtrip Unicorn e comandi incrementali |
| [Ambiente desktop (inglese)](../gui.md) | Interfaccia Qt Quick opzionale, worker separato, ABI C, annotazioni e flussi MCP |
| [Validazione del desktop (inglese)](../gui-qualification.md) | Misure GUI, limiti del packaging e validazione delle piattaforme ancora necessaria |
| [Recupero del sorgente da interpreti](interpreter-recovery.md) | Specializzazione sperimentale `--devirtualize`, controlli CLI, contratto di esecuzione, evidenze e limiti; proposte di prova per cicli annidati; budget di scoperta espliciti e API C versionata; API di prova esatta dal codice nativo a LLVM |
| [Ricostruzione delle eccezioni Windows](windows-exception-reconstruction.md) | Matrice di supporto SEH/C++, contratto IR, regole di patch nativo e validazione PE |
| [Esecuzione CPU](cpu-execution.md) | Configurazione, capacità, disponibilità dei backend ed esiti tipizzati |
| [Prove bitvector](solver.md) | Prove Z3 opzionali, sintesi verificata, test indipendenti ed export |
| [Emulazione dei processi guest](process-emulation.md) | Profilo Linux ELF, avvio, servizi, limiti e test |
| [Emulazione dei driver Windows](driver-emulation.md) | Ciclo WDM/KMDF x64 limitato, richieste, scenari hardware, SEH, sottoinsiemi PnP, selezione backend e limiti |
| [Audit e hunt di sicurezza della memoria](memory-safety.md) | Analisi di vita dell’heap e overflow di copia: contratto di identità per formato, catalogo sink/source, verdetti, budget e schema JSON |
| [Plugin nativi](plugins.md) | ABI del descrittore C puro, callback ed eventi, flusso di build/link, rilevamento e regole di compatibilità |
| [Plugin Python](python-plugins.md) | Sviluppo, API di sessione ed eventi, isolamento, test e pubblicazione |
| [Panoramica mobile (inglese)](../mobile.md) | CLI sperimentale Android/iOS, input, output, report e limiti |
| [Ricostruzione Java per Android](android.md) | APK (incluso multidex), DEX, file/directory smali → Java; CLI, report JSON, risoluzione dei problemi, limiti e verifica |
| [Recupero dei sorgenti iOS](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → C nativo e sorgenti Objective-C/Swift supportati; layout, CLI/export, copertura JSON, limiti e prove eseguibili |
| [Decompilazione EVM](evm.md) | Input, hardfork, IR a stadi, ABI host C/LLVM, ricostruzione Solidity e limiti |
| [Decompilazione Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, output C/Rust, verifica e limiti noti |
| [Roadmap](roadmap.md) | Stato: formati nativi, EVM e Solana SBF implementati |
| Documentazione tradotta | I collegamenti linguistici in alto aprono l’indice e la panoramica di ogni lingua |
