#pragma once
#include <cstdlib>
inline void* osi_calloc(size_t n) { void* p = std::calloc(1, n); if (!p) std::abort(); return p; }
inline void osi_free(void* p) { std::free(p); }
