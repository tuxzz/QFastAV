#pragma once

#include <QString>
#include <QSize>
#include <QFile>
#include <QMutex>
#include "publicutil.hpp"
extern "C"
{
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class AVSeeker;
class AVPacketProvider;
class AVPacketDecoder;

DEFINE_EXCEPTION(IOError, std::runtime_error)
DEFINE_EXCEPTION(NoStreamError, std::runtime_error)

class AVFrameProvider final
{
public:
  enum FrameType
  {
    UnknownFrame = 0,
    AudioFrame,
    VideoFrame
  };

  AVFrameProvider(const QString &path, bool enableAudio, bool enableVideo);
  ~AVFrameProvider();

  QString path() const;

  void seek(double time, bool async = true);
  void waitSeekDone();

  void startDecoder(bool async = true);
  void stopDecoder(bool async = true);
  bool isDecoderRunning() const;

  FrameType currentFrameType() const;
  const AVFrame *currentFrame() const;
  const AVFrame *currentAudioFrame() const;
  const AVFrame *currentVideoFrame() const;
  bool nextFrame();
  bool nextAudioFrame();
  bool nextVideoFrame();
  bool isAudioFinished() const;
  bool isVideoFinished() const;
  bool isFinished() const;

  double videoPts() const;
  double audioPts() const;

  double duration() const;

  bool hasVideo() const;
  double videoFramerate() const;
  QSize videoSize() const;
  AVPixelFormat videoPixelFormat() const;

  bool hasAudio() const;
  int audioSamprate() const;
  AVSampleFormat audioSampleFormat() const;

private:
  static int _ioReadPacket(void *opaque, uint8_t *buf, int buf_size);
  static int64_t _ioSeek(void *opaque, int64_t offset, int whence);
  static double _calcPts(AVStream *pStream, AVFrame *pFrame);

  QString m_path;
  QMutex m_fileLock;
  QFile m_file;
  unsigned char m_ioBuffer[32 * 1024];

  AVIOContext *m_pIOCtx;
  AVFormatContext *m_pFormatCtx;
  int m_iAudioStream, m_iVideoStream;
  AVStream *m_pAudioStream, *m_pVideoStream;

  AVSeeker *m_seeker;
  AVPacketProvider *m_packetProvider;
  AVPacketDecoder *m_packetDecoder;

  FrameType m_currentFrameType;
  AVFrame *m_currentAudioFrame, *m_currentVideoFrame;
  double m_videoPts, m_audioPts;

  bool m_audioFinished, m_videoFinished;
};
