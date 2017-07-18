#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QSet>
#include <QHash>
#include <QQueue>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
}

class AVPacketProvider final : public QThread
{
  Q_OBJECT

private:
  typedef QQueue<AVPacket*> PacketQueue;
  typedef QHash<int, PacketQueue*> StreamDict;
public:
  typedef QSet<int> StreamSet;

  AVPacketProvider(AVFormatContext *pFormatCtx, const StreamSet &streamIndexSet, QObject *parent = nullptr);
  ~AVPacketProvider();

  QMutex *locker();
  QWaitCondition *syncer();

  void setQueueSize_lockfree(int v);
  int queueSize_lockfree() const;

  void requestWakeUp_lockfree();
  AVPacket *getPacket_lockfree(int iStream);
  void returnPacket_lockfree(int iStream, AVPacket *packet);
  void clearQueue_lockfree();

  void requestStart();
  void waitUntilFullyStarted_lockfree();

protected:
  void run() override;

private:
  AVFormatContext *m_pFormatCtx;

  StreamDict m_streamQueueDict;
  int m_queueSize;

  bool m_fullyStarted;

  QMutex m_locker;
  QWaitCondition m_syncer;
};
