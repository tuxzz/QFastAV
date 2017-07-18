#pragma once

#include <QThread>
#include <atomic>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
}

class AVSeeker final : public QThread
{
  Q_OBJECT
public:
  AVSeeker(AVFormatContext *pFormatCtx, QObject *parent = nullptr);
  ~AVSeeker();

  void setPos_lockfree(qint64 v);
  qint64 pos_lockfree() const;

protected:
  void run() override;

private:
  AVFormatContext *m_pFormatCtx;
  qint64 m_pos;
};
