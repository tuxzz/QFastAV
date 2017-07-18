#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QHash>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class AVPacketProvider;

class AVPacketDecoder final : public QThread
{
  Q_OBJECT

private:
  typedef QHash<int, AVCodecContext*> StreamDict;
public:
  typedef QSet<int> StreamSet;

  AVPacketDecoder(AVPacketProvider *packetProvider, AVFormatContext *pFormatCtx, const StreamSet &streamSet, QObject *parent = nullptr);
  ~AVPacketDecoder();

  QMutex *locker();
  QWaitCondition *syncer();

  void requestFeeding_lockfree();
  void requestWakeUp_lockfree();

  bool getFrame(int iStream, AVFrame *pOut);
  void waitUntilFullyStarted_lockfree();
  void requestStart();

protected:
  void run() override;

private:
  AVPacketProvider *m_packetProvider;
  StreamDict m_streamDict;
  bool m_fullyStarted;

  QMutex m_locker;
  QWaitCondition m_syncer;
};
