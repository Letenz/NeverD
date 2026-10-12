**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: b504f377f29d587519311580a92c9cdaed9118057832a8afd81551fbd34802df -->

[← Documentation Index](README.md)

# Patch da sorgenti e oggetti nativi

NeverD compila una funzione sostitutiva con il compilatore scelto e la installa in un ingresso originale esplicito. Il compilatore definisce la semantica C/C++; NeverD gestisce indirizzi, rilocazioni, memoria, record di unwinding e pubblicazione. La riparazione binaria e la riscrittura del codice sollevato mantengono le proprie pipeline. Il resto del programma non viene ricompilato.

<!-- i18n-section: compiler -->
## Scelta del compilatore

Scegliere `--compiler=clang`, `--compiler=gcc` o `--compiler=neverc`. Clang e NeverC in modalità ordinaria possono produrre LLVM IR; `object` conserva le istruzioni native. La CLI sceglie gli oggetti per GCC per impostazione predefinita. Il SDK Python richiede `artifact="object"` esplicito per GCC. Non viene mai usato implicitamente un altro compilatore.

Sostituire gli indirizzi di esempio con ingressi originali verificati. La dichiarazione deve corrispondere all’ABI originale, incluse convenzione di chiamata e disposizione delle strutture; un indirizzo non lo dimostra. C++ richiede `--language=c++` e l’eseguibile Clang++/G++ corretto. Preferire un ingresso `extern "C"` o indicare il nome decorato esatto. Sono consentite funzioni ausiliarie. `--entry` e `--func` selezionano esplicitamente la sostituzione; anche il vecchio `--from-c` rispetta l’indirizzo.

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
## Profili verificati e limiti

| Compilatore | Artefatto | Profilo runtime OS verificato |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++; Mach-O x64/ARM64 C/C++; PE x64 C/C++ FH3 |
| Clang 22.1.6 | Oggetto nativo | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | Oggetto nativo | ELF x64 C/C++ con runtime Itanium compatibile |
| NeverC 3389.1.5 modalità ordinaria | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 modalità ordinaria | Oggetto nativo | ELF x64 C |

Sono profili verificati, non garanzie per ogni versione od opzione. `dyncode` e NeverC C++ non sono ammessi. GCC non produce LLVM IR. Gli oggetti nativi COFF/Mach-O non sono ammessi; l’installazione di oggetti richiede ELF x64. Rilocazioni sconosciute, TLS, costruttori, LTO, simboli common/IFUNC e import non risolti falliscono esplicitamente. La ricevuta JSON registra compilatore, argomenti, associazioni e indirizzi installati; gli oggetti includono anche SHA-256.

La matrice CPU separata copre quattro architetture (`x86`, `x64`, `ARM32`, `ARM64`) e tre formati (`ELF`, `PE`, `Mach-O`), verificando l’installazione o il rifiuto esplicito. L’esecuzione CPU isolata non certifica il loader OS, l’unwinder C++ o ABI arbitrarie. Per queste conclusioni usare i profili runtime sopra.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

`CPU` indica installazione ed esecuzione isolata delle funzioni; `REFUSED` indica un contratto di installazione mancante. ARM32 ELF richiede registrazione EHABI; Mach-O a 32 bit richiede contratti verificati di loader, frontend e unwinding; l’installazione PE da sorgente richiede attualmente x64.

La matrice CPU usa aritmetica, rami e chiamate ausiliarie autonome. Non certifica riferimenti dati ELF x86/ARM64 o import C++ ARM64; le prove aggiuntive hanno rifiutato i loro contratti di rilocazione non supportati.

<!-- i18n-section: references -->
## Riferimenti originali e memoria

Le associazioni corrispondono a dichiarazioni esterne: `function` richiede una funzione originale o uno stub import verificato; `data` richiede memoria scrivibile; `const` richiede una dichiarazione IR costante. I dati degli oggetti richiedono un’estensione non nulla. I limiti della memoria non possono essere superati, né RELRO può diventare scrivibile. I callback al corpo originale sostituito sono rifiutati: questo profilo non conserva il prologo in un trampolino.

Il nuovo codice resta RX e lo stato mutabile RW. Si riutilizzano solo import esistenti; gli slot GOT/IAT restano distinti dagli stub chiamabili. Mach-O richiede spazio per comandi di caricamento, per esempio `-Wl,-headerpad,0x400`; PE richiede spazio per intestazioni di sezione. Lo spazio insufficiente provoca un errore. ELF `.data.rel.ro` e le protezioni CET/IBT originali richiedono contratti aggiuntivi e sono attualmente rifiutati.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

La crescita delle tabelle Mach-O richiede anche spazio libero adiacente nel file e nello spazio virtuale; la sola riserva per intestazioni non basta. I fixture verificati distinguono questi requisiti.

<!-- i18n-section: exceptions -->
## Eccezioni C++

ELF usa FDE/LSDA del compilatore; Mach-O usa compact unwind/DWARF. PE x64 ammette tabelle FH3 con runtime MSVC esistente compatibile e identità dei tipi coerenti. I dati type-info possono richiedere associazioni esplicite. FH4, personality personalizzate, sorgenti SEH e modalità CFG/EHCont/XFG attive sono rifiutati; le protezioni originali restano invariate.

I casi runtime coprono ritorno normale, catch interno, una nuova eccezione catturata dal chiamante originale, un’eccezione dall’originale attraverso la sostituzione, pulizia RAII esattamente una volta, rilancio, terminazione noexcept e funzioni intatte. Il punto di chiamata originale deve già consentire la propagazione. Una patch non ripristina catch eliminati attorno a una funzione dimostrata non generare eccezioni.

<!-- i18n-section: publication -->
## SDK e pubblicazione

L’API C aggiuntiva è `neverd_patch_source` con `neverd_source_patch_options_v1`; `neverd_patch_source_report` restituisce la ricevuta. L’input oggetto specifica la lunghezza in byte. La vecchia API `neverd_patch_from_c` resta disponibile.

Gli input vengono fissati. Il candidato viene preparato accanto alla destinazione, ricaricato e verificato per ogni ingresso prima della pubblicazione esclusiva. File esistenti e alias simbolici non vengono sovrascritti. Una sola mappatura assente fa fallire l’intera richiesta. `--mode=inplace` è rifiutato. Le trasformazioni IR della sessione non si applicano agli oggetti nativi. Mach-O verifica e rinnova rigorosamente le firme; le firme PE non vengono eliminate silenziosamente.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Compilazione e verifica

Scegliere un compilatore compatibile con `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 verificato). La compilazione integrata corregge solo una copia LLVM nel percorso di build, senza modificare sottomodulo o cache condivisa. Il difetto PLT x64 dei pacchetti r3/r4 può rifiutare una patch IR; gli oggetti nativi evitano questo percorso MC. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` richiede la produzione PLT esatta e corretta.

`NeverDSourcePatchTests` contiene casi nativi C/API/oggetti/eccezioni; `NeverDSourcePatchMatrixTests` verifica tutte le combinazioni con Unicorn. `SourcePatchPE` esegue sette scenari C++ con `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` e `NEVERD_TEST_WINDOWS_SDK_LIB`. Su host non Windows richiede anche `NEVERD_TEST_WINDOWS_RUNNER`. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` considera un errore gli input mancanti. Il workflow Windows dedicato richiede esecuzione nativa e rifiuta casi saltati.

Le prove Linux sul Mac di sviluppo usano Docker e un loader Linux esplicito; PE usa Wine 11. Mach-O ARM64 viene eseguito sul Mac stesso. I risultati CI Windows nativi vanno controllati separatamente: configurare un job non equivale a superarlo. Conteggi storici, errori baseline e limiti degli emulatori sono nei [contratti dettagliati e nel registro di convalida](../source-patching-details.md).

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

La copia LLVM corregge anche i riferimenti dati GOT Mach-O x64: la tabella catch tipizzata di Clang è relativa alla fine della parola di rilocazione di quattro byte. r3/r4 la sposta di quattro byte; la verifica prima della pubblicazione rifiuta il risultato. Sul Mac, x64 usa Rosetta 2 e ARM64 viene eseguito direttamente.

La verifica nativa Windows x64 [96cf7cd59](https://github.com/NeverSight/NeverD/actions/runs/38200205814) ha superato tutti i 15 test mirati PE/oggetti/trampoline e i 48 test della matrice dei target, senza casi saltati. Il test FH3 ha eseguito sette scenari C++. Questa esecuzione ha usato Clang 22.1.6 con MSVC e Windows SDK nativi.
