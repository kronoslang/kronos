#pragma once
#include "picojson.h"
#include <ostream>

bool equal(picojson::value const& l, picojson::value const& r, std::ostream* diags = nullptr);