#include "avseeker.hpp"
#include "privateutil.hpp"

AVSeeker::AVSeeker(AVFormatContext *pFormatCtx, QObject *parent) : QThread(parent)
{
  Q_ASSERT(pFormatCtx);
  m_pFormatCtx = pFormatCtx;
  m_pos = 0;
}

AVSeeker::~AVSeeker()
{}

void AVSeeker::setPos_lockfree(qint64 v)
{ m_pos = v; }

qint64 AVSeeker::pos_lockfree() const
{ return m_pos; }

void AVSeeker::run()
{
  int seekResult = av_seek_frame(m_pFormatCtx, -1, m_pos, AVSEEK_FLAG_BACKWARD);
  CHECK_AVRESULT(seekResult, seekResult >= 0);
  exit();
}
