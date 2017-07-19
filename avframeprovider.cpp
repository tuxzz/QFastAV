#include "avframeprovider.hpp"
#include "avseeker.hpp"
#include "avpacketprovider.hpp"
#include "avpacketdecoder.hpp"
#include "privateutil.hpp"
#include <QDebug>

IMPL_EXCEPTION(IOError, std::runtime_error)
IMPL_EXCEPTION(NoStreamError, std::runtime_error)

AVFrameProvider::AVFrameProvider(const QString &path, bool enableAudio, bool enableVideo)
{
  m_path = path;

  m_pIOCtx = nullptr;
  m_pFormatCtx = nullptr;
  m_iAudioStream = AVERROR_STREAM_NOT_FOUND;
  m_iVideoStream = AVERROR_STREAM_NOT_FOUND;
  m_pAudioStream = nullptr;
  m_pVideoStream = nullptr;

  m_seeker = nullptr;
  m_packetProvider = nullptr;
  m_packetDecoder = nullptr;

  m_currentFrameType = UnknownFrame;
  m_currentAudioFrame = nullptr;
  m_currentVideoFrame = nullptr;
  m_videoPts = 0.0;
  m_audioPts = 0.0;

  m_audioFinished = false;
  m_videoFinished = false;

  m_file.setFileName(path);
  if(!m_file.open(QFile::ReadOnly))
  {
    qCritical()<<"Failed to open file for reading:"<<path;
    throw IOError("Failed to open file for reading.");
  }

  // create io context
  {
    m_pIOCtx = avio_alloc_context(m_ioBuffer, sizeof(m_ioBuffer), 0, reinterpret_cast<void*>(this), &_ioReadPacket, nullptr, &_ioSeek);
    if(!m_pIOCtx)
      throw FFmpegError("Cannot create ffmpeg io context.");
  }

  // open file
  {
    // probe
    qint64 realReadSize = m_file.read(reinterpret_cast<char*>(m_ioBuffer), sizeof(m_ioBuffer));
    if(realReadSize < 0)
    {
      qCritical("Cannot read file header.");
      throw IOError("Cannot read file header.");
    }
    m_file.seek(0);

    AVProbeData probeData;
    memset(reinterpret_cast<void*>(&probeData), 0, sizeof(probeData));
    probeData.buf = m_ioBuffer;
    probeData.buf_size = realReadSize;
    probeData.filename = "aaa";
    AVInputFormat *pInputFormat = av_probe_input_format(&probeData, 1);
    if(!pInputFormat)
      throw IOError("Unsupported input format");

    // open context
    m_pFormatCtx = avformat_alloc_context();
    if(!m_pFormatCtx)
      throw FFmpegError("Cannot create ffmpeg format context.");
    m_pFormatCtx->pb = m_pIOCtx;
    m_pFormatCtx->iformat = pInputFormat;
    m_pFormatCtx->flags = AVFMT_FLAG_CUSTOM_IO;

    int openFileResult = avformat_open_input(&m_pFormatCtx, "", nullptr, nullptr);
    CHECK_AVRESULT(openFileResult, openFileResult == 0);
  }

  // initialize stream info
  {
    int findStreamInfoResult = avformat_find_stream_info(m_pFormatCtx, nullptr);
    CHECK_AVRESULT(findStreamInfoResult, findStreamInfoResult >= 0);
  }

  // get stream, copy codec context
  for(int i = 0; i < static_cast<int>(m_pFormatCtx->nb_streams); ++i)
  {
    AVStream *pStream = m_pFormatCtx->streams[i];

    if(enableVideo && !m_pVideoStream && pStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    {
      if(pStream->codecpar)
      {
        m_iVideoStream = i;
        m_pVideoStream = pStream;
      }
      else
        qWarning("Unsupported video codec at stream #%d.", i);
    }
    else if(enableAudio && !m_pAudioStream && pStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
    {
      if(pStream->codecpar)
      {
        m_iAudioStream = i;
        m_pAudioStream = pStream;
      }
      else
        qWarning("Unsupported audio codec at stream #%d.", i);
    }
    else
      qWarning("Unsupported stream #%d.", i);
  }

  // check stream
  if(!m_pAudioStream && !m_pVideoStream)
    throw NoStreamError("No stream available.");

  // alloc frame
  if(m_pVideoStream)
    m_currentVideoFrame = av_frame_alloc();
  if(m_pAudioStream)
    m_currentAudioFrame = av_frame_alloc();

  m_seeker = new AVSeeker(m_pFormatCtx);
  {
    AVPacketProvider::StreamSet streamSet;
    if(m_pVideoStream)
      streamSet.insert(m_iVideoStream);
    if(m_pAudioStream)
      streamSet.insert(m_iAudioStream);
    m_packetProvider = new AVPacketProvider(m_pFormatCtx, streamSet);
    m_packetDecoder = new AVPacketDecoder(m_packetProvider, m_pFormatCtx, streamSet);
  }
}

AVFrameProvider::~AVFrameProvider()
{
  if(m_packetDecoder)
  {
    m_packetDecoder->locker()->lock();
    m_packetDecoder->requestInterruption();
    m_packetDecoder->requestWakeUp_lockfree();
    m_packetDecoder->locker()->unlock();
    m_packetDecoder->wait();
    delete m_packetDecoder;
  }

  if(m_packetProvider)
  {
    m_packetProvider->locker()->lock();
    m_packetProvider->requestInterruption();
    m_packetProvider->requestWakeUp_lockfree();
    m_packetProvider->locker()->unlock();
    m_packetProvider->wait();
    delete m_packetProvider;
  }
  if(m_seeker)
  {
    m_seeker->wait();
    delete m_seeker;
  }
  if(m_pVideoStream)
  {
    av_frame_unref(m_currentVideoFrame);
    av_frame_free(&m_currentVideoFrame);
  }
  if(m_pAudioStream)
  {
    av_frame_unref(m_currentAudioFrame);
    av_frame_free(&m_currentAudioFrame);
  }
  if(m_pFormatCtx)
    avformat_close_input(&m_pFormatCtx);
  if(m_pIOCtx)
    av_free(reinterpret_cast<AVIOContext*>(m_pIOCtx));
}

QString AVFrameProvider::path() const
{ return m_path; }

void AVFrameProvider::seek(double time, bool async)
{
  if(isDecoderRunning())
  {
    qWarning("Seeking on decoder running.");
    stopDecoder(async);
  }
  m_seeker->wait();
  m_videoFinished = false;
  m_audioFinished = false;
  qint64 pts = static_cast<qint64>(std::round(time * static_cast<double>(AV_TIME_BASE)));
  if(async)
  {
    m_seeker->setPos_lockfree(pts);
    m_seeker->start();
  }
  else
    av_seek_frame(m_pFormatCtx, -1, pts, AVSEEK_FLAG_BACKWARD);
}

void AVFrameProvider::waitSeekDone()
{ m_seeker->wait(); }

void AVFrameProvider::startDecoder(bool async)
{
  waitSeekDone();
  if(!m_packetProvider->isRunning())
    m_packetProvider->requestStart();
  else
    qCritical("Packet provider is already running.");
  if(!m_packetDecoder->isRunning())
    m_packetDecoder->requestStart();
  else
    qCritical("Packet decoder is already running.");
  if(!async)
  {
    m_packetProvider->locker()->lock();
    m_packetProvider->waitUntilFullyStarted_lockfree();
    m_packetProvider->locker()->unlock();
    m_packetDecoder->locker()->lock();
    m_packetDecoder->waitUntilFullyStarted_lockfree();
    m_packetDecoder->locker()->unlock();
  }
}

void AVFrameProvider::stopDecoder(bool async)
{
  waitSeekDone();
  m_packetDecoder->locker()->lock();
  m_packetDecoder->requestInterruption();
  m_packetDecoder->requestWakeUp_lockfree();
  m_packetDecoder->locker()->unlock();
  if(!async)
    m_packetDecoder->wait();
  m_packetProvider->locker()->lock();
  m_packetProvider->requestInterruption();
  m_packetProvider->clearQueue_lockfree();
  m_packetProvider->requestWakeUp_lockfree();
  m_packetProvider->locker()->unlock();
  if(!async)
    m_packetProvider->wait();
}

bool AVFrameProvider::isDecoderRunning() const
{ return m_packetDecoder->isRunning() || m_packetProvider->isRunning(); }

AVFrameProvider::FrameType AVFrameProvider::currentFrameType() const
{ return m_currentFrameType; }

const AVFrame *AVFrameProvider::currentFrame() const
{
  if(m_currentFrameType == AudioFrame)
    return m_currentAudioFrame;
  else if(m_currentFrameType == VideoFrame)
    return m_currentVideoFrame;
  else
    return nullptr;
}

const AVFrame *AVFrameProvider::currentAudioFrame() const
{ return m_currentAudioFrame; }

const AVFrame *AVFrameProvider::currentVideoFrame() const
{ return m_currentVideoFrame; }

bool AVFrameProvider::nextFrame()
{
  bool ok = false;
  while(!ok)
  {
    if(isFinished())
    {
      m_currentFrameType = UnknownFrame;
      return false;
    }
    else if(!isAudioFinished() && (m_audioPts < m_videoPts || isVideoFinished()))
      ok = nextAudioFrame();
    else if(m_pVideoStream)
      ok = nextVideoFrame();
    else
    {
      qFatal("No available stream.");
      throw std::runtime_error("No available stream.");
    }
  }
  return true;
}

bool AVFrameProvider::nextAudioFrame()
{
  if(isAudioFinished())
    return false;
  av_frame_unref(m_currentAudioFrame);
  m_audioFinished = !m_packetDecoder->getFrame(m_iAudioStream, m_currentAudioFrame);
  if(m_audioFinished)
    m_currentFrameType = UnknownFrame;
  else
  {
    m_currentFrameType = AudioFrame;
    m_audioPts = _calcPts(m_pAudioStream, m_currentAudioFrame);
  }
  return !m_audioFinished;
}

bool AVFrameProvider::nextVideoFrame()
{
  if(isVideoFinished())
    return false;
  av_frame_unref(m_currentVideoFrame);
  m_videoFinished = !m_packetDecoder->getFrame(m_iVideoStream, m_currentVideoFrame);

  if(m_videoFinished)
    m_currentFrameType = UnknownFrame;
  else
  {
    m_currentFrameType = VideoFrame;
    m_videoPts = _calcPts(m_pVideoStream, m_currentVideoFrame);
  }
  return !m_videoFinished;
}

bool AVFrameProvider::isAudioFinished() const
{ return !m_pAudioStream || m_audioFinished; }

bool AVFrameProvider::isVideoFinished() const
{ return !m_pVideoStream || m_videoFinished; }

bool AVFrameProvider::isFinished() const
{ return isAudioFinished() && isVideoFinished(); }

double AVFrameProvider::videoPts() const
{ return m_videoPts; }

double AVFrameProvider::audioPts() const
{ return m_audioPts; }

double AVFrameProvider::duration() const
{ return static_cast<double>(m_pFormatCtx->duration) / static_cast<double>(AV_TIME_BASE); }

bool AVFrameProvider::hasVideo() const
{ return m_pVideoStream != nullptr; }

double AVFrameProvider::videoFramerate() const
{
  Q_ASSERT(hasVideo());
  return av_q2d(m_pVideoStream->r_frame_rate);
}

QSize AVFrameProvider::videoSize() const
{
  Q_ASSERT(hasVideo());
  return QSize(m_pVideoStream->codecpar->width, m_pVideoStream->codecpar->height);
}

AVPixelFormat AVFrameProvider::videoPixelFormat() const
{
  Q_ASSERT(hasVideo());
  return static_cast<AVPixelFormat>(m_pVideoStream->codecpar->format);
}

bool AVFrameProvider::hasAudio() const
{ return m_pAudioStream != nullptr; }

int AVFrameProvider::audioSamprate() const
{
  Q_ASSERT(hasAudio());
  return m_pAudioStream->codecpar->sample_rate;
}

AVSampleFormat AVFrameProvider::audioSampleFormat() const
{
  Q_ASSERT(hasAudio());
  return static_cast<AVSampleFormat>(m_pAudioStream->codecpar->format);
}

int AVFrameProvider::_ioReadPacket(void *opaque, uint8_t *buf, int buf_size)
{
  auto provider = reinterpret_cast<AVFrameProvider*>(opaque);

  provider->m_fileLock.lock();
  int bytesRead = provider->m_file.read(reinterpret_cast<char*>(buf), buf_size);
  provider->m_fileLock.unlock();

  if(bytesRead == 0)
    return AVERROR_EOF;
  else if(bytesRead < 0)
  {
    qWarning("Failed to read buffer.");
    return -1;
  }
  else
    return bytesRead;
}

int64_t AVFrameProvider::_ioSeek(void *opaque, int64_t offset, int whence)
{
  Q_ASSERT(opaque);
  whence &= ~AVSEEK_FORCE;
  auto provider = reinterpret_cast<AVFrameProvider*>(opaque);

  bool seekResult = true;
  int returnResult;
  provider->m_fileLock.lock();
  if(whence == AVSEEK_SIZE)
    returnResult = provider->m_file.size();
  else
  {
    if(whence == SEEK_SET)
      seekResult = provider->m_file.seek(offset);
    else if(whence == SEEK_CUR)
      seekResult = provider->m_file.seek(provider->m_file.pos() + offset);
    else if(whence == SEEK_END)
      seekResult = provider->m_file.seek(provider->m_file.size() + offset);
    else
    {
      qFatal("Invalid whence %d", whence);
      std::abort();
    }
    returnResult = provider->m_file.pos();
  }
  provider->m_fileLock.unlock();

  if(seekResult)
    return returnResult;
  else
  {
    qWarning("Failed to seek.");
    return -1;
  }
}

double AVFrameProvider::_calcPts(AVStream *pStream, AVFrame *pFrame)
{ return static_cast<double>(pFrame->pts * pStream->time_base.num) / static_cast<double>(pStream->time_base.den); }
