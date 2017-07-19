#include "avpacketdecoder.hpp"
#include "avpacketprovider.hpp"
#include "privateutil.hpp"

AVPacketDecoder::AVPacketDecoder(AVPacketProvider *packetProvider, AVFormatContext *pFormatCtx, const StreamSet &streamSet, QObject *parent) : QThread(parent)
{
  m_packetProvider = packetProvider;
  for(int iStream:streamSet)
  {
    Q_ASSERT(iStream >= 0 && iStream < static_cast<int>(pFormatCtx->nb_streams));
    AVStream *pStream = pFormatCtx->streams[iStream];
    AVCodecParameters *pCodecPar = pStream->codecpar;

    // find decoder
    AVCodec *pCodec = avcodec_find_decoder(pStream->codecpar->codec_id);
    if(!pCodec)
    {
      qCritical("Could not found available codec for stream %d.", iStream);
      throw FFmpegError("Could not found available codec.");
    }

    // create codec context
    AVCodecContext *pCodecCtx = avcodec_alloc_context3(pCodec);
    if(!pCodecCtx)
    {
      qCritical("Could not alloc codec context for stream %d.", iStream);
      throw FFmpegError("Could not alloc codec context.");
    }
    {
      int parToCtxResult = avcodec_parameters_to_context(pCodecCtx, pCodecPar);
      CHECK_AVRESULT(parToCtxResult, parToCtxResult >= 0);
    }

    // set parameter
    av_codec_set_pkt_timebase(pCodecCtx, pStream->time_base);
    if(pCodecPar->codec_type == AVMEDIA_TYPE_VIDEO)
      pCodecCtx->thread_count = av_cpu_count();
    else
      pCodecCtx->thread_count = av_cpu_count() / 2;
    pCodecCtx->thread_type = FF_THREAD_FRAME;

    // open codec
    {
      lockFFmpeg();
      int codecOpenResult = avcodec_open2(pCodecCtx, pCodec, nullptr);
      unlockFFmpeg();
      CHECK_AVRESULT(codecOpenResult, codecOpenResult == 0);
    }

    m_streamDict.insert(iStream, pCodecCtx);
  }
  m_fullyStarted = false;
}

AVPacketDecoder::~AVPacketDecoder()
{
  for(AVCodecContext *pCodecCtx:m_streamDict)
  {
    avcodec_close(pCodecCtx);
    avcodec_free_context(&pCodecCtx);
  }
}

QMutex *AVPacketDecoder::locker()
{ return &m_locker; }

QWaitCondition *AVPacketDecoder::syncer()
{ return &m_syncer; }

void AVPacketDecoder::requestFeeding_lockfree()
{ m_syncer.wakeAll(); }

void AVPacketDecoder::requestWakeUp_lockfree()
{ m_syncer.wakeAll(); }

bool AVPacketDecoder::getFrame(int iStream, AVFrame *pOut)
{
  AVCodecContext *pCodecCtx = m_streamDict.value(iStream, nullptr);
  Q_ASSERT(pCodecCtx);

  while(true)
  {
    m_locker.lock();
    waitUntilFullyStarted_lockfree();
    int receiveFrameResult = avcodec_receive_frame(pCodecCtx, pOut);
    m_syncer.wakeAll();
    m_locker.unlock();

    if(receiveFrameResult >= 0)
      return true;
    else if(receiveFrameResult == AVERROR_EOF)
      return false;
    else if(receiveFrameResult == AVERROR(EAGAIN))
    {
      if(!isRunning())
      {
        qWarning("EOF is not seen.");
        return false;
      }
    }
    else
      CHECK_AVRESULT(receiveFrameResult, false);
  }
}

void AVPacketDecoder::waitUntilFullyStarted_lockfree()
{
  if(!m_fullyStarted && isRunning())
    m_syncer.wait(&m_locker);
}

void AVPacketDecoder::requestStart()
{
  Q_ASSERT(!isRunning());
  m_fullyStarted = false;
  start();
}

void AVPacketDecoder::run()
{
  m_locker.lock();

  auto end = m_streamDict.cend();
  for(auto it = m_streamDict.cbegin(); it != end; ++it)
  {
    AVCodecContext *pCodecCtx = it.value();
    avcodec_flush_buffers(pCodecCtx);
  }

  QMutex *packetLocker = m_packetProvider->locker();
  packetLocker->lock();
  m_packetProvider->waitUntilFullyStarted_lockfree();
  packetLocker->unlock();
  while(!isInterruptionRequested())
  {
    int nEOF = 0;
    packetLocker->lock();
    {
      auto end = m_streamDict.end();
      for(auto it = m_streamDict.begin(); it != end; ++it)
      {
        int iStream = it.key();
        AVCodecContext *pCodecCtx = it.value();

        while(true)
        {
          AVPacket *packet = m_packetProvider->getPacket_lockfree(iStream);
          int sendPacketResult = avcodec_send_packet(pCodecCtx, packet);

          if(!packet) // meet eof
          {
            ++nEOF;
            break;
          }
          else if(sendPacketResult == AVERROR(EAGAIN))
          {
            m_packetProvider->returnPacket_lockfree(iStream, packet);
            break;
          }
          else
          {
            av_packet_unref(packet);
            av_packet_free(&packet);
            if(sendPacketResult == AVERROR_EOF)
              qWarning("Stream %d EOF too early", iStream);
            else if(sendPacketResult < 0)
              CHECK_AVRESULT(sendPacketResult, false);
          }
        }
      }
    }
    m_packetProvider->requestWakeUp_lockfree();
    packetLocker->unlock();
    if(nEOF == m_streamDict.size())
      break;

    m_fullyStarted = true;
    m_syncer.wakeAll();
    m_syncer.wait(&m_locker);
  }

  m_fullyStarted = true;
  m_syncer.wakeAll();
  m_locker.unlock();
  exit();
}
