#pragma once

#include <string>

namespace Slic3r {

// Identifiers of the cloud services an ICloudServiceAgent can stand for.
static const std::string INLONG_CLOUD_PROVIDER("inlong");
// Kept for compatibility with upstream Orca integrations and plugin metadata.
static const std::string ORCA_CLOUD_PROVIDER("orca");
static const std::string BBL_CLOUD_PROVIDER("bbl");

} // namespace Slic3r
