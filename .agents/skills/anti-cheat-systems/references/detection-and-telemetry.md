# Detection And Telemetry

## Detection Mechanisms

### Memory Detection
```
- Code section hashing and integrity verification
- Executable private memory and manual-map detection
- Injected module and anomalous image mapping detection
- Memory modification and stack provenance monitoring
```

### Process Detection
```
- Handle access stripping and protected-process enforcement
- Thread start address, APC, and context inspection
- Debug register and hidden-thread monitoring
- Stack trace and module-correlation analysis
```

### Kernel-Level Detection
```
- Driver verification, signature policy, and blocklist checks
- Callback registration and object access monitoring
- System call, dispatch table, and hook integrity checks
- PatchGuard, test-signing, and kernel trust state checks
- Kernel pool scanning (Segment Heap aware) for hidden drivers and shellcode
```

### Kernel Pool Scanning (Segment Heap Era)
```
Why Segment Heap matters for anti-cheat:
Cheat drivers allocate memory in NonPagedPool for shellcode, hook tables,
hidden modules. The Segment Heap (19H1+) changed pool internals:
headers are HeapKey XOR encoded, allocation paths are split (kLFH, VS,
Segment, Large), metadata is isolated. Anti-cheat pool scanners must
understand these mechanisms to scan accurately without false positives.

Detection targets:

1. BigPool / Large Allocation scanning:
   - Walk nt!PoolBigPageTable (nt!PoolTrackTable)
   - Find allocations without corresponding DRIVER_OBJECT or loaded module
   - Detect manually mapped drivers that allocate large pool chunks
   - Large allocations have no inline header; metadata is external

2. VS Allocator chunk scanning:
   - Traverse _SEGMENT_HEAP → VsContext → SubsegmentList
   - Decode _HEAP_VS_CHUNK_HEADER using HeapKey:
     real_sizes = encoded_header ^ chunk_address ^ HeapKey
   - Check decoded chunk for suspicious PoolTag, executable content,
     or allocation without matching driver
   - VS chunks carry both _HEAP_VS_CHUNK_HEADER (encoded) and
     _POOL_HEADER (PoolTag still present)

3. kLFH bucket scanning:
   - _SEGMENT_HEAP → LfhContext → Buckets[] → AffinitySlots → Subsegments
   - kLFH randomizes block placement (harder to predict adjacency)
   - FreeHint encoded with LfhKey
   - Allocation pattern anomalies in specific size buckets can indicate
     pool grooming by cheat drivers

4. Suspicious PoolTag detection:
   - Cheat drivers use custom or rare tags; maintain blacklist
   - Cross-reference tags against known-good tag database (pooltag.txt)
   - Tags present in pool but absent from any loaded module = suspicious

5. Executable memory in NonPagedPool:
   - Find chunks with X permission but no corresponding module
   - Scan decoded chunk content for known cheat signatures, ROP gadgets,
     specific syscall stub patterns

6. Segment Heap integrity checks:
   - Validate _SEGMENT_HEAP.Signature == 0xDDEEDDEE
   - Verify VS chunk header encoding consistency
   - Detect tampered heap metadata (indicates heap exploitation attempt)

Required knowledge for scanner:
- nt!RtlpHpHeapGlobals (HeapKey, LfhKey) — obtained via pattern scan
- nt!ExpPoolQuotaCookie — for ProcessBilled decoding
- Per-pool-type _SEGMENT_HEAP instance addresses (nt!PoolVector)
- Allocation path determination (size → kLFH/VS/Segment/Large)

Anti-cheat KDP integration:
- Store detection rule tables in Secure Pool (ExAllocatePool3 + KDP)
- Rules become VTL0-immutable — cheat drivers cannot modify
  detection signatures even with kernel R/W primitives
```

### Behavioral Analysis
```
- Raw input timing and pattern analysis
- Movement and aim anomaly detection
- Statistical improbability and ML-assisted scoring
- Telemetry collection and server-side review
- AI visual aimbot detection (input pattern + gameplay behavior)
```

### AI Visual Aimbot Detection
```
AI visual cheats (OBS capture + YOLO + hardware input) are the hardest
class to detect because they involve no memory access, no code injection,
no kernel driver on the gaming PC. Detection must move to behavioral
analysis and environmental signals.

Input Pattern Analysis:
- Mouse movement micro-signature: AI-generated trajectories have
  characteristic acceleration profiles distinct from human muscle control
  even with smoothing and jitter injection
- Engagement timing: AI reacts within a narrow, consistent latency band
  (capture → inference → output, typically 20-50 ms total) that lacks
  the variance of human reaction time distributions
- Sub-pixel precision: hardware input devices report integer deltas,
  but AI-calculated movements exhibit systematic rounding patterns
  that differ from natural hand movement
- Correction patterns: AI trajectories show characteristic overshoot-and-settle
  patterns at consistent magnitudes; human correction is more variable
- Target switching: AI switches between targets with machine-like
  priority ordering (closest-to-crosshair or highest-confidence);
  humans exhibit attention bias and tunnel vision

Gameplay Behavioral Signals:
- Anomalous K/D ratio combined with other statistical outliers
- "Snap" engagement pattern: rapid crosshair movement to target
  followed by immediate fire, repeated consistently
- FOV-limited engagement: AI only engages targets within a specific
  pixel radius (the configured FOV), creating an unnatural engagement
  boundary visible in replay analysis
- Consistent headshot angle distribution that doesn't match
  the player's ranked skill bracket
- Engagement rate: AI engages available targets at a higher percentage
  than human players who miss, ignore, or react slowly to some targets

Environmental Detection:
- OBS process detection: Game Capture mode injects a graphics hook DLL
  (obs-graphics-hook64.dll) into the game process; detectable via
  loaded module enumeration, though banning it risks false-positive
  on legitimate streamers
- Window Capture detection: DXGI Desktop Duplication creates detectable
  API state (IDXGIOutputDuplication usage patterns)
- Frame readback detection: unusual GPU-to-CPU copy patterns
  (ReadbackTexture / staging resource creation at frame rate)
- Known hardware input device USB VID/PID signatures
  (KMBox, certain Arduino/Teensy boards)
- USB device enumeration anomalies: input device appearing/changing
  mid-session
- Logitech driver version detection: known exploitable G HUB versions
- Interception driver (interception.sys) loaded = high-risk signal

Server-Side Statistical Analysis:
- Aim trajectory reconstruction from server-received input deltas
- Compare aim distribution against player population at same rank
- Detect systematic per-frame aim correction vectors
  (AI produces consistent delta sequences)
- Cross-session pattern analysis: AI users show unnaturally stable
  performance metrics across sessions (low variance in accuracy)
- Replay-based ML classifiers trained on confirmed AI aimbot cases

Anti-AI Countermeasures (Game Design):
- Visual disruption: flashbang/smoke effects that confuse CV models
- Character skin variety and camouflage that reduce YOLO confidence
- Dynamic UI elements overlapping character models
- Server-side aim validation: reject physically impossible aim transitions
- Randomized character proportions or outline disruption to break
  trained model assumptions
```

### Server-Side Replay Analysis for AI Aimbot Detection
```
Server-side detection operates on input telemetry data uploaded
from the client, independent of what runs on the gaming PC.
This is the strongest layer against zero-memory AI cheats.

Input Telemetry Collection:
- Record raw mouse delta (dx, dy) per tick at server tick rate
- Record timestamps of each input event (sub-millisecond precision)
- Record crosshair angle / view angle per tick
- Record fire events with corresponding view angle at fire time
- Record damage events with hit location (head/body/limb)
- Collect per-session: total engagement count, hit count,
  headshot count, K/D, average engagement distance

Replay-Based Trajectory Reconstruction:
- Reconstruct full crosshair trajectory from recorded input deltas
- Overlay trajectory onto 3D game state (player positions, obstacles)
- Identify "engagement windows": trajectory segments where crosshair
  moves toward and locks onto a target
- Measure per-engagement: time-to-target, overshoot magnitude,
  correction count, final hold time before fire

Statistical Features for AI Detection:
  (extracted from reconstructed trajectories)

Temporal features:
- Reaction time distribution: time from target visibility to
  first crosshair movement toward target
  → AI: narrow, consistent band (30-80 ms capture+inference+output)
  → Human: wide, right-skewed distribution (150-400 ms typical)
- Time-to-lock distribution: time from engagement start to
  crosshair on target
  → AI: consistent, speed-limited by smoothing algorithm
  → Human: highly variable, depends on initial angular distance

Spatial features:
- Trajectory curvature: AI Bézier curves have characteristic
  smooth, parametric curvature; human paths are more erratic
  with micro-corrections and random deviations
- Overshoot-correction ratio: AI smoothing algorithms produce
  predictable overshoot magnitudes; humans vary wildly
- End-point precision: AI consistently lands on bounding box
  center or specific offset; humans show scatter distribution
- Angular velocity profile: AI ramps up/down smoothly;
  humans have irregular acceleration spikes

Engagement pattern features:
- Target selection consistency: AI always picks closest-to-crosshair
  or highest-confidence target; humans show attention bias, tunnel
  vision, and suboptimal target priority
- FOV boundary effect: AI shows sharp engagement cutoff at
  configured pixel radius; humans have gradual falloff
- Engagement rate: percentage of visible targets engaged;
  AI engages more consistently than humans who miss, ignore,
  or react slowly to peripheral targets
- Multi-target switch pattern: AI switches with machine-like
  regularity; humans show grouping and hesitation
```

### ML Classifier for AI Aimbot Detection
```
Feature engineering and model architecture for detecting
AI-generated mouse input at scale.

Feature Vector (per engagement window):
  f1:  reaction_time_ms
  f2:  time_to_lock_ms
  f3:  initial_angular_distance_deg
  f4:  trajectory_curvature_mean
  f5:  trajectory_curvature_std
  f6:  overshoot_magnitude_px
  f7:  correction_count
  f8:  final_hold_time_ms
  f9:  angular_velocity_max_deg_per_sec
  f10: angular_velocity_std
  f11: micro_correction_frequency (small deltas < 2px per tick)
  f12: trajectory_straightness_ratio (distance / path_length)
  f13: dx_dy_correlation (Pearson correlation of delta components)
  f14: delta_magnitude_entropy (Shannon entropy of |delta| sequence)
  f15: fire_timing_relative_to_lock_ms

Session-level aggregate features:
  s1:  headshot_ratio
  s2:  hit_ratio
  s3:  reaction_time_cv (coefficient of variation across engagements)
  s4:  engagement_rate (targets engaged / targets visible)
  s5:  k/d_ratio
  s6:  fov_engagement_boundary_sharpness
  s7:  target_selection_optimality_score
  s8:  trajectory_curvature_consistency (inter-engagement variance)

Model architecture options:
- Gradient Boosted Trees (XGBoost/LightGBM):
  Best for tabular feature vectors, interpretable feature importance,
  fast inference on server. Preferred for production deployment.
- Random Forest: simpler, less prone to overfitting on small datasets
- 1D-CNN / LSTM on raw delta sequences:
  Operates on raw (dx, dy, dt) sequences instead of engineered features.
  Can capture patterns human engineers might miss.
  Higher compute cost; suitable for batch/offline analysis.
- Ensemble: combine tree-based (tabular features) + sequence model
  (raw deltas) for highest accuracy

Training data:
- Positive samples: confirmed AI aimbot users (manual review, honeypot,
  or controlled testing with known cheat software)
- Negative samples: legitimate high-skill players (important: include
  top-percentile players to avoid false-positives on skilled play)
- Hard negatives: players with aim-assist controllers (console),
  players using legitimate accessibility tools

Evaluation metrics:
- False Positive Rate (FPR): must be extremely low (< 0.01%)
  for production deployment — banning legitimate players is catastrophic
- True Positive Rate (TPR): secondary to FPR; 70-85% TPR is acceptable
  if FPR is near-zero
- Use session-level aggregation: flag a player only if multiple
  sessions show consistent AI patterns (reduces both FP and FN)

Deployment pipeline:
  Client → input telemetry upload (per tick) → server telemetry DB
  → batch feature extraction (per engagement window)
  → ML inference (per session)
  → risk score aggregation (per player, across sessions)
  → threshold → manual review queue or automated action

Adversarial robustness:
- Cheat developers tune smoothing parameters to evade specific features
- Defense: retrain model periodically on newly confirmed samples
- Use feature combinations rather than single-feature thresholds
- Ensemble across multiple sessions reduces evasion success
- Raw sequence models (CNN/LSTM) are harder to evade than
  hand-crafted feature thresholds because the evasion space
  is higher-dimensional
```

### Hardware Input Device Detection
```
Detecting KMBox and similar hardware input injectors at the
platform/driver level.

USB Enumeration Signals:
- Known VID/PID combinations for KMBox, Arduino Leonardo (2341:8036),
  Teensy (16C0:0486), generic CH340/CP2102 serial adapters
- USB device appearing/disappearing during game session
- Multiple HID mouse devices where only one physical mouse is expected
- USB device with HID mouse capability but no manufacturer string
  or generic "Arduino LLC" / "Teensyduino" manufacturer

USB HID Report Analysis:
- Hardware input devices generate genuine HID reports, but:
  - Report rate: KMBox/Arduino typically report at exactly the
    programmed rate (e.g., every 1ms or 8ms); real mice have
    polling rate jitter tied to USB microframe scheduling
  - Report timing: AI-injected movements arrive in bursts
    (idle → sudden burst of calculated deltas → idle),
    while human movement is continuous with natural pauses
  - Delta distribution: AI deltas cluster around computed values
    with optional noise; human deltas follow characteristic
    distributions per movement speed

Network Traffic Indicators (KMBox Net):
- KMBox Net uses UDP communication on the local network
- Packet pattern: consistent-size UDP packets at high frequency
  from a secondary device to the KMBox's IP
- If cheat PC is on the same network: detectable via
  network monitoring (firewall/router logs)

Driver-Level Detection:
- interception.sys: known driver signature, detectable via
  module enumeration and PiDDBCacheTable
- Logitech G HUB DLL injection: detect unexpected DLL loads
  into GHUB process, or specific exploitable GHUB versions
  via file version checking

Limitations:
- Pure hardware HID injection (KMBox, Arduino) is fundamentally
  indistinguishable from real mouse input at the HID protocol level
- Detection must rely on statistical input analysis rather than
  driver/device-level signatures
- Dual-machine setups with capture cards leave zero footprint
  on the gaming PC beyond the hardware input device
```

## Telemetry Pipeline

### Client-Side Collection
```
- Module list enumeration and hash reporting
- Handle table snapshots for suspicious access patterns
- Stack trace sampling at periodic intervals
- Driver load events and callback registration state
- Hardware fingerprint (disk serial, NIC MAC, SMBIOS, GPU)
```

### Transport & Server-Side
```
- Encrypted telemetry channel (TLS + custom encryption layer)
- Server-side aggregation and anomaly scoring
- ML-based behavioral clustering for ban waves
- Replay system integration for suspicious session review
```
