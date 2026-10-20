# S3SS patch version

`limb_ik_scale_fix_patch.cpp` is the same fix as `LimbIkScaleFix.asi`, written as a patch for
[Sims 3 Settings Setter](https://github.com/sims3fiend/Sims3SettingsSetter) (S3SS), for anyone who'd rather
have it there than as a separate ASI.

Status: not yet built inside S3SS. The redirect it applies is the one the standalone ASI applies, which is
tested in game on EA App 1.69.47. It knows the Steam 1.67.2 (`0x0057E0E9`) and EA App 1.69.47 (`0x0057D709`)
addresses and falls back to a pattern scan on other builds.

To try it: copy the file into S3SS's `patches\` folder, add it to `Sims3SettingsSetter.vcxproj`
(`<ClCompile Include="patches\limb_ik_scale_fix_patch.cpp" />`), build, and enable **Limb IK Scale Fix**
under Animation. Don't run it together with `LimbIkScaleFix.asi`; whichever loads second will see the call
already redirected and refuse to patch.

Like the rest of this repository, it was written with Claude, Anthropic's AI model.
