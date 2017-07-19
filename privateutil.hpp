#pragma once
#include "publicutil.hpp"
#include <stdexcept>

DEFINE_EXCEPTION(FFmpegError, std::runtime_error)

#define CHECK_AVRESULT(code, successCond) \
if(!(successCond)) \
{ \
  char _buf[1024]; \
  av_strerror(code, _buf, 1024); \
  qCritical("[%s:%s:%d]Operation failed: %s", __FILE__, __func__, __LINE__, _buf); \
  throw FFmpegError("FFmpeg operation failed."); \
}

void lockFFmpeg();
void unlockFFmpeg();
