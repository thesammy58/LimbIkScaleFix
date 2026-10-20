# Sims 3 limb IK scale flicker: technical write-up

> Written by Claude, Anthropic's AI model, from its own analysis of the game; see the README's "How this was made". Addresses are from the Steam build (1.67.2) unless marked EA.

Hands and feet on sims with size sliders flicker between two sizes because the limb IK solver reads a scaled bone matrix as if it were a pure rotation, then writes a rotation of the wrong length back into the pose. Smooth Animation's blend repairs that rotation on blended frames only, so blended and unblended frames show different sizes. Redirecting one function call inside the solver fixes it, and the fix is confirmed working in game on the EA App build (1.69.47).

![How a scaled hand or foot reaches the screen](how-it-works.png)

The first three steps happen once per sim snapshot; the split happens per rendered frame. The fix removes the error at step 2, so both kinds of frame show the same size.

## What goes wrong

The bug needs three things at once: a scaled hand or foot, limb IK running, and frames that alternate between blended and unblended.

1. **Sliders scale the bone in the pose itself.** When a clip is evaluated, each bone delta (BOND) is added straight into the pose's per-bone local transform: offset into translation, rotation added and renormalised, and `local.scale += delta.scale` (`0x0054E780`). Sage's hand slider at max leaves `b__L_Hand__` with scale 1.2; NikSim's feet slider leaves `b__L_Foot__` at (1.25, 1, 1.25).
2. **The IK solver reads that scaled bone as a pure rotation.** The limb IK chain solver (`0x0057D670`) finishes by turning the end bone (hand or foot) to the IK target's orientation. To find where the bone points now, it builds the bone's model-space matrix with `0x00577160` (called at `0x0057E0E9`), which multiplies each rotation row by the bone's accumulated scale. It then converts that matrix to a quaternion with the standard trace / largest-diagonal method (`0x0057E106`–`0x0057E386`), which is only valid when there is no scale. With scale, the quaternion comes out the wrong length and pointing slightly the wrong way.
3. **The bad rotation is written into the pose.** The correction (target × conjugate of the current rotation) is multiplied into the end bone's local rotation and stored without normalising (`0x0057E4C5`). Turning a quaternion of length m into a matrix gives (1 − m²)·I + m²·R instead of R, which resizes and shears the hand or foot and everything parented to it.
4. **Smooth Animation repairs it on some frames only.** The two-pose blend (`0x0054D7D0`) nlerps and renormalises every bone rotation, which cancels the error. Frames that skip the blend render the raw IK pose: the interpolation clock (`0x00693660`) returns −1 when rendering has run past the newest simulation snapshot, and `0x009420E0` then draws that snapshot directly. A mix of the two kinds of frame produces the two alternating sizes.

Turning off either setting hides it. With RenderLimbIk off there is no bad rotation. With Smooth Animation off every frame skips the blend, so the size is wrong but stable.

## How big the error is

Feet get the worst of it: a 25% foot slider becomes about 56% on unblended frames. The rig's foot sits about 177° from the calf, which is close to the worst case for this error. Hands depend on how far the forearm is twisted, from nothing at neutral to about 22% at 90°.

| Case | Intended size | Unblended frame (largest change) | Difference |
| --- | --- | --- | --- |
| No slider (hand or foot) | 1.00 | 1.00 | 0% |
| NikSim feet, max (1.25, 1, 1.25) | 1.25 | 1.56 | +25% |
| NikSim feet, min (0.75, 1, 0.75) | 0.75 | 0.56 | −25% |
| Sage hands, max 1.2, forearm twist 0° | 1.20 | 1.20 | 0% |
| Sage hands, max 1.2, forearm twist 30° | 1.20 | 1.24 | +3% |
| Sage hands, max 1.2, forearm twist 60° | 1.20 | 1.34 | +12% |
| Sage hands, max 1.2, forearm twist 90° | 1.20 | 1.46 | +22% |
| Sage hands, min 0.8, forearm twist 90° | 0.80 | 0.64 | −20% |

Figures come from re-running the engine's own formulas in Python on the adult rig's rest pose (`auRig`), with the IK target equal to the bone's current orientation. Blended frames show the intended size in every case.

## Seen in a live game

A memory dump of the EA App build, taken while a sim's hands were flickering, shows the bad rotations directly. In that sim's pose, both hand bones carry slider scale and both rotations are too long; every other bone's rotation is exactly unit length.

| Bone | Scale (x, y, z) | Rotation length | Wrist angle | Size error on unblended frames |
| --- | --- | --- | --- | --- |
| Hand, first | 1.130, 1.158, 1.125 | 1.081 | 73° | about +13% |
| Hand, second | 1.130, 1.158, 1.125 | 1.115 | 83° | about +24% |
| Every other bone | varies | 1.000 | varies | none |

The size errors are computed from the rotation length and wrist angle with the formula in step 3 above. With the fix installed, the flicker is gone.

## Which sliders trigger it

What matters is the hand or foot's combined scale: its own scale times every parent bone's. Any slider that leaves that at 1 is safe, however much it scales other bones.

| Slider | Bones it scales (from its BOND) | Triggers it | Why |
| --- | --- | --- | --- |
| Hand Size (Sage) | `b__L/R_Hand__`, ±0.2 uniform | Yes | Scales the hand, the last bone of the arm IK chain |
| NikSim Feet | `b__L/R_Foot__` (x, z ±0.25) and toes | Yes | Scales the foot, the last bone of the leg IK chain |
| leWing Feet: Foot Length/Height | Foot (y, z about ±0.18) | Yes | Scales the foot |
| leWing Feet: Foot Height/Width | Foot (x, y about ±0.19) | Yes, small at rest | Its x and y changes nearly cancel at the rest pose; the error grows as the foot rotates |
| Pcfreak Arm 1.55 | Upper arm, forearm and hand | Yes | Leaves the hand's combined scale off 1 |
| leWing Hands: hand size | Finger and thumb bones only | No | The hand bone itself stays at 1 |
| leWing Feet: toe sliders | Toes only | No | Toes sit below the foot in the chain |
| Jonha Calf Size / Thigh Size | Calf or thigh, plus the inverse on the foot | No | The foot's combined scale stays 1 (1.1 × 0.909) |
| Bloom Foot Width | Moves the `footOffset` / `footWorld` helper bones | No | Translation only, no scale |

The "No" rows are predictions from the bone data and haven't been checked in game. Jonha's sliders show the slider-side workaround: scale the foot by the inverse so its combined scale stays 1. That only works when the hand or foot isn't meant to change size.

## The fix

The fix redirects the one matrix build at `0x0057E0E9` (EA: `0x0057D709`) to a wrapper. The wrapper copies the bone's transform, sets its scale to 1, normalises its rotation, and calls the original builder (`0x00577240`; EA: `0x00576C50`). The solver then reads the bone's true orientation, the correction has unit length, and blended and unblended frames agree. Sims without scaled hands or feet get the same result to float precision.

```cpp
void __fastcall BuildRotationOnlyMatrix(const void* transform, void* /*edx*/, float* outMatrix) {
    BoneTransform rotationOnly;                       // alignas(16): the original reads it with movaps
    memcpy(&rotationOnly, transform, sizeof(rotationOnly));
    rotationOnly.scale[0] = rotationOnly.scale[1] = rotationOnly.scale[2] = 1.0f;
    Normalise(rotationOnly.rotation);
    g_originalBuildMatrix(&rotationOnly, outMatrix);  // __thiscall, callee pops 4
}
```

The call is found with the pattern `8B 5C 24 ?? 8D 3C 7F 8D 94 24 ?? ?? 00 00 C1 E7 04 52 8D 0C 1F E8 ?? ?? ?? ?? 0F 28 94 24 ?? ?? 00 00` (offset 21), plus a check of the call target's prologue. It matches once in the Steam build and once in the EA App build. The EA solver (`0x0057CC90`) is instruction-for-instruction identical to Steam's apart from addresses.

Without the fix, these stop the flicker:

| Option | Cost |
| --- | --- |
| RenderLimbIk off (S3SS Settings tab, Animation) | Hands and feet no longer adjust to objects and the ground |
| Smooth Animation off | Hands and feet stay at the wrong, unblended size; animation is no longer interpolated between sim snapshots |
| Slider scales only fingers or toes | The palm or foot itself can't get bigger without a geometry morph |

## Addresses

Steam 1.67.2 (image base `0x00400000`). EA App 1.69.47 equivalents found so far: the IK chain solver `0x0057CC90`, the patched call `0x0057D709`, and the transform-to-matrix helper `0x00576C50` → `0x00576B70`.

| Address | What it is | Role in the bug |
| --- | --- | --- |
| `0x011CD800` | Pointer to the animation settings object (built at `0x00572490`) | `+0x35` RenderLimbIk, `+0x36` Smooth Animation, `+0x37` Pose Caching, `+0x34` RenderRootIk, `+0x32` RenderLookAt |
| `0x0093F920` | Builds and serialises an actor's procedural node graph | Adds the LimbIk node (type `0x7419E37C`) when RenderLimbIk is on |
| `0x0054E630` | Clip node evaluate | Resets the pose to rest, then adds bone deltas (loop at `0x0054E780`) |
| `0x00550600` | IK node evaluate, shared by limb and root IK | Weight ramp and target setup, then calls the chain solver |
| `0x0057D670` | IK chain solver | Bends the limb, then aligns the end bone |
| `0x0057E0E9` | `call 0x00577240` on the end bone's model transform | The call the fix redirects |
| `0x00577240` → `0x00577160` | Transform to 4×4 matrix | Multiplies rotation rows by scale; assumes a unit quaternion |
| `0x0057E106`–`0x0057E386` | Matrix to quaternion (trace / largest diagonal) | Wrong for a scaled matrix |
| `0x0057E4C5` | Store of the end bone's new local rotation | Not normalised |
| `0x0054D7D0` | Blend node evaluate (type `0xD4E4CCF8`) | Lerps translation and scale, nlerps and renormalises rotation |
| `0x009420E0` | Per-actor animation update | Picks the blended (Smooth Animation) or single-snapshot path; pose cache |
| `0x00693660` | Interpolation clock | Returns −1 when render time has passed the newest snapshot |
| `0x00574460` | Pose to the actor's bone matrices | Builds matrices from local quaternions without normalising |

## Open questions

- **Which frames skip the blend.** The −1 path in the interpolation clock was read from the code, not watched at runtime. The fix doesn't depend on it.
- **Elbow and knee aim.** Three earlier calls in the solver (`0x0057DD2C`, `0x0057DEF1`, `0x0057DFA5`) also use scaled matrices. With uneven scaling on the upper arm, forearm, thigh or calf (Pcfreak Arm, for example), the bend direction may be slightly off. Those produce unit rotations, so they can't cause size flicker, and the fix leaves them alone.
- **Other rigs.** The numbers use the adult rig only; child, toddler and pet rigs weren't checked.

Credit to Just Harry for spotting that turning off RenderLimbIk stops the flicker, which pointed the search at limb IK.
