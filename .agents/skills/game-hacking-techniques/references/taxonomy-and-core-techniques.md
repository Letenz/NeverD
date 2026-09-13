# Taxonomy And Core Techniques

## Escalation Model

### User-Mode
- Read and write process memory
- Inject DLLs or shellcode
- Hook graphics or input APIs

### Kernel-Mode
- Use signed or vulnerable drivers for direct memory access
- Bypass handle-based protections and inspect protected processes
- Interact with callbacks, page tables, or kernel objects directly

### Below the OS
- Virtualize the system with a hypervisor
- Read memory through PCIe DMA hardware
- Move logic to external devices or secondary machines

## Core Concepts

### Memory Manipulation
- Read Process Memory (RPM)
- Write Process Memory (WPM)
- Pattern scanning
- Pointer chains
- Structure reconstruction

### Process Injection
- DLL injection methods
- Manual mapping
- Shellcode injection
- Thread hijacking
- APC injection

### Hooking Techniques
- Inline hooking (detours)
- IAT/EAT hooking
- VTable hooking
- Hardware breakpoint hooks
- Syscall hooking

## Cheat Categories

### Visual Cheats (ESP)
```
- World-to-Screen transformation
- Player/entity rendering
- Box ESP, skeleton ESP
- Item highlighting
- Radar/minimap hacks
```

### Aim Assistance
```
- Aimbot algorithms (memory-based and AI visual)
- Triggerbot (auto-fire on crosshair detection)
- No recoil/no spread
- Bullet prediction and lead calculation
- Silent aim (server-side angle manipulation)
- AI visual aimbot (YOLO-based, no memory access required)
```

### AI Visual Cheats (Computer Vision Aimbot)
```
Architecture overview:
"Zero memory, zero driver injection" paradigm — uses screen capture +
AI object detection + hardware input injection. No process attachment,
no kernel driver, no game memory reading.

Typical setup:
┌─────────────────┐     screen capture      ┌──────────────────┐
│  Gaming PC      │ ───────────────────────▶ │  AI Pipeline     │
│  Game + OBS     │                          │  (same PC, or    │
│                 │ ◀─────────────────────── │   second PC)     │
└─────────────────┘     hardware input       │  YOLO model      │
                        (KMBox / Logitech)   │  TensorRT/CUDA   │
                                             └──────────────────┘

Dual-machine variant (maximum isolation):
- Machine A (game): only runs game + OBS, sends frames via NDI/capture card
- Machine B (cheat): runs AI model, sends mouse commands via USB/network
  to hardware input device on Machine A
- Game machine has zero cheat code/process

Single-machine variant:
- OBS + AI model run on the same PC
- AI implemented as OBS filter plugin (looks like "OBS is running")
- Mouse output via hardware device or driver-level injection

Pipeline stages:

1. Frame Capture:
   - OBS Game Capture (injects graphics hook DLL into game process)
   - OBS Window Capture (no injection, uses DXGI Desktop Duplication)
   - OBS plugin filter form (AI as OBS filter, minimal footprint)
   - Direct framebuffer copy from GPU output layer (60+ FPS)
   - Capture card (for dual-machine: HDMI/DP input on cheat PC)

2. AI Object Detection:
   - Model: YOLOv5 / YOLOv8 / YOLOv10 / YOLO11 (lightweight variants)
   - Training: fine-tuned on game-specific screenshots
     (enemy bodies, heads, torsos as labeled bounding boxes)
   - Input: cropped region around crosshair (320x320 or 640x640)
     to reduce inference cost
   - Output: bounding boxes with class (head/body/enemy) + confidence score
   - Acceleration: TensorRT (NVIDIA), CUDA, DirectML, OpenVINO
   - Target latency: < 20–30 ms per frame for competitive play

3. Coordinate Transform and Aiming Logic:
   - Convert pixel coordinates to mouse movement delta:
     delta_x = (target_x - screen_center_x) * sensitivity
     delta_y = (target_y - screen_center_y) * sensitivity
   - Target selection: closest to crosshair, highest confidence,
     head priority, or combined scoring
   - FOV (Field of View) lock: only engage targets within
     configurable pixel radius from crosshair center

4. Human-like Trajectory Smoothing:
   - Not instant snap — gradual movement with acceleration curve
   - Micro-jitter injection (simulates hand tremor)
   - Bézier curve or cubic interpolation for path
   - End-point correction (overshoot then settle)
   - Random engagement probability (e.g., 85-90% lock rate)
   - Slight intentional offset (not pixel-perfect center-mass)
   - Variable reaction delay (50-200 ms simulated human response)

5. Mouse Movement Execution:
   - Hardware input devices (see Input Simulation section below)
   - Movement commands sent as physical HID reports
   - Game sees genuine hardware mouse input, not API calls

Why OBS specifically:
- Legitimate streaming software, used by millions of streamers
- Anti-cheat cannot ban OBS-related processes without collateral damage
- Game Capture provides fast, low-latency frame access
- Plugin system allows embedding AI as a filter (invisible to AC)
- Supports D3D11, D3D12, Vulkan, OpenGL capture paths
```

### YOLO Model Training Pipeline (for Game AI Aimbot)
```
End-to-end workflow from raw game screenshots to deployed TensorRT model.

1. Data Collection:
   - Capture game screenshots during actual gameplay (OBS recording or replay)
   - Capture diverse scenarios: different maps, lighting, character skins,
     distances, poses, partial occlusion, smoke/flash effects
   - Aim for 2,000-10,000+ labeled images for robust detection
   - Include negative samples (empty scenes, friendlies, environment objects)

2. Annotation / Labeling:
   - Tools: LabelImg (YOLO format), CVAT (collaborative), Roboflow (cloud),
     Label Studio, makesense.ai (browser-based)
   - YOLO format: one .txt per image, each line:
     <class_id> <center_x> <center_y> <width> <height>
     (all values normalized to 0-1 relative to image dimensions)
   - Class definitions (typical):
     0: enemy_body (full body bounding box)
     1: enemy_head (head-only bounding box, for headshot targeting)
     2: friendly (to avoid shooting teammates)
   - Label head separately from body for head-priority targeting
   - Quality control: consistent label boundaries, no missed instances

3. Data Augmentation:
   - Built-in Ultralytics augmentations (mosaic, mixup, copy-paste)
   - Game-specific augmentations:
     - Brightness/contrast variation (simulate different map lighting)
     - Random crop around crosshair area (match inference ROI)
     - Motion blur (simulate fast movement)
     - Noise injection (simulate compression artifacts)
   - Avoid augmentations that distort aspect ratio
     (characters would look unnatural, hurting accuracy)

4. Training:
   - Framework: Ultralytics YOLOv8/v10/v11/YOLO11
   - Base model: yolov8n.pt or yolov8s.pt (nano/small for speed)
     or yolo11n.pt for latest architecture
   - Training command:
     yolo detect train data=game_dataset.yaml model=yolov8n.pt
       epochs=100 imgsz=640 batch=16 device=0
   - dataset.yaml structure:
     path: /path/to/dataset
     train: images/train
     val: images/val
     names: {0: enemy_body, 1: enemy_head, 2: friendly}
   - Key hyperparameters to tune:
     - imgsz: 320 (fastest) or 640 (more accurate)
     - lr0: initial learning rate (default 0.01)
     - conf: confidence threshold for inference (typically 0.4-0.6)
     - iou: IoU threshold for NMS (typically 0.45-0.7)
   - Training time: 1-4 hours on RTX 3060+ for nano model

5. Validation and Testing:
   - Evaluate mAP@0.5 and mAP@0.5:0.95 on validation set
   - Target: mAP@0.5 > 0.85 for reliable game detection
   - Test inference speed on target hardware
   - Visual inspection on held-out game screenshots

6. Export to TensorRT (deployment):
   - Step 1: Export to ONNX
     yolo export model=best.pt format=onnx simplify=True opset=17
   - Step 2: Convert ONNX to TensorRT engine
     yolo export model=best.pt format=engine half=True device=0
     (half=True enables FP16 precision)
   - Or use trtexec directly:
     trtexec --onnx=best.onnx --saveEngine=best.engine
       --fp16 --workspace=4096
   - FP16 performance: ~17 ms latency, ~57 FPS throughput,
     ~0.9% mAP drop vs FP32 (acceptable trade-off)
   - INT8 quantization: even faster but requires calibration dataset
     and careful accuracy validation

7. Runtime Integration:
   - Load TensorRT engine in C++/Python inference loop
   - Input: preprocessed frame (resize, normalize, HWC→CHW, float32/16)
   - Output: [N, 6] tensor (x1, y1, x2, y2, confidence, class_id)
   - Apply NMS (Non-Maximum Suppression) to deduplicate detections
   - Select target based on: closest to crosshair + highest confidence
   - Convert pixel coordinates to mouse delta

Alternative acceleration backends:
- DirectML (AMD GPUs, Windows native)
- OpenVINO (Intel GPUs/CPUs)
- ONNX Runtime with CUDA EP (cross-platform)
- CoreML (macOS, less common for game cheats)
```

### Movement Cheats
```
- Speed hacks
- Fly hacks
- No clip
- Teleportation
- Bunny hop automation
```

### Miscellaneous
```
- Wallhacks
- Skin changers
- Unlock all
- Economy manipulation
```
