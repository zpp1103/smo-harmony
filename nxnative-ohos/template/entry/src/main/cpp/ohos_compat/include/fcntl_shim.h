#pragma once
#ifndef F_FULLFSYNC
#define F_FULLFSYNC 51 /* shim: fcntl 返回 EINVAL，周末改 fdatasync */
#endif
