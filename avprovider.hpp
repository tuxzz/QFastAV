#pragma once

#include <QQueue>
#include <QString>

class AVFrameProvider;
class TicketProvider;
class TicketDeleter;
struct PlayQueueItem;

class AVProvider
{
public:
  AVProvider(bool enableVideo, bool enableAudio);
  ~AVProvider();

  void addToPlayQueue(const QString &path);
  void insertToPlayQueue(int before, const QString &path);
  void deleteFromPlayQueue(int i);
  int playQueueSize() const;
  int currentPlayingIndex() const;
  QString pathAt(int i) const;

  void setMaxPreloadCount(int v);
  int maxPreloadCount() const;

  bool enableVideo() const;
  bool enableAudio() const;

  AVFrameProvider *currentFrameProvider();
  bool nextFrame();

private:
  void _preload();

  int m_maxPreloadCount;
  bool m_enableVideo, m_enableAudio;

  QQueue<PlayQueueItem *> m_playQueue;

  TicketProvider *m_ticketProvider;
  TicketDeleter *m_ticketDeleter;
  int m_iCurrentPlaying;
};
