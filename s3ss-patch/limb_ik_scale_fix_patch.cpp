#include "../patch_system.h"
#include "../patch_helpers.h"
#include "../logger.h"
#include <cmath>
#include <cstring>
#include <format>

// Limb IK Scale Fix (S3SS patch version of LimbIkScaleFix.asi)
// Written with Claude, Anthropic's AI model; see the repository README, "How this was made".
// Not yet built inside S3SS. The standalone ASI with the same redirect is tested in game on EA App 1.69.47.
//
// Why hands/feet flicker between two sizes with hand/foot size sliders:
//
// 1. Bone-delta (BOND) sliders are added straight into the pose's per-bone local scale when a clip is evaluated
//    (Steam 0x0054E7A5..0x0054E835: local.scale += delta.scale), so e.g. b__L_Hand__ ends up with scale 1.2.
//
// 2. The limb IK chain solver (Steam 0x0057D670) finishes by turning the chain's end bone (hand/foot) to the
//    IK target's orientation. To read the bone's current model-space orientation it builds the bone's 4x4 model matrix
//    with the transform -> matrix helper (Steam 0x00577240 -> 0x00577160), which multiplies the rotation rows by the
//    bone's accumulated scale, then converts that matrix to a quaternion with the trace / largest-diagonal method
//    (Steam 0x0057E106..0x0057E386). That conversion is only valid for a pure rotation matrix. With scale != 1 it
//    returns a quaternion that is the wrong length and slightly the wrong direction.
//
// 3. The correction (target * conj(current)) is multiplied straight into the end bone's local rotation and stored
//    (Steam 0x0057E4C5), so the pose now holds a non-unit quaternion for the hand/foot. Quaternion -> matrix
//    (0x00577160) assumes unit length; with length m it produces (1 - m^2) * I + m^2 * R, which visibly resizes and
//    shears the hand/foot (and everything parented to it).
//
// 4. Smooth Animation's two-pose blend (Steam 0x0054D7D0) nlerps and renormalises every bone rotation, which hides
//    the error. Frames that skip the blend (e.g. the render clock ran past the newest sim snapshot -> t = -1, Steam
//    0x00693660 / 0x009420E0) render the raw IK pose. Mixing the two kinds of frames gives the two alternating sizes.
//
// Fix: redirect that one call so the end bone's matrix is built from a copy of its transform with the scale forced to
// (1,1,1) and the rotation re-normalised. The matrix -> quaternion step then gets a pure rotation, the correction is unit
// length, and both kinds of frames agree. Nothing else in the solver changes; sims without scaled limbs are unaffected
// (for them the old and new matrices match to float precision).

namespace {

// Engine bone transform as stored in pose buffers (48 bytes, 16-byte aligned):
// translation (x,y,z,w), rotation quaternion (x,y,z,w), scale (x,y,z,w).
struct alignas(16) EngineBoneTransform {
    float translation[4];
    float rotation[4];
    float scale[4];
};
static_assert(sizeof(EngineBoneTransform) == 0x30, "EngineBoneTransform must match the engine layout");

// Original transform -> 4x4 matrix helper (Steam 0x00577240): __thiscall, one stack argument, callee pops it.
using BuildMatrixFn = void(__thiscall*)(const EngineBoneTransform* transform, float* outMatrix);
BuildMatrixFn g_originalBuildMatrix = nullptr;

// Drop-in replacement for the call site. Same ABI as the original: transform in ECX, output pointer on the stack,
// callee pops 4 bytes. __fastcall with an unused EDX parameter gives exactly that.
void __fastcall BuildRotationOnlyMatrix(const EngineBoneTransform* transform, void* /*unusedEdx*/, float* outMatrix) {
    EngineBoneTransform rotationOnly;
    std::memcpy(&rotationOnly, transform, sizeof(rotationOnly));

    // The solver only wants the bone's orientation here, not its size.
    rotationOnly.scale[0] = 1.0f;
    rotationOnly.scale[1] = 1.0f;
    rotationOnly.scale[2] = 1.0f;

    // Make sure the matrix is a pure rotation even if an earlier step left a non-unit quaternion behind.
    float* q = rotationOnly.rotation;
    const float lengthSq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (lengthSq > 1e-12f) {
        const float inverseLength = 1.0f / std::sqrt(lengthSq);
        q[0] *= inverseLength;
        q[1] *= inverseLength;
        q[2] *= inverseLength;
        q[3] *= inverseLength;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }

    g_originalBuildMatrix(&rotationOnly, outMatrix);
}

} // namespace

class LimbIkScaleFixPatch : public OptimizationPatch {
  private:
    std::vector<PatchHelper::PatchLocation> patchedLocations;

    // The call that builds the end bone's model matrix right before the matrix -> quaternion step in the limb IK solver.
    // Steam 0x0057E0D4:
    //   8B 5C 24 4C           mov ebx, [esp+4Ch]        ; model-space transform array
    //   8D 3C 7F              lea edi, [edi+edi*2]      ; end bone index * 3
    //   8D 94 24 20 03 00 00  lea edx, [esp+320h]       ; out matrix
    //   C1 E7 04              shl edi, 4                ; * 16 -> * 48 bytes
    //   52                    push edx
    //   8D 0C 1F              lea ecx, [edi+ebx]        ; &model[endBone]
    //   E8 xx xx xx xx        call 0x00577240           ; <- redirected
    //   0F 28 94 24 20 03 00 00 movaps xmm2, [esp+320h] ; matrix row 0 -> quaternion conversion
    static inline const AddressInfo endEffectorMatrixCall = {
        .name = "LimbIkScaleFix::endEffectorMatrixCall",
        .addresses = {{GameVersion::Steam, 0x0057E0E9}, {GameVersion::EA, 0x0057D709}},
        .pattern = "8B 5C 24 ?? 8D 3C 7F 8D 94 24 ?? ?? 00 00 C1 E7 04 52 8D 0C 1F E8 ?? ?? ?? ?? 0F 28 94 24 ?? ?? 00 00",
        .patternOffset = 21,
        .expectedBytes = {0xE8},
    };

    // Prologue of the original helper (Steam 0x00577240), checked so we never redirect a call to something else:
    //   push ebp / mov ebp,esp / and esp,-16 / mov edx,[ebp+8] / push edx / call 0x00577160
    static constexpr BYTE kBuildMatrixPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x55, 0x08, 0x52, 0xE8};

  public:
    LimbIkScaleFixPatch() : OptimizationPatch("LimbIkScaleFix", nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        LOG_INFO("[LimbIkScaleFix] Installing...");

        auto callSite = endEffectorMatrixCall.Resolve();
        if (!callSite) return Fail("Could not find the limb IK end-effector matrix call");

        const int32_t relative = *reinterpret_cast<const int32_t*>(*callSite + 1);
        const uintptr_t originalTarget = *callSite + 5 + relative;
        if (!PatchHelper::ValidateBytes(reinterpret_cast<LPVOID>(originalTarget), kBuildMatrixPrologue, sizeof(kBuildMatrixPrologue))) {
            return Fail(std::format("Call at {:#010x} does not target the expected matrix helper ({:#010x})", *callSite, originalTarget));
        }

        // Must be set before the call is redirected.
        g_originalBuildMatrix = reinterpret_cast<BuildMatrixFn>(originalTarget);

        auto tx = PatchHelper::BeginTransaction();
        bool ok = PatchHelper::WriteRelativeCall(*callSite, reinterpret_cast<uintptr_t>(&BuildRotationOnlyMatrix), &tx.locations);
        if (!ok || !PatchHelper::CommitTransaction(tx)) {
            PatchHelper::RollbackTransaction(tx);
            return Fail("Failed to redirect the end-effector matrix call");
        }
        patchedLocations = tx.locations;

        LOG_INFO(std::format("[LimbIkScaleFix] Redirected call at {:#010x} (original helper {:#010x})", *callSite, originalTarget));
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        LOG_INFO("[LimbIkScaleFix] Uninstalling...");

        if (!PatchHelper::RestoreAll(patchedLocations)) return Fail("Failed to restore original bytes");

        // g_originalBuildMatrix is left set on purpose: a solver call already inside the wrapper can still finish.
        isEnabled = false;
        LOG_INFO("[LimbIkScaleFix] Successfully uninstalled");
        return true;
    }
};

REGISTER_PATCH(LimbIkScaleFixPatch,
    {.displayName = "Limb IK Scale Fix",
        .description = "Fixes hands/feet flickering between two sizes on sims using hand or foot size sliders.\n"
                       "Can be toggled at any time.",
        .category = "Animation",
        .experimental = true,
        .supportedVersions = VERSION_ALL,
        .technicalDetails = {"The limb IK solver reads the hand/foot's current orientation by converting its scaled model matrix to a quaternion.",
            "With a size slider on the hand/foot (or a scaled parent bone) that quaternion comes out non-unit and skewed, and gets written into the pose.",
            "Smooth Animation's blend renormalises it on some frames but not others, so the hand/foot alternates between two sizes.",
            "This redirects the one matrix build in that step (Steam 0x0057E0E9) to a copy of the transform with scale 1 and a normalised rotation.",
            "Sims without scaled hands/feet are unaffected (same result to float precision)."}})
