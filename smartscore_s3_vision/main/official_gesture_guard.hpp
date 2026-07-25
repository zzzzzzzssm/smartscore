#pragma once

#include <cstring>

namespace official_gesture_guard {

inline bool is_protected_label(const char *label)
{
    return label != nullptr &&
           (std::strcmp(label, "ok") == 0 || std::strcmp(label, "five") == 0 ||
            std::strcmp(label, "like") == 0);
}

template <typename CandidateRange>
const typename CandidateRange::value_type *find_protected_candidate(const CandidateRange &candidates,
                                                                     float confidence_threshold)
{
    for (const auto &candidate : candidates) {
        if (is_protected_label(candidate.cat_name) && candidate.score >= confidence_threshold) {
            return &candidate;
        }
    }
    return nullptr;
}

inline const char *veto_label(const char *label)
{
    if (label != nullptr && std::strcmp(label, "like") == 0) {
        return "official_veto_like";
    }
    if (label != nullptr && std::strcmp(label, "ok") == 0) {
        return "official_veto_ok";
    }
    if (label != nullptr && std::strcmp(label, "five") == 0) {
        return "official_veto_five";
    }
    return "official_veto";
}

} // namespace official_gesture_guard
