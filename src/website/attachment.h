#pragma once

#include "driver/picojson.h"
#include "driver/package.h"

std::string attach(picojson::value& link, const std::string& path, const std::string& ext, const std::string& mime);
const std::vector<unsigned char> b64decode(const void* data, const size_t& len);
std::vector<unsigned char> b64decode(const std::string& str64);
const std::string b64encode(const void* data, const size_t& len);
std::string b64encode(const std::string& str);