# OBS ProGrade

Native OBS Studio color-grading filter for Windows x64.

## v0.1 test build
- Four real Qt color wheels: Lift, Gamma, Gain and Offset
- Luminance slider for each wheel
- Saturation
- Dual LUT slots (.cube/.png) with independent mix
- GPU shader processing for primary grading
- LUTs are applied by OBS's native LUT filter in sequence

## Build
GitHub Actions builds the Windows package automatically on push.

After a successful run, download the Windows x64 artifact from the Actions run and copy the plugin contents into the OBS Studio plugin directories as described in the artifact README.
