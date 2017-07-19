#include "privateutil.hpp"
#include <QMutex>

IMPL_EXCEPTION(FFmpegError, std::runtime_error)
static QMutex g_ffmpegLocker;

void lockFFmpeg()
{ g_ffmpegLocker.lock(); }

void unlockFFmpeg()
{ g_ffmpegLocker.unlock(); }
