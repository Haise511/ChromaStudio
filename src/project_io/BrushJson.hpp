#pragma once
// ChromaStudio Engine · project_io · brush profiles as §6.1 JSON.
//
// brushToJson writes every field of the schema (the starting profiles are fully materialised);
// brushFromJson reads one object strictly: no unknown keys, every required field, enum spellings,
// family rules, then the §6.1 ranges through core::validate. catalogFromJson reads a `brushes.json`
// catalogue {"version": 1, "brushes": [...]}: 1–4096 profiles, IDs unique.

#include "core/BrushProfile.hpp"
#include "project_io/Json.hpp"

#include <string>
#include <vector>

namespace chroma::project {

json::Value brushToJson(const core::BrushProfile& profile);

/// Throws std::runtime_error naming the first problem (Russian).
core::BrushProfile brushFromJson(const json::Value& value);

json::Value catalogToJson(const std::vector<core::BrushProfile>& brushes);
std::vector<core::BrushProfile> catalogFromJson(const json::Value& value);

}  // namespace chroma::project
