#pragma once

#include <cstddef>
#include "raw_noise_model.h"

namespace rtengine {

// Private engine implementation contract, not an exported C++ SDK. External
// callers must use rte_raw_session_* so engine-global serialization is applied.

class ProcessingJob;
class ProgressListener;
class IImagefloat;
class WorkingImageSession;

// Negative session errors; positive decode errors are passed through unchanged.
enum WorkingImageSessionError {
    WORKING_IMAGE_INVALID_ARGUMENT = -1,
    WORKING_IMAGE_UNSUPPORTED_PIPELINE = -2,
    WORKING_IMAGE_PROCESSING_FAILED = -3
};

// Takes ownership of job, including on failure. listener is borrowed only for
// this call. Requires workingProfile == "ProPhoto".
// FULL uses the normal pipeline; fast+resize exactly reuses the legacy fast
// order: init -> transform -> early resize -> denoise -> retain.
// V1 retains AFTER stage_transform (and early resize/NR for previews): geometry, gamut compression, dehaze,
// Fattal and other base operations are already baked in, not just demosaic.
// This is NOT a pre-NR buffer or an external noise-reduction contract.
WorkingImageSession* createWorkingImageSession(
    ProcessingJob* job, int& errorCode, ProgressListener* listener = nullptr,
    bool flush = true);

// New internal source mode; same ownership, listener and pipeline constraints as
// V1, and uses the same get/finish/destroy functions below. Retains immediately
// AFTER stage_to_working_space and BEFORE stage_working_base_operations.
// Explicitly disables legacy dirpyrDenoise on the owned job before init and
// skips its RGB NR stage, avoiding double NR with an external source editor.
// This is NOT untouched demosaic data or a pre-DCP-nonlinear boundary: input
// profile conversion has already run (including enabled DCP processing), as
// have WB, configured raw preprocessing/capture sharpening/Retinex/spot removal
// and film-negative processing. Other configured noise-reduction tools are NOT
// disabled by this mode. "Source" only means before the working base operations
// (gamut compression, first analysis, dehaze, Fattal and geometry).
// Preview: toWorking -> early resize of the full canvas -> retain. Geometry
// runs on that scaled canvas at finish, then its scaled crop is applied. This
// reordering (including Lab resize) does NOT promise legacy preview byte parity.
WorkingImageSession* createProPhotoSourceSession(
    ProcessingJob* job, int& errorCode, ProgressListener* listener = nullptr,
    bool flush = true);

// Borrowed immutable tightly packed RGBRGB floats, row-major, linear ProPhoto
// channel encoding in RT units (nominal white 65535, NOT normalized to 1).
// No clipping is applied. Lifetime: until destroy, including across finish.
// "Linear" describes channel encoding, not absence of profile/other processing;
// whether working base operations are baked in depends on the create function.
const float* getWorkingImageSessionLinear(
    const WorkingImageSession* session, int& width, int& height,
    std::size_t& floatCount);

// Owned value copy of create-time evidence, unchanged by finish (including
// edited finishes). Reference matches do NOT certify working-image noise.
// nullptr returns a default invalid model. Same serialization as get/finish.
raw_noise::Model getWorkingImageSessionNoiseModel(const WorkingImageSession* session);

// nullptr + floatCount==0 uses the unmodified retained planar source (no
// interleaved roundtrip). Otherwise copies exactly width*height*3 finite floats
// with the same layout/units as get. Does not retain editedRGB or listener.
// Returns a caller-owned output image, just as processImage does. Session is
// reusable after success or failure; finish never decodes/demosaics again.
// Source mode runs working base operations before stage_finish on each fresh
// image/params/ipf/histogram state; V1 keeps its existing post-transform path.
IImagefloat* finishWorkingImageSession(
    WorkingImageSession* session, const float* editedRGB, std::size_t floatCount,
    int& errorCode, ProgressListener* listener = nullptr);

// nullptr is allowed. Calls on a session must be externally serialized;
// destroy must not race get/finish. No engine-global locking is provided here.
void destroyWorkingImageSession(WorkingImageSession* session);

// Native is the full oriented stage_init canvas, before early resize/crop,
// not the sensor's masked pixel array. Retained is the editable RGB canvas.
// Spatial scale only: never a sensor/working-noise calibration factor.
struct WorkingImageSessionGeometry {
    int nativeWidth;
    int nativeHeight;
    int retainedWidth;
    int retainedHeight;
    double resolutionScale;
};
WorkingImageSessionGeometry getWorkingImageSessionGeometry(const WorkingImageSession* session);

} // namespace rtengine
