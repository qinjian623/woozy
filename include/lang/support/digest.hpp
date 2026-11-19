#pragma once
#include <string>
#include <string_view>
namespace lang {
std::string sha256(std::string_view bytes);
}
