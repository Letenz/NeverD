**Lingue**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← Progetto NeverD](project.md)

# Documentazione NeverD

Panoramica, build e CLI sono nel README del repository. I riferimenti di design e test per i contributor sono raccolti qui.

NeverD supporta Android e iOS tramite la CLI sperimentale `neverd mobile`: APK (incluso multidex), DEX e file o directory smali diventano Java con un report JSON; IPA, `.app` e Mach-O (arm64/x86_64) diventano C nativo e sorgenti Objective-C/Swift supportati con un report di copertura JSON. L’estensione dipende dai modelli di codice supportati; le guide ne descrivono i limiti.

L’esecuzione CPU separa ammissione ISA, memoria guest, trasporto del backend e politiche OS. `NEVERD_ENABLE_CPU_EMULATION` attiva il livello CPU x64/ARM64; `NEVERD_ENABLE_DRIVER_EMULATION` aggiunge l’ambiente Windows WDM/KMDF x64 limitato. `linux-elf64-v1` esegue processi Linux ELF supportati. Vedere [Esecuzione CPU](cpu-execution.md), [Emulazione dei processi guest](process-emulation.md) e [Emulazione dei driver Windows](driver-emulation.md).

I profili checked usano KVM su Linux e WHP su Windows con ISA corrispondente, e Unicorn tra ISA diverse. La validazione nativa ARM64/WHP resta da eseguire. `driver-strict` usa attualmente Unicorn; il percorso driver KVM/WHP richiede `checked-x64-v1`. Un backend esplicito non disponibile fallisce chiaramente. RAM condivisa, alias, scritture per fasi, operazioni atomiche scalari, eccezioni tipizzate e stato FP/SSE x64 completo sono implementati nei contratti ISA/OS documentati. Ciò non garantisce driver arbitrari o ambienti Android/Darwin implementati.

| Documento | Descrizione |
|-----------|-------------|
| [README (italiano)](project.md) | Panoramica, avvio rapido, build, SDK, CLI |
| [Contribuire](CONTRIBUTING.md) | Ambiente, profili di build, workflow, stile e requisiti PR |
| [Architettura](architecture.md) | Percorsi IR, confini dei componenti, lifting strict, profondità del supporto e punti di modifica |
| [Test](testing.md) | Suite, fixture generate, roundtrip Unicorn e comandi incrementali |
| [Esecuzione CPU](cpu-execution.md) | Configurazione, capacità, disponibilità dei backend ed esiti tipizzati |
| [Emulazione dei processi guest](process-emulation.md) | Profilo Linux ELF, avvio, servizi, limiti e test |
| [Prove bitvector](solver.md) | Prove Z3 opzionali, sintesi verificata, test indipendenti ed export |
| [Recupero del sorgente da interpreti](interpreter-recovery.md) | Specializzazione sperimentale `--devirtualize`, controlli CLI, contratto di esecuzione, evidenze e limiti; proposte di prova per cicli annidati; budget di scoperta espliciti e API C versionata |
| [Emulazione dei driver Windows](driver-emulation.md) | Ciclo WDM/KMDF x64 limitato, richieste, scenari hardware, SEH, sottoinsiemi PnP, selezione backend e limiti |
| [Ricostruzione delle eccezioni Windows](windows-exception-reconstruction.md) | Matrice di supporto SEH/C++, contratto IR, regole di patch nativo e validazione PE |
| [Audit e hunt di sicurezza della memoria](memory-safety.md) | Analisi di vita dell’heap e overflow di copia: contratto di identità per formato, catalogo sink/source, verdetti, budget e schema JSON |
| [Plugin nativi](plugins.md) | ABI del descrittore C puro, callback ed eventi, flusso di build/link, rilevamento e regole di compatibilità |
| [Plugin Python](python-plugins.md) | Sviluppo, API di sessione ed eventi, isolamento, test e pubblicazione |
| [Decompilazione EVM](evm.md) | Input, hardfork, IR a stadi, ABI host C/LLVM, ricostruzione Solidity e limiti |
| [Decompilazione Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, output C/Rust, verifica e limiti noti |
| [Panoramica mobile (English)](../mobile.md) | CLI sperimentale Android/iOS, input, output, report e limiti |
| [Ricostruzione Java per Android](android.md) | APK (incluso multidex), DEX, file/directory smali → Java; CLI, report JSON, risoluzione dei problemi, limiti e verifica |
| [Recupero dei sorgenti iOS](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → C nativo e sorgenti Objective-C/Swift supportati; layout, CLI/export, copertura JSON, limiti e prove eseguibili |
| [Roadmap](roadmap.md) | Stato: formati nativi, EVM e Solana SBF implementati |
| [English README](../../README.md) | Versione inglese |
| [Altre lingue](../README.md) | Altre versioni localizzate |
