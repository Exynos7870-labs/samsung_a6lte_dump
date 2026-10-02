#pragma once
#include <cstdio>
#define LOG_ERROR(tag, format, ...) std::fprintf(stderr, tag ": " format "\n", ##__VA_ARGS__)
#define LOG_INFO(tag, format, ...) std::fprintf(stderr, tag ": " format "\n", ##__VA_ARGS__)
