# ProGrade v0.2 roadmap

## Priority 1 — realtime interaction
- Stop calling obs_source_update on every mouse-move.
- Update live grading values directly in the filter state.
- Persist settings only on mouse release / editing finished / panel close.
- Cache the wheel bitmap instead of regenerating it on every paint.

## Color page UI
- DaVinci-style Lift / Gamma / Gain / Offset wheels.
- Numeric RGB values and luminance under each wheel.
- Reset per wheel.
- Integrated preview panel where technically stable.
- Optional dock mode later so the panel can remain inside the OBS main window.

## CST / camera transform
- Dedicated first-stage technical transform slot.
- Presets for camera/log families (Sony, Canon, Panasonic, Blackmagic, DJI, etc.) implemented as managed technical LUTs to Rec.709.
- Keep creative LUT 1 and LUT 2 separate after CST.

## Scopes
- Waveform RGB / luma first.
- Vectorscope second.
- Scope refresh can be lower than the grading path (e.g. 10–15 fps) while grading remains frame-realtime.

## Bloom
- Highlight extraction in linear light.
- Multi-pass Gaussian blur.
- Screen/lighten-style compositing back over the image.
- Threshold, radius, intensity and soft-knee controls.

## Halation
- Highlight-edge extraction.
- Red-channel dominant blur / spread.
- Linear/additive composite with intensity, radius and threshold controls.

## Processing order
CST -> Primary wheels -> Saturation -> Bloom -> Halation -> Creative LUT 1 -> Creative LUT 2
