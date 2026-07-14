# Intel HDA audio

## Controller discovery

`kernel/hda.c` locates an Intel High Definition Audio controller through PCI class codes. The driver maps BAR0, enables bus mastering, resets the controller, and inspects codec presence through `STATESTS`.

## Codec command transport

Codec verbs are sent through CORB and responses are read from RIRB. Ring sizes are selected from the capabilities advertised by the controller. The driver resets ring pointers before enabling DMA and uses bounded waits for command completion.

## Widget graph

The codec is not configured through fixed node IDs. The driver queries function groups and widgets, then identifies:

- audio output converters
- mixer and selector paths
- speaker and headphone pins
- connection lists
- amplifier capabilities
- default pin configuration

This is required because node numbering and routing differ between codecs and firmware revisions.

## Output routing

The driver supports `auto`, `speaker`, and `headphones` modes. Automatic mode prefers a headphone pin when pin sense reports a connected jack, otherwise it uses the speaker path.

Pin control, EAPD, stream/channel assignment, converter format, and amplifier gain are programmed along the chosen path. Gain values are clamped to codec capabilities rather than assuming that the maximum encoded value is safe.

## DMA playback

Audio output uses an HDA stream descriptor and a Buffer Descriptor List. Buffers are statically allocated, aligned, and addressed directly. The stream format is 48 kHz stereo PCM.

Source WAV data is converted into this output format in software. Supported source widths are 8, 16, 24, and 32 bits. The parser also accepts PCM wrapped in WAVEFORMATEXTENSIBLE.

## Streaming

The shell does not load an entire WAV file into memory. A `wav_stream` keeps file metadata and a 64 KiB read-ahead cache. Samples are decoded, channel-converted, and resampled into the HDA ring as space becomes available.

The background player is serviced from keyboard polling. It can continue while the shell or editor waits for input.

## Playback controls

Foreground playback supports:

- Space: pause or resume
- Left / Right: seek by ten seconds
- Up / Down: change volume
- `R`: repeat
- `S`: shuffle
- Escape or Ctrl+X: stop

Background commands manage queue state, volume, repeat, shuffle, next/previous selection, and output routing.

## Real-hardware considerations

- Codec routing must be discovered, not copied from an emulator.
- Maximum amplifier gain may produce clipping or dangerous volume.
- Jack presence detection may require a pin-sense verb after enabling the pin.
- DMA position and completion behavior must be tested on the target controller.
- Background polling must remain frequent enough to prevent underruns.
