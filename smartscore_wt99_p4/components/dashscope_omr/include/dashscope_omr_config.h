#pragma once

/* The real value is generated from the build process environment. */
#include "dashscope_omr_private_config.h"

/* Retained as a fail-safe for nonstandard builds. Never put a key here. */
#ifndef DASHSCOPE_API_KEY
#define DASHSCOPE_API_KEY "__DASHSCOPE_API_KEY__"
#endif
