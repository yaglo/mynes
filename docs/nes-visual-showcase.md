# Choosing the NES showcase

This selection starts with games praised for their artwork, then selects actual MyNES captures for composition, recognizable imagery and visible signal behaviour. A game's reputation alone does not make its first available frame a good screenshot.

## Research behind the selection

[Den of Geek's graphics-focused list](https://www.denofgeek.com/games/best-looking-nes-games-graphics-ever-all-time/) highlights Batman: Return of the Joker, Kirby's Adventure, Darkwing Duck and Little Samson, and also includes Super Mario Bros. 3 and Contra. Its discussion emphasizes colour, animation, large sprites and detailed environments.

[NES developer nesdoug's visual selections](https://nesdoug.com/2018/06/16/nes-graphics/) independently include Kirby, Little Samson, Batman, Mega Man 2 and Shatterhand. This is an art selection, explicitly not a ranked list. These sources guided the candidates; the images below are our renderer's output, not screenshots copied from those articles.

## Watch

- [Five-game showcase reel — 20 seconds, 60.1 fps](images/showcase/showcase-reel.mp4). Each scene runs for four seconds, with straight cuts and no frame blending. Approximate starts: 0:00 Kirby, 0:04 Little Samson, 0:08 Darkwing Duck, 0:12 Mario 3, 0:16 Mega Man 2.
- Individual four-second videos are linked below.

## The chosen scenes

| Game | Scene and reason | Preset | Native-cadence clip |
|---|---|---|---|
| Kirby's Adventure | Complete title composition and animated stars; the longer clip continues into the colourful attract scene. | JVC D-Series | [60.1 fps](images/showcase/kirby-jvc_d_series_2000.mp4) |
| Little Samson | Mountain-to-palace opening: layered architecture, columns and warm/cool contrast. The GIF concentrates on the palace. | JVC D-Series | [60.1 fps](images/showcase/little-samson-jvc_d_series_2000.mp4) |
| Darkwing Duck | Bridge gameplay: moving and jumping across bright rails against a blue night sky and city lights. | Stas's Favourite | [60.1 fps](images/showcase/darkwing-stass_favourite.mp4) |
| Super Mario Bros. 3 | Recognizable theatrical title with animated characters and curtains. | Toshiba 14AF | [60.1 fps](images/showcase/mario-3-toshiba_14af43.mp4) |
| Mega Man 2 | Complete title and rooftop hero, replacing an earlier capture of mid-intro prose. | Sony PVM-14L2 | [60.1 fps](images/showcase/mega-man-2-sony_pvm_14l2.mp4) |

Batman and Shatterhand were also captured. The sampled plain logos/intro fragments were weaker showcase compositions than the selected scenes, so they are not included simply to fill the page. Zelda's sampled fade and story text were also rejected. This is a selection of scenes, not a claim that the chosen games are objectively the best-looking games.

[Native 4K close-ups and beam-height review](gpu-beam-closeups.md).

## Unaveraged animation

README GIFs retain 24 consecutive frames at 20 ms each: 50 fps, with the same GIF timing compromise documented in the [motion review](gpu-motion-review.md). A fixed palette avoids palette changes creating extra flicker; no temporal blending or optical-flow interpolation is used. Full scenes are reduced from 960×720 to 640×480 in linear light, frame by frame, before GIF palette conversion. Exposure remains 0.7 across every capture. Darkwing’s GIF selects the jump at source frames 8100–8123 from the decoded gameplay video; it therefore also inherits the MP4’s chroma compression. Its full clip starts at frame 8000.

Each linked MP4 contains 240 consecutive frames at 60.0988 fps. Browser/GIF playback is not a verification of live panel cadence. The exact scenes and frame counts are in the [capture manifest](showcase-captures.json).

The following **native-pixel Kirby detail** uses lossless RGB and 16/17 ms WebP frame durations. It makes the alternating phase visible without enlarging the effect or slowing it for emphasis:

![Kirby title: native-pixel, unaveraged phase detail](images/showcase/kirby-phase-detail.webp)

The [all-preset Contra comparison](contra-preset-gallery.md) remains the controlled comparison of display models. This showcase deliberately varies games and presets; it is not a controlled tube-to-tube comparison.
