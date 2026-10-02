# Native AudioWorklet

Gea compiles an AudioWorklet module into a registered C++ entry point. Each
AudioContext owns a dedicated native task and compiler runtime realm. Module
initialization, processor construction, `process`, message callbacks, and
processor/factory destruction all run in that realm. On dual-core ESP targets,
the worklet is explicitly pinned to core 1, separate from AEC/capture on core 0.
This prevents ESP32-S3 first-FPU-use affinity from choosing a core implicitly. No JavaScript interpreter
or application network service is part of the audio host.

The supported graph is a MediaStream audio source connected to an
AudioWorkletNode connected to its context destination. Nodes accept zero or one
mono input and one mono output. `process` receives reusable 128-frame Float32
channels and an empty parameters object. AudioParam descriptors/automation,
arbitrary graphs, multichannel mixing, and `processorOptions` are not currently
implemented. Unsupported input/output channel counts reject construction.

AudioContext accepts integer sample rates from 8–48 kHz. The host converts
between that rate and the existing 16 kHz microphone/speaker paths; existing
AEC and full-duplex handling remain below the worklet. `addModule` accepts a URL
registered by the compiled module build, rather than fetching/evaluating source
at runtime. `resume`, `suspend`, `close`, and `addModule` expose Promises through
the compiler bridge. Closing is idempotent and releases processors and factories
before the owning runtime realm is cleared.

MessagePort transfers use the worker runtime's bounded native queues. A port
transferred from a network worker to a processor communicates directly between
those tasks. Audio processing never waits on a network request or the UI event
pump. Control operations such as graph construction synchronize with the audio
task; message delivery is asynchronous. Errors thrown by `process` silence that
node and deliver `onprocessorerror` on the node creator's task.

The software output queue is bounded to 2048 samples at the 16 kHz hardware
rate. The producer reserves space before each process call and yields when the
queue is full, preserving capture and playback during codec startup or mixer
delays. Catch-up is bounded by both call count and one quantum of wall time.
Capture batches and partial converted quanta are retained. Missed render
deadlines catch up in bounded groups; only an actual capture-ring overwrite
loses samples, and that loss is reported explicitly.
`baseLatency` reports the maximum software buffering delay: queue capacity plus
the platform mixer chunk. `outputLatency` reports the driver's buffering
estimate, or zero when unavailable. ES8311 derives it from its configured DMA
frame and descriptor counts. With the default 512-frame mixer and 6 × 240-frame
DMA ring these estimates are 160 ms and 90 ms. They are conservative bounds;
`currentTime` is the render clock, not a measurement of sound at the DAC.

`flushAudioWorkletOutput()` is an explicit Gea hardware cancellation extension,
not a Web Audio API. Call it from a worklet message handler after clearing the
processor's playback ring. It clears the context's converted PCM/resampler
history and requests cancellation on the existing output consumer. That task
discards ES8311 TX DMA descriptors before its next pull, using the public IDF
preload API; any in-flight write finishes before this reset. The worklet never
waits for speaker locks, so microphone processing continues. Continuous source
callbacks remain attached; RX, microphone readers, codecs and AEC are retained.
There is no interception of application JSON commands in the MessagePort host.

Run `bash packages/core/test/run-audio-worklet.sh` from the core repository.
The test uses the sibling compiler runtime headers by default; set
`GEA_COMPILER_RUNTIME` to an installed runtime include directory when needed.
It checks task/realm ownership, reusable typed channels, PCM conversion, direct
worker-port messages without pumping the UI, suspended clocks, exception
routing, callback ownership, and teardown under ASan/UBSan.

ESP builds emit one `gea_worklet` statistics line approximately every two seconds.
It includes render quantum count; maximum native render/process time and deadline
lag; actual hardware capture samples read, pending peak and overwritten samples;
and queued/peak output samples and empty hardware pulls. Sample counts on that
line use the hardware 16 kHz rate, while quanta use the context sample rate.
A 24 kHz context normally renders approximately 375 quanta per two seconds.
These measurements are emitted from native audio code without UI callbacks.
