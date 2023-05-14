#pragma once

#include <unordered_map>
#include <string>
#include <ostream>
#include <istream>
#include <vector>

bool upgrade_websocket(const std::unordered_map<std::string, std::string>& headers, std::ostream& response);
const char* read_websocket(std::iostream& socketStream, std::vector<char>& buffer);
void write_websocket(std::ostream& socketStream, const char *data, size_t len);