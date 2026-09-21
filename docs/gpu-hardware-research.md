# Hardware research and model decisions

[Gameplay showcase](nes-visual-showcase.md) · [4K gameplay and beam-height measurements](gpu-beam-closeups.md)

Reviewed 21 September 2026. The curated targets are Sony PVM-14L2, JVC AV-27D201, Toshiba 14AF43, and the generic worn set **Stas's Favourite**. These are nominal behavioural profiles. A manufacturer's bandwidth specification, an owner's photograph and a measurement of a particular tube are different kinds of evidence.

## Monitor identity

| Target | Documented characteristics | Model consequence | Values still estimated |
|---|---|---|---|
| Sony PVM-14L2 | P22 Trinitron; 600 TVL; 0.25 mm grille; 267.5 × 200.6 mm visible area; 10 MHz RGB; D65/D93; composite, Y/C and RGB/component inputs | Aperture grille, approximately 1070 triads, neutral D65, restrained geometry, sharp RGB amplifiers | Beam FWHM, exact phosphor primaries and decay, optics and unit condition |
| JVC AV-27D201 | 27-inch dark-tinted 1.7R tube, two-line digital comb, component and Y/C inputs | Curved consumer raster and two-scanline composite separation; slot mask | Spot growth, regulation, optics; 661 triads uses an owner's 0.83 mm tube-pitch entry, not a manufacturer calibration |
| Toshiba 14AF43 | Flat 14-inch/357 mm tube, three-line comb, no velocity modulation; LA76600M separator on schematic | Nearly flat raster, broad compact-TV beam, three-scanline separation, slot mask | 480 triads is explicitly an estimate; no measured tube pitch or beam response found |
| Stas's Favourite | No commercial model claimed; user's old-TV recovery/breathing reference | Causal horizontal voltage recovery, modest contraction under load, imperfect convergence, inline slot mask over RF | All defect strengths and time constants are generic preference values |

Sources: [Sony specification](https://www.sony.jp/pro-monitor/products/PVM-14L2/), [Sony catalogue](https://www.sony.jp/products/catalog/SPC_PVM-20N6J.PDF), [JVC product archive](https://support.jvc.com/consumer/product.jsp?modelId=MODL020660), [Toshiba service manual](https://consolemods.org/wiki/images/f/f7/Toshiba_14AF43_Service_Manual.pdf). The [JVC owner record](https://crtdatabase.com/crts/jvc/jvc-av-27d201) supplies supplementary tube metadata and photographs; its pitch is treated as provisional.

TVL is horizontal resolving power normalized to picture height. It is neither the number of phosphor triads nor a reason to render 600 horizontal scanlines. These are standard-definition sets; the NES's repeated non-interlaced field occupies the same raster lines. We do not turn them into 480p/HD CRTs.

## Decoder research that changed the implementation

The PVM service manual identifies **MC141627** Y/C separation, **CXA2163AQ** chroma decoding and **CXA1739S** drive/cutoff control. It also describes distinct picture/brightness ABL circuits. The previous notch-only preset omitted the comb. It now selects a generic adaptive line comb; it does not claim the IC's internal algorithm or ABL calibration. [Sony service manual, theory of operation and parts list](https://consolemods.org/wiki/images/f/fc/PVM-L2_Service_Manual.pdf).

Motorola's MC141627 documentation describes chroma-band filtering, line memories, correlation processing, an adaptive vertical enhancer, 8-bit conversion and a 4×subcarrier clock. Its NTSC chroma BPF half-width is nominally 0.75 MHz. We use that width and a correlation-based separation approximation. Exact quantization, clock jitter, vertical enhancement/coring and the proprietary decisions are not reproduced. [Motorola MC141627 datasheet](https://pdf.dzsc.com/27F/MC141627FT_1084067.pdf).

A two-line filter has **one** line delay; a three-line filter uses **two** delays. The old mode named “3line” actually averaged four full scanlines. It now uses the centre line and its two neighbours. More significantly, line averaging now operates on the **chroma band**, then subtracts that result from the original composite. This preserves low-frequency vertical luminance detail. GPU tests exercise single-line gray detail, carrier cancellation, the three-line footprint and Y+C reconstruction.

JVC's two-line topology is documented, but our fixed two-line mode does not reproduce every adaptive choice of its decoder. Sanyo's LA76600M uses a 2H CCD store, rather than implying that all three-line filters are digital. [TC90A45 block diagram in Sony's service documentation](https://audiocircuit.dk/downloads/sony/Sony-SBV55A-avs-sm.pdf), [Sanyo LA76600M datasheet](https://www.alldatasheet.net/datasheet-pdf/pdf/200214/SANYO/LA76600M.html).

The named consumer profiles describe their NTSC versions. Loading a PAL game retains the game's timing and uses horizontal separation plus the PAL chroma delay line. NTSC line-comb modes are disabled in PAL. This is a functional generic PAL fallback, not a claim that the North American JVC or Toshiba accepted PAL or that the Sony's PAL comb has been replicated.

## Audit of the complete path

| Stage | Evidence and implementation decision | Boundary of the model |
|---|---|---|
| PPU DAC | Codes select measured voltage rails and emphasis; the waveform precedes colour decoding | Particular chip measurements, not an interchangeable palette or every PPU revision |
| PPU output impedance | Voltage-dependent RC response adds the published 2C02G phase-distortion estimate; 30 ns at white is editable | An estimated equivalent impedance, not transistor simulation; NTSC composite/RF only |
| Horizontal/vertical raster | Sync, porch, burst, borders and blanking precede reception; native NES timing is retained | Core does not export every border write or exact pulse-length detail |
| Output amplifier | Independent nominal 6 MHz console pole | No evidence that every NES board has exactly this corner |
| Cable | Short terminated-lead R/C equivalent and explicit optional echo | No frequency-dependent transmission-line solver; cable delay and dBm are not calibrated measurements |
| RF | Equivalent negative-AM envelope detector, complex noise from carrier/noise power ratio, bandwidth and sync-based gain | No sampled VHF carrier, VSB/IF asymmetry, tuning or intercarrier sound |
| AGC/clamp/sync | Sync-based amplitude control, detected porch black and sync timing | Generic loop constants; no free-running vertical oscillator or rolling under loss of sync |
| Burst/PLL | Measured phase/amplitude, holdover, colour kill and reacquisition | No chip-specific PLL loop filter or oscillator phase-noise spectrum |
| Y/C separation | Chroma band followed by notch, two-line, adaptive or three-line separation | Generic transfer functions; commercial decoder decisions remain approximations |
| Chroma detection | Quadrature demodulation, bandwidth filtering, PAL delay correction | Equal-band axes are equivalent to rotated colour-difference axes; optional unequal bandwidth is a separate legacy approximation |
| Matrix/white balance | Colour-difference decoder gains; daylight white points; gun drive/cutoff; nominal 525 phosphor primaries in linear light | Consumer values and spectra are estimates; P22 is a family, not one gamut |
| Gun amplifiers | Separate per-gun bandwidth; voltage-to-light precedes spatial spreading | Power-law gun transfer and Gaussian spot; exact saturation and amplifier poles are not measured |
| DC recovery/video rail | Causal horizontal bias/gain recovery after the amplifiers | Generic fault model, not a diagnosis of the user's old TV |
| EHT/deflection/focus | Shared line-current state, local load, signed size response and focus growth | No circuit-level EHT/deflection regulation or calibrated ABL knee |
| Beam/landing | Pixel-integrated spots, separate gun-current width, convergence and edge focus | No measured asymmetric/non-Gaussian tube spot or complete electron optics |
| Phosphors | Per-channel recursive decay in linear light; actual elapsed frames | Frame-sampled exponential approximation; no measured multi-component afterglow |
| Face/mask | Distinct aperture/slot/dot structures; coverage normalized for neutral mean light | Exact dimensions are only documented for selected targets; no inferred LCD subpixel layout |
| Glass/room | Energy redistribution, screen-relative optical radius, modest tint/reflection | Generic scatter PSF and ambient term, not a measured glass stack/room |
| Host display | Native drawable geometry, integer panel periods or filtered physical pitch; linear HDR and final output shoulder | Host gamut, luminance, persistence and compositor limit reproduction; no absolute-nit calibration |
| Audio | APU → console filter → cable → speaker, shared CPU/GPU state and bounded queue | Generic cabinet/speaker transfer, no microphone measurement or full RF sound demodulation |

## Evidence for cross-stage decisions

- **NES timing and phase:** the measured voltage-waveform approach and brightness-dependent impedance are described in the [NESdev NTSC reference](https://www.nesdev.org/wiki/NTSC_video). The nonlinear output stage is applied to the full raster, including burst; a global hue offset could not substitute for it. The regression checks approximately 14° more phase lag between the lowest and highest coloured voltage rows.
- **Transmission and DC restoration:** nominal 75 Ω connections require termination; AC coupling can change the baseline and produce droop. [Analog Devices video interfaces](https://www.analog.com/en/resources/technical-articles/switching-video-using-analog-switches.html), [clamping and AC coupling](https://www.analog.com/en/resources/technical-articles/get-a-grip-on-clamps-bias-and-accoupled-video-signals.html). We removed the former unsupported claim that the unused coupling-capacitor control was a sub-Hz effect.
- **Broadcast versus NES:** receiver assumptions come from television timing, while the source keeps NES timing. Consequently the NTSC fixed 1H store is 2730 samples against a 2728-sample NES line. [ITU-R BT.470](https://www.itu.int/rec/R-REC-BT.470/en).
- **Beam and current:** measured beam profiles vary in both their central region and tails. [Hitachi beam-profile measurement](https://www.fujipress.jp/jrm/rb/robot000700030238/). Conserving integrated energy is necessary, but does not prove that our Gaussian shape matches a particular tube.
- **Image-dependent CRT output:** measured CRT luminance can depend on pattern orientation, DC restoration and supply regulation. [García-Pérez and Peli](https://pelilab.partners.org/papers/monitor/artifacts_cathode.pdf). Thus streaks and loading are modeled upstream of emitted light, not as decorative overlays.
- **Persistence:** phosphor decay need not remain a single exponential at low levels; an LCD's hold interval is a separate limitation. [Display-timing research](https://www.sciencedirect.com/science/article/abs/pii/S0165027010003420). Two-frame still exposure is a review method, not a simulation of CRT motion persistence.
- **Host pixels:** backing pixels and native panel pixels are separate API quantities. We query both and provide native fullscreen. [Apple backing scale](https://developer.apple.com/documentation/appkit/nswindow/backingscalefactor), [SDL high-DPI guidance](https://wiki.libsdl.org/SDL3/README-highdpi). This is not an optical measurement of the panel or proof of its subpixel order.

## Photograph review

The supplied Contra Waterfall boss photograph and palette-only screenshot remain the primary visual scene reference. A local ROM was run to the same boss and its **actual PPU codes** were captured; the project does not reconstruct the scene from the photograph. Projectile/player states differ. Comparison checks facial ridges, teeth, bright platform, black mouth, colour bleed, spot growth and mask structure. Camera exposure, white balance, focus and resampling prevent deriving an absolute colour or luminance calibration from those photographs.

We also inspected owner photographs of [PVM-14L2](https://crtdatabase.com/crts/sony/sony-pvm-14l2), [PVM-20M4U](https://crtdatabase.com/crts/sony/sony-pvm-20m4u), [JVC AV-27D201](https://crtdatabase.com/crts/jvc/jvc-av-27d201) and [Toshiba 14AF43](https://crtdatabase.com/crts/toshiba/toshiba-14af43). A [BVM owner macro with capture settings](https://forums.libretro.com/t/calling-all-crt-owners-photos-please/36593?page=4) helped inspect grille grouping and spot shape; its tube dimensions were not transferred to the 14L2. Search results that depicted other shaders were excluded as hardware evidence.

The [visual review](gpu-visual-review.md) contains our own paired renders and their limitations. Third-party photos are linked rather than redistributed in the repository.

## Latest comparison decisions

The four-profile colour defaults, RF assumptions and response ranges are listed in the [preset audit](gpu-preset-audit.md). Sony's white balance is set to its documented D65 option. Consumer whites are cooler, with separate colour-difference gain and small tracking errors. These are nominal choices, not an attempt to reproduce camera white balance from the Contra photograph.

The slot-mask model keeps vertical phosphor stripes and staggers only the bridges between adjacent triads, consistent with the [inline slit-mask construction](https://patents.google.com/patent/US3973965A/en). An ablation on the same Contra codes showed that Stas's previous coarse delta-dot pattern generated the dominant diagonal weave. Its replacement retains visible RGB separation with a less intrusive inline pattern. Composite dot crawl can still produce phase-dependent diagonals; it is distinct from random RF noise.

Additional direct owner photographs include [JVC Mario and Adventure Island closeups](https://sector.sunthar.com/guides/crt-rgb-mod/jvc-av-27d201.html). These show an RGB-modified set and inform spot/mask structure, not composite decoder calibration. Analog Devices describes [differential gain and phase](https://www.analog.com/en/resources/technical-articles/2022/07/21/08/24/visual-impact-of-video-parameters-in-video-systems.html); the current model includes the measured source's level-dependent phase estimate but not chip-specific receiver differential gain/phase curves or any unsupported universal “chroma latching” effect.
