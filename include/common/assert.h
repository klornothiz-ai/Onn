#pragma once
#include <cassert>
#include <iostream>

#define KYTY_ASSERT(cond) assert(cond)
#define KYTY_ASSERT_MSG(cond, msg) assert((cond) && (msg))
#define KYTY_NOT_IMPLEMENTED() std::cerr << "NOT IMPLEMENTED: " << __FUNCTION__ << "\n"
