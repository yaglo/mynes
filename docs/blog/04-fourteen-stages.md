# 14 Stages from DAC to Glass

*Modeling a CRT television as a GPU compute pipeline*

---

Most CRT shaders are a single fragment shader. They darken every other row, warp the image with barrel distortion, maybe overlay a phosphor mask texture. The result looks vaguely CRT-like, in the same way a guitar amp skin on a music app looks vaguely like a Marshall stack. It is immediately recognizable as a filter applied to a clean digital image.

The reason is architectural. A real CRT television has 14 stages of analog electronics between the video input and the phosphor glow. Each stage introduces characteristic artifacts. Those artifacts interact with each other in ways that a post-process filter cannot reproduce, because the interactions depend on the signal -- the actual composite waveform -- which a fragment shader never had.

Consider chroma bleed. The comb filter imperfectly separates luma from chroma. The residual cross-color enters the chroma demodulator, which phase-shifts it according to the subcarrier angle at that sample position. The matrix decode then maps those shifted values to specific wrong colors that depend on the original palette index. You cannot paint this on after the fact. The wrong colors are a consequence of the math, not a lookup table.

## The pipeline

| Stage | Name | Domain | What it models |
|-------|------|--------|----------------|
| 1 | 2C02 DAC | Signal | Palette index to composite waveform |
| 2 | Console output | Signal | Coupling cap, amplifier bandwidth, PSU hum |
| 3 | Cable | Signal | RC transmission line, ghosting |
| 4 | RF modulator | Signal | RF channel simulation (RF path only) |
| 5 | TV input | Signal | Coupling cap, automatic gain control |
| 6 | Comb filter | Signal | Y/C separation |
| 7 | Chroma demodulator | Signal | I/Q recovery via quadrature multiplication |
| 8 | Luma processing | Signal | FIR bandwidth limiting |
| 9 | Matrix decode | Signal | YIQ to RGB conversion |
| | | | *--- signal domain ends ---* |
| 10 | Video amplifier | Display | Per-gun bandwidth, gamma |
| 11 | Electron beam | Display | Bloom, convergence, noise, geometry |
| 12 | Phosphor screen | Display | Temporal persistence, dot crawl cancellation |
| 13 | CRT glass | Display | Halation, barrel distortion, tint |
| 14 | Environment | Display | Vignette, ambient light, tone mapping |

Each stage is a separate GPU compute shader. The signal domain operates on 491,520 float samples (2048 samples per scanline, 240 scanlines). The display domain converts that into the output resolution with CRT physics.

## Connection types are not a quality slider

Different cables physically bypass different stages. This is not an approximation or a simplification. It is what actually happens when you change the cable.

| Connection | Active stages | Why |
|------------|--------------|-----|
| RF | All 14 | Full signal path through RF modulator and TV tuner |
| Composite | 1-3, 5-14 | Skips RF mod/demod (no carrier) |
| S-Video | 1-3, 5 (bypass comb), 7-14 | Y/C pre-separated by cable |
| Component | 1-3, 5, 8-14 | Baseband Cb/Cr, no chroma modulation |
| RGB | 1-3, 10-14 | No color space conversion needed |
| Direct | 1-2, 10-14 | No cable, shortest path |

An S-Video cable carries luma and chroma on separate wires. There is no composite signal to comb-filter because the signals were never combined. The comb filter stage is not "skipped for better quality." It is physically absent from the signal path. You cannot get composite artifacts from an S-Video connection any more than you can get wet from disconnected plumbing.

The implementation is a single function call:

```c
bool video_chain_stage_active(const VideoChain *chain, int stage) {
    VideoConnectionType c = chain->connection;
    switch (stage) {
    case 4:  return c == VIDEO_CONN_RF;
    case 6:  return c == VIDEO_CONN_RF || c == VIDEO_CONN_COMPOSITE
                  || c == VIDEO_CONN_SVIDEO;
    case 7:  return c == VIDEO_CONN_RF || c == VIDEO_CONN_COMPOSITE
                  || c == VIDEO_CONN_SVIDEO;
    case 9:  return c != VIDEO_CONN_RGB && c != VIDEO_CONN_DIRECT;
    // ...
    }
}
```

Hot-switching from RF to S-Video is toggling `enabled` flags on the stage array. The runner does not change. The shaders do not change. The signal path reconfigures itself.

## The signal chain runner

All dispatch boilerplate is handled by a generic, data-driven runner. The same runner handles both video and audio chains.

```c
typedef struct {
    const char     *name;           // human-readable (for visualiser)
    ChainKernelType kernel_type;    // which compute shader
    bool            enabled;        // false = skip
    bool            bypass;         // user-toggled (visualiser B key)
    uint8_t         params[128];    // uniform data
    uint32_t        params_size;
    uint32_t        dispatch_x, dispatch_y, dispatch_z;
    double          timing_us;      // wall-clock dispatch time
    bool            needs_carry;    // RC filter: inter-block state
    bool            needs_taps;     // FIR: tap coefficient buffer
    bool            dual_output;    // modulator IQ: writes 2 buffers
} ChainStage;
```

Adding a stage means appending a struct. No C code changes. The runner iterates the array, dispatches each enabled stage's kernel, and manages buffer routing. It does not know or care what any stage does.

## Ping-pong buffers

Two GPU buffers alternate as input and output. Stage N reads from buffer A and writes to buffer B. Stage N+1 reads from buffer B and writes to buffer A. The runner tracks which buffer holds the current data.

Some kernels operate in-place. The RC filter processes each scanline sequentially -- it reads and writes the same buffer because the IIR feedback requires the previous output sample. The runner knows not to swap buffers for these stages.

Some kernels produce dual output. The comb filter writes Y to one buffer and C to another. The modulator in IQ mode writes I and Q simultaneously. Auxiliary buffers handle this -- up to 4 extra buffers beyond the ping-pong pair, routed automatically by flags on the `ChainStage` struct: `dual_output` for the modulator, `reads_secondary` for stages that consume the second output.

The same runner processes audio through 10 stages (coupling cap, feedback network, amplifier saturation, PSU hum, cable capacitance, speaker model, decimation). Different buffer sizes -- audio is ~29,829 float samples per frame versus video's 491,520 -- but the dispatch machinery is identical.

## Emergent artifacts

The key insight: none of the characteristic CRT artifacts are special-cased.

Dot crawl exists because the 3.579545 MHz subcarrier phase advances between frames. The DAC shader (Stage 1) encodes this phase. The comb filter (Stage 6) fails to fully cancel it. The residual shows up as a crawling rainbow pattern. No code says "add dot crawl here."

Chroma bleed exists because the FIR bandwidth in Stage 7 is set to 1 MHz for a composite connection. At that bandwidth, low-frequency I/Q components spread 4-5 pixels horizontally. A PVM with 1.5 MHz chroma bandwidth has less bleed. The difference is one float in a uniform buffer.

Rainbow shimmer on horizontal stripes exists because the 1-line comb filter averages two scanlines. When vertical detail changes rapidly, the luma estimate is wrong, and the error leaks into the chroma channel. A 3-line comb averages four scanlines and largely eliminates this -- but introduces slight vertical softening. The tradeoff is real. It is the same tradeoff TV engineers made in the 1980s.

Convergence fringing exists because the red and blue electron beams are offset from green. The beam shader (Stage 11) reads RGB values from shifted sample positions. The offset is worst at screen edges, scaled by `edge_factor = cx^2 + cy^2`. On a well-calibrated PVM, the offsets are near zero. On the Basement TV preset: `conv_r_x = 6.0, conv_b_x = -5.0`. Every edge has visible red-blue fringing.

These are not effects. They are consequences of the signal processing. The pipeline does not simulate a CRT. It simulates the electronics that drive one, and the CRT behavior follows.
