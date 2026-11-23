// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/theme/scene.hpp>

#include <array>

namespace headunit::theme::graphics {

inline const std::array<std::vector<Point>, 3> brand_polygons{{
    {{24, 76},
     {239, 76},
     {239, 184},
     {221, 166},
     {221, 94},
     {42, 94},
     {42, 194},
     {217, 194},
     {235, 212},
     {24, 212}},
    {{194, 143}, {222, 143}, {279, 200}, {251, 200}},
    {{264, 104},
     {406, 104},
     {406, 228},
     {290, 228},
     {272, 210},
     {388, 210},
     {388, 122},
     {282, 122},
     {282, 176},
     {264, 158}},
}};

}  // namespace headunit::theme::graphics
