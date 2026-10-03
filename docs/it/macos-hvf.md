**Lingue**: [English](../macos-hvf.md) | [简体中文](../zh-CN/macos-hvf.md) | [繁體中文](../zh-TW/macos-hvf.md) | [日本語](../ja/macos-hvf.md) | [한국어](../ko/macos-hvf.md) | [Français](../fr/macos-hvf.md) | [Deutsch](../de/macos-hvf.md) | [Español](../es/macos-hvf.md) | [Italiano](macos-hvf.md) | [Русский](../ru/macos-hvf.md) | [العربية](../ar/macos-hvf.md)

<!-- i18n-source: 3d2e1d7ba9709cac62c969fd06f4cfe7d5312c872f9339afb0161bc9643c4e33 -->

[← Indice della documentazione](README.md)

# Esecuzione CPU nativa su macOS (HVF)

NeverD usa [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor) come equivalente macOS di KVM e WHP. `--backend hvf` lo seleziona esplicitamente; `auto` lo usa per un contratto nativo compatibile e ISA host/guest coincidenti: ARM64 su Apple Silicon, x86-64 su Intel. `software-cpu-v1` e l’esecuzione automatica tra ISA diverse usano Unicorn. Un eseguibile tradotto da Rosetta viene rifiutato.

Servono macOS 11 o successivo e virtualizzazione hardware; le altre dipendenze possono richiedere versioni più recenti. Il backend nativo selezionato segnala l’indisponibilità senza ripiego silenzioso. [Virtualization.framework](https://developer.apple.com/documentation/virtualization) gestisce VM complete; NeverD richiede il controllo di vCPU, registri, mappature ed eccezioni offerto da Hypervisor.framework.

## Compilazione e firma

Attivare `NEVERD_ENABLE_CPU_EMULATION=ON` oppure l’emulazione dei driver. `NEVERD_EMULATION_BACKEND_HVF` è `ON` per impostazione predefinita e collega il framework solo su macOS. Con `OFF`, il nome `hvf` resta riconosciuto, ma l’API delle capacità restituisce `build_disabled`.

Il collegamento del framework e la firma hypervisor sono limitati al target di compilazione macOS (`CMAKE_SYSTEM_NAME=Darwin`). I target mobili Apple, incluso iOS, non ricevono questa dipendenza né questo permesso. Un profilo guest iOS può comunque usare HVF quando NeverD viene eseguito su un Mac con ISA corrispondente.

L’**eseguibile del processo** deve avere [`com.apple.security.hypervisor`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor). Firmare soltanto `libneverd.dylib` non basta. CMake firma CLI, worker e test usando `resources/macos/neverd-hypervisor.entitlements`. `NEVERD_HVF_SIGN_IDENTITY` usa `-` per la firma ad hoc, oppure un’identità già disponibile. Il packaging riapplica e verifica il diritto dopo la correzione delle dipendenze Mach-O.

L’applicazione che incorpora la libreria firma il proprio eseguibile; NeverD non rifirma l’interprete Python installato. `cpu-capabilities --configuration=JSON --probe-host` controlla il processo effettivo. Il worker autonomo viene firmato per impostazione predefinita; usare `NEVERD_WORKER_SIGN_HVF=OFF` solo quando non necessita di HVF.

## Responsabilità ed esecuzione

`backends/hvf/HvfExecutor` possiede una VM per processo e una vCPU, create, usate e distrutte su un thread dedicato. Le CPU logiche condividono l’esecutore e serializzano gli ingressi. Il cambio CPU rimuove prima le due regioni fisiche del proprietario precedente. Distruggere una CPU inattiva non modifica le mappature altrui. I collegamenti vengono rimossi prima di liberare RAM; una registrazione parziale fallita viene annullata. Se la rimozione fallisce, la VM viene terminata prima del rilascio della memoria; un errore irrecuperabile di distruzione arresta il processo.

Le mappature host usano la dimensione di pagina host, inclusi 16 KiB su Apple Silicon. Tabelle architetturali e budget CPU guest restano a 4 KiB. Ammissione delle istruzioni, permessi, stato, transazioni di memoria e servizi OS rimangono nelle rispettive componenti. Il worker nativo non chiama osservatori guest né acquisisce i blocchi centrali della memoria del chiamante.

ARM64 esegue singolarmente cinque istruzioni immutabili di manutenzione TLB/I-cache, poi l’istruzione ammessa. `PSTATE.D` non maschera le eccezioni di debug indirizzate a EL2. Lo stato scalare, TLS e FP/SIMD viene acquisito integralmente. Intel negozia i controlli VMCS, usa monitor trap, invalida i TLB e trasferisce pacchetti XSAVE completi. RIP/RFLAGS passano direttamente per VMCS anche dopo la ricreazione della vCPU. CR0/CR4 rispettano maschere del framework e bit hardware obbligatori. Le uscite autenticate di lettura CR8 vengono completate nello strato ISA; altri accessi ai registri di controllo falliscono. Ogni vCPU inizializza un `IA32_KERNEL_GS_BASE` privato e gestito; gli accessi MSR guest restano intercettati, mentre MSR/SWAPGS non supportati sono rifiutati.

La coda conserva token di arresto e scadenza originali. Preparazione, manutenzione, ingresso e acquisizione condividono il budget. `RunDeadline` attende la conferma degli interrupt prima di restituire il controllo. L’annullamento ricrea la vCPU per isolare interrupt tardivi; quelli host Intel non pertinenti riprovano nella stessa generazione. Errori di acquisizione ed eccezioni autenticate prevalgono sull’arresto simultaneo; il normale stato annullato non viene pubblicato. L’annullamento è cooperativo, senza garanzie di tempo reale rigido.

## Verifica

Usare Release con CMake, Ninja, Python 3, Clang, `ld.lld`, `lld-link`, `ld64.lld` e `codesign`. I linker LLVM generano fixture ELF, PE e Mach-O; una fixture nativa obbligatoria assente causa un errore.

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon può aggiungere `-DNEVERD_LLVM_PREBUILT=ON`; Intel compila la revisione LLVM fissata. Il [workflow HVF](../../.github/workflows/hvf.yml) supporta runner `self-hosted, macOS, ARM64/X64, hvf` e `hosted-intel` su `macos-15-intel`. Verifica prima creazione e distruzione reali di VM/vCPU. `validation=probe` non prova l’esecuzione di istruzioni; `transport` controlla soltanto il trasporto; `darwin` richiede tutti i carichi Darwin compatibili; `full` richiede entrambe le verifiche complete CPU e Darwin.

Il trasporto richiede 12 casi ARM64 o 10 Intel; la verifica CPU completa richiede rispettivamente 16 o 14 controlli obbligatori. Copertura: stato completo, privilegi, permessi, attraversamento di pagine, alias, cambio CPU, rollback, annullamento e ripresa. Intel controlla CR8 prima della compilazione estesa. Gli artefatti conservano inventario, revisione, host, risultati e tentativi distinti. [GitHub](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) considera sperimentale la virtualizzazione annidata; resta utile un Mac nativo dedicato.

Su Intel ospitato, `--execution-methods` esegue in serie ogni metodo GoogleTest con tutti i parametri CTest, flag, ambiente e directory originali. Le proprietà sconosciute sono rifiutate. La scadenza complessiva per metodo è al massimo 120 secondi, senza limite separato per parametro; segue la terminazione del gruppo di processi con attesa limitata. Si conservano XML originale, associazioni dei nomi e stato d’uscita. Timeout o XML incompleto producono un fallimento parziale; casi nativi obbligatori mancanti o saltati impediscono il successo. I runner dedicati mantengono processi e scadenze CTest per singolo caso.

## Risultati e limiti

Stato al 2026-10-03; le righe si sovrappongono e non vanno sommate:

| Ambito | Sorgente | Superati | Falliti | Saltati | Obbligatori nativi |
| --- | --- | ---: | ---: | ---: | ---: |
| CPU ARM64, 20 target | `defc93928` | 849 | 0 | 5,993 | 16/16 |
| Darwin ARM64 | `defc93928` | 65 | 0 | 221 | 39/39 |
| Darwin Intel | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| Target FP Intel completo | `3e01cda5c` | 35 | 0 | 36 | 12 casi HVF |

ARM64 ha riconciliato 6,842 registrazioni; Intel ne ha 6,840 negli stessi 20 target. Sono passati anche tutti i 253 casi nativi Intel di eccezioni, divisione e transizione di stato. Il [run Darwin Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) verifica 286 identità e 32 processi, oltre a dieci casi di trasporto, 100 riprese e CR8. Collettore e audit hanno superato 124 controlli. L’integrazione copre CLI, SDK, worker e firma di 186 immagini Mach-O; le dipendenze del pacchetto richiedono macOS 15.0.

La verifica CPU Intel completa resta aperta. L’[esecuzione precedente](https://github.com/NeverSight/NeverD/actions/runs/37106679688) è terminata il 2026-10-03 alle 08:35 UTC; GitHub ha segnalato la perdita di comunicazione con il runner, senza artefatto CPU. Compilazione e verifiche preliminari non sostituiscono il risultato completo. Il confronto col kernel macOS non dimostra il comportamento del kernel su un dispositivo iOS.

Il piccolo benchmark ARM64 equivalente ha misurato 73.9 ms con Unicorn e 95.1 ms con HVF, circa il 29 % di tempo in più. Nessuna accelerazione è dimostrata. Un’istruzione ordinaria richiede sei ingressi nativi; misure con host molto carico non provano prestazioni stabili. Consultare le [prove dettagliate](../macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03) e il [contratto Darwin limitato](darwin-emulation.md).

## Inventario Intel completo suddiviso in gruppi

L’esecuzione completa `37106679688` è terminata il 2026-10-03 alle 08:35 UTC; GitHub segnala la perdita di comunicazione con il runner. Tutti i quattro job di `37116327329` hanno perso la connessione senza produrre XML CPU. Questi fatti non identificano un’istruzione guest difettosa. Intel ospitato in modalità `full` utilizza quattro job, con al massimo due contemporanei. Ciascuno esegue quattro batch consecutivi: sedici partizioni complessive, numerate `job + 4 × batch`, mantenendo l’insieme originale di test di ogni job. Ogni batch compila e controlla prima l’intero inventario CTest dei venti target. `--hvf-shard INDEX/COUNT` mantiene insieme metodi interi e tutti i parametri, anche quando le proprietà di esecuzione differiscono.

Prima dell’esecuzione CPU, `scripts/prepare_hvf_batches.py` salva inventario completo, selezioni e piani dei metodi; il workflow carica separatamente questa diagnostica. Un’azione composita locale esegue quattro batch e carica subito dopo ciascuno XML originale, corrispondenze, stati dei processi e lista consentita delle variabili necessarie. Un unico limite esterno di 30 minuti comprende tutti i batch e i caricamenti. Il fallimento di un batch impedisce l’esecuzione dei successivi. Dopo errore o timeout, un caricamento diagnostico separato dispone di due minuti se il runner rimane raggiungibile; con la perdita della connessione restano soltanto le prove già caricate. Piani e pacchetti incompleti non valgono come partizioni superate.

Un job Linux separato esegue `scripts/audit_hvf_shards.py` e ricava nuovamente target e requisiti nativi dai sorgenti estratti. Il workflow scarica solo artefatti CPU del tentativo corrente; l’audit richiede tutte le sedici partizioni dello stesso commit senza modifiche locali, ISA macOS corretta e contratti normalizzati coincidenti. I risultati devono essere disgiunti e coprire esattamente l’inventario completo; tutti i processi devono terminare correttamente e ogni requisito nativo deve passare. Partizioni mancanti, filtri modificati, riepiloghi incoerenti, XML incompleto o requisiti saltati causano errore. Ogni job nativo conserva trasporto, ripresa, CR8 e verifica Darwin indipendente. I runner propri mantengono CTest senza suddivisione. I batch da soli non dimostrano l’accettazione Intel. Ogni nuovo tentativo deve rieseguire tutti i job nativi; gli artefatti dei tentativi precedenti non vengono combinati.

Il primo batch CPU verificato dell’[esecuzione `37123148209`](https://github.com/NeverSight/NeverD/actions/runs/37123148209), sorgente pulito `f5f29a484`, è la partizione `1/16`: 476 risultati registrati in 31 processi di metodi, con 82 superati, 0 falliti e 394 saltati; anche il suo unico caso nativo obbligatorio è passato. È stato verificato lo SHA-256 dell’artefatto `11274755752`; XML originale, stati di uscita, inventario e piano dei metodi sono stati confrontati con il piano precedente all’esecuzione. Questa prova Intel parziale non conclude la verifica CPU completa.

## Ultima verifica nativa locale

Sorgenti senza modifiche locali `4ce0b8247`, 2026-10-03 UTC. L’inventario ARM64 completo è passato in 503 processi di metodo; ogni risultato XML originale, contratto di esecuzione e terminazione del processo figlio è stato verificato indipendentemente. Anche la verifica Darwin separata è passata. Le righe si sovrappongono e non vanno sommate.

| Ambito | Registrati | Superati | Falliti | Saltati | Obbligatori nativi |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU, inventario completo | 7,003 | 867 | 0 | 6,136 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-current-4ce0-full-evidence/summary.json` · `build-hvf-native/hvf-current-4ce0-darwin-evidence/summary.json`

## Diagnostica Intel indipendente

Il [workflow diagnostico Intel](../../.github/workflows/hvf-intel-diagnostic.yml), avviato manualmente, estrae separatamente il controller e i sorgenti verificati. `source-ref` richiede uno SHA completo; `shards` seleziona fra le sedici partizioni originali. `first-method` parte da zero, `method-count=0` seleziona i metodi rimanenti e `case-index` può scegliere un parametro originale solo con `method-count=1`. Prima della selezione viene rilevato l’inventario completo dei venti target. Build Release, comandi, parametri e verifiche native obbligatorie originali restano invariati.

`intel-image` seleziona per impostazione predefinita `macos-15-intel`, oppure `macos-26-intel` per un confronto controllato, come nel workflow completo. Il titolo dell’esecuzione indica l’immagine scelta e il controllo di disponibilità richiede sempre un host x86-64 nativo. Cambiare immagine coinvolge sistema operativo, SDK e strumenti di compilazione; non isola una modifica del kernel.

La compilazione dispone di 120 minuti all’interno di un job limitato a 180 minuti: la prima build completa su macOS 26 ha richiesto 76 minuti. Ogni metodo nativo resta limitato a 120 secondi e l’azione diagnostica a 30 minuti.

Prima di ogni metodo, l’azione carica il piano immutabile e un’istantanea dell’host. Dopo l’esecuzione conserva XML originale, terminazione dei processi, stato del controller e una seconda istantanea. Registra memoria, swap, carico, disco e identificatori, stato, CPU, RSS e nomi degli eseguibili dei processi, senza argomenti o ambiente. Anche gli errori di raccolta restano visibili. Un errore di esecuzione o caricamento ferma i metodi successivi. Ogni metodo mantiene il limite di 120 secondi; un host irraggiungibile può impedire pulizia e caricamento finale. In tal caso rimangono solo le prove già caricate. Queste diagnosi parziali non soddisfano la verifica CPU completa né quella Darwin indipendente. L’ultimo marcatore iniziale individua un confine di esecuzione, non l’istruzione guest difettosa o la causa.

Anche il workflow completo `Native macOS HVF` accetta un `source-ref` facoltativo. Il valore predefinito è il commit del workflow; un valore esplicito deve essere uno SHA completo. I job nativi e l’audit aggregato estraggono e verificano gli stessi sorgenti. L’audit confronta le prove con il commit testato, anche se il controller usa una revisione diversa.

Per `hosted-intel`, il workflow completo accetta `intel-image=macos-15-intel` (predefinito) oppure `macos-26-intel`, presenti nelle [immagini ufficiali dei runner](https://github.com/actions/runner-images). È così possibile confrontare esplicitamente gli ambienti host con lo stesso `source-ref`; l’immagine cambia anche sistema operativo, SDK e strumenti. I requisiti per VM/vCPU, trasporto nativo, CR8, CPU completo e Darwin restano invariati. La sola scelta dell’immagine non dimostra stabilità né una correzione in esecuzione.

`sample-active-child=true` conserva facoltativamente un’istantanea sigillata dopo cinque secondi di esecuzione di un metodo: un secondo di campionamento degli stack del processo figlio nativo di cui è stata verificata l’identità, fino a 1 MiB della coda del log corrente e lo stato dell’host. Il valore predefinito è `false`. Il campionamento accetta al massimo 166 metodi per job per rispettare il limite degli artifact; il comando ha una scadenza di cinque secondi e il rapporto un limite di 1 MiB. Gli errori di identificazione e raccolta vengono registrati. Il caricamento usa una directory immutabile separata; se fallisce, annulla il processo figlio nativo e fa fallire l’action. Il campionamento modifica la pianificazione ed è indicato come evidenza parziale strumentata. Non riavvia il timer originale del metodo e non sostituisce la convalida completa.
