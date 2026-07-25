#pragma once

#include <cstddef>
#include <cstdint>

namespace vision_config {

inline constexpr bool kUseQvga = false;
inline constexpr uint16_t kFrameWidth = kUseQvga ? 320 : 640;
inline constexpr uint16_t kFrameHeight = kUseQvga ? 240 : 480;
inline constexpr int kCameraXclkHz = 5'000'000;
inline constexpr int kJpegQuality = 12;
inline constexpr size_t kCameraFrameBufferCount = 2;

inline constexpr int64_t kCaptureFrameIntervalMs = 100;
inline constexpr int64_t kAiIntervalMs = 100;
inline constexpr int64_t kPhotoIntervalMs = 3000;
inline constexpr int64_t kMemoryLogIntervalMs = 10'000;
inline constexpr int64_t kQueueWarningIntervalMs = 5'000;
inline constexpr int64_t kCaptureWarningIntervalMs = 3'000;
inline constexpr size_t kAiJpegBufferBytes = 256U * 1024U;
inline constexpr uint32_t kAiWorkerTaskStackBytes = 8192;
inline constexpr unsigned kAiWorkerTaskPriority = 5;
inline constexpr int kAiWorkerCore = 1;

inline constexpr float kHandDetectConfidence = 0.30F;
inline constexpr float kGestureConfidence = 0.55F;
inline constexpr float kControlZoneBottomRatio = 0.85F;

inline constexpr size_t kClassificationHistorySize = 1;
inline constexpr size_t kClassificationVotesRequired = 1;
inline constexpr int64_t kOkHoldMs = 0;
inline constexpr int64_t kOpenPalmHoldMs = 0;
inline constexpr int64_t kThumbUpHoldMs = 0;
inline constexpr int64_t kActionCooldownMs = 800;

inline constexpr float kOfficialGesturePriorityConfidence = 0.55F;
inline constexpr int kOfficialGestureTopK = 3;
inline constexpr float kOfficialGestureVetoConfidence = 0.25F;
inline constexpr float kPointDirectionHandConfidence = 0.35F;
inline constexpr float kPointDirectionConfidence = 0.55F;
inline constexpr float kPointDirectionMargin = 0.20F;
// Training crops use 28% landmark padding; the runtime crop remains square when shifted at an edge.
inline constexpr float kPointCropPaddingRatio = 0.28F;

inline constexpr size_t kSdWriterQueueLength = 2;
inline constexpr uint32_t kSdWriterTaskStackBytes = 4096;
inline constexpr unsigned kSdWriterTaskPriority = 4;
inline constexpr int kSdMaxOpenFiles = 5;
inline constexpr size_t kSdAllocationUnitBytes = 16 * 1024;

inline constexpr size_t kSessionPathBufferSize = 160;
inline constexpr size_t kPhotoPathBufferSize = 224;
inline constexpr char kSdMountPoint[] = "/sdcard";
inline constexpr char kStorageRoot[] = "/sdcard/score";

static_assert(kClassificationVotesRequired <= kClassificationHistorySize);
static_assert(kSdWriterQueueLength <= 2);

} // namespace vision_config
