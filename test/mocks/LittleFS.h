#ifndef FASTBEE_TEST_MOCK_LITTLEFS_H
#define FASTBEE_TEST_MOCK_LITTLEFS_H

#include "MockLittleFS.h"

using File = MockFile;

#ifndef FILE_READ
#define FILE_READ "r"
#endif
#ifndef FILE_WRITE
#define FILE_WRITE "w"
#endif

#endif
