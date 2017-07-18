#include "avpacketprovider.hpp"
#include "privateutil.hpp"
#include <limits>

AVPacketProvider::AVPacketProvider(AVFormatContext *pFormatCtx, const AVPacketProvider::StreamSet &streamIndexSet, QObject *parent) : QThread(parent)
{
  Q_ASSERT(pFormatCtx);
  Q_ASSERT(!streamIndexSet.isEmpty());
  m_pFormatCtx = pFormatCtx;
  m_queueSize = 32;

  for(int iStream:streamIndexSet)
  {
    Q_ASSERT(iStream >= 0 && iStream < static_cast<int>(pFormatCtx->nb_streams));
    PacketQueue *queue = new PacketQueue();
    m_streamQueueDict.insert(iStream, queue);
  }
  m_fullyStarted = false;
}

AVPacketProvider::~AVPacketProvider()
{
  m_locker.lock();
  for(PacketQueue *queue:m_streamQueueDict)
  {
    for(AVPacket *packet:*queue)
    {
      av_packet_unref(packet);
      av_packet_free(&packet);
    }
    delete queue;
  }
  m_streamQueueDict.clear();
  m_locker.unlock();
}

QMutex *AVPacketProvider::locker()
{ return &m_locker; }

QWaitCondition *AVPacketProvider::syncer()
{ return &m_syncer; }

void AVPacketProvider::setQueueSize_lockfree(int v)
{
  Q_ASSERT(v > 0);
  m_queueSize = v;
}

int AVPacketProvider::queueSize_lockfree() const
{ return m_queueSize; }

void AVPacketProvider::requestWakeUp_lockfree()
{ m_syncer.wakeAll(); }

AVPacket *AVPacketProvider::getPacket_lockfree(int iStream)
{
  PacketQueue *queue = m_streamQueueDict.value(iStream, nullptr);
  Q_ASSERT(queue);

  if(queue->isEmpty())
  {
    if(isRunning())
    {
      m_syncer.wakeAll();
      m_syncer.wait(&m_locker);
    }
    else
      return nullptr;
  }
  if(queue->isEmpty())
    return nullptr;
  else
    return queue->dequeue();
}

void AVPacketProvider::returnPacket_lockfree(int iStream, AVPacket *packet)
{
  PacketQueue *queue = m_streamQueueDict.value(iStream, nullptr);
  Q_ASSERT(queue);

  queue->insert(0, packet);
}

void AVPacketProvider::clearQueue_lockfree()
{
  for(PacketQueue *queue:m_streamQueueDict)
  {
    for(AVPacket *packet:*queue)
    {
      av_packet_unref(packet);
      av_packet_free(&packet);
    }
    queue->clear();
  }
}

void AVPacketProvider::requestStart()
{
  Q_ASSERT(!isRunning());
  m_fullyStarted = false;
  start();
}

void AVPacketProvider::waitUntilFullyStarted_lockfree()
{
  if(!m_fullyStarted && isRunning())
    m_syncer.wait(&m_locker);
}

void AVPacketProvider::run()
{
  m_locker.lock();
  m_fullyStarted = true;
  while(!isInterruptionRequested())
  {
    int minPacketQueueSize = std::numeric_limits<int>::max();
    auto calcMinPacketQueueSize = [&](){
      minPacketQueueSize = std::numeric_limits<int>::max();
      for(PacketQueue *queue:m_streamQueueDict)
      {
        int queueSize = queue->size();
        if(queueSize < minPacketQueueSize)
          minPacketQueueSize = queueSize;
      }
    };
    calcMinPacketQueueSize();

    while(minPacketQueueSize < m_queueSize)
    {
      AVPacket *packet = av_packet_alloc();
      av_init_packet(packet);

      // demux and enqueue packet
      {
        int packetReadingResult = av_read_frame(m_pFormatCtx, packet);
        int iStream = packet->stream_index;
        if(packetReadingResult < 0 || !m_streamQueueDict.contains(iStream))
        {
          av_packet_unref(packet);
          av_packet_free(&packet);
          if(packetReadingResult == AVERROR_EOF)
            goto cleanUp;
          else if(packetReadingResult < 0)
            CHECK_AVRESULT(packetReadingResult, false);
        }
        else
        {
          PacketQueue *queue = m_streamQueueDict.value(iStream, nullptr);
          if(queue)
            queue->enqueue(packet);
          else
          {
            av_packet_unref(packet);
            av_packet_free(&packet);
          }
        }
      }
      calcMinPacketQueueSize();
    }

    m_syncer.wakeAll();
    m_syncer.wait(&m_locker);
  }
cleanUp:
  m_syncer.wakeAll();
  m_locker.unlock();
  exit();
}
