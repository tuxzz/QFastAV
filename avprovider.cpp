#include "avprovider.hpp"
#include "avframeprovider.hpp"
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QVarLengthArray>

struct Ticket
{
  QString path;
  AVFrameProvider* provider;
};

class TicketProvider final : public QThread
{
public:
  TicketProvider(bool enableAudio, bool enableVideo, QObject *parent = nullptr) : QThread(parent)
  {
    m_enableAudio = enableAudio;
    m_enableVideo = enableVideo;
  }

  ~TicketProvider()
  {
    requestInterruption();
    m_syncer.wakeAll();
    wait();
    for(Ticket *ticket:m_ticketQueue)
    {
      if(ticket->provider)
        delete ticket->provider;
      delete ticket;
    }
  }

  Ticket *createTicket(const QString &path)
  {
    auto ticket = new Ticket;
    ticket->path = path;
    ticket->provider = nullptr;

    m_locker.lock();
    m_ticketQueue.append(ticket);
    m_locker.unlock();
    m_syncer.wakeAll();

    return ticket;
  }

  void ensureTicket(Ticket *ticket)
  {
    volatile AVFrameProvider *provider = ticket->provider;
    m_locker.lock();
    while(!provider)
    {
      m_syncer.wakeAll();
      m_syncer.wait(&m_locker);
      provider = ticket->provider;
    }
    m_locker.unlock();
    Q_ASSERT(ticket->provider);
  }

  void requestStop(bool async = true)
  {
    requestInterruption();
    m_syncer.wakeAll();
    if(!async)
      wait();
  }

protected:
  void run() override
  {
    m_locker.lock();
    while(!isInterruptionRequested())
    {
      for(Ticket *ticket:m_ticketQueue)
      {
        Q_ASSERT(!ticket->provider);
        ticket->provider = new AVFrameProvider(ticket->path, m_enableAudio, m_enableVideo);
        ticket->provider->startDecoder(true);
        m_syncer.wakeAll();
      }
      m_ticketQueue.clear();

      m_syncer.wakeAll();
      m_syncer.wait(&m_locker);
    }
    m_locker.unlock();
  }

private:
  bool m_enableAudio, m_enableVideo;
  QVarLengthArray<Ticket*, 128> m_ticketQueue;

  QMutex m_locker;
  QWaitCondition m_syncer;
};

class TicketDeleter final : public QThread
{
public:
  TicketDeleter(TicketProvider *provider, QObject *parent = nullptr) : QThread(parent)
  {
    m_provider = provider;
  }

  ~TicketDeleter()
  {
    requestStop(false);
    for(Ticket *ticket:m_workQueue)
    {
      m_provider->ensureTicket(ticket);
      if(ticket->provider)
        delete ticket->provider;
      delete ticket;
    }
    for(Ticket *ticket:m_mainQueue)
    {
      m_provider->ensureTicket(ticket);
      if(ticket->provider)
        delete ticket->provider;
      delete ticket;
    }
  }

  void requestStop(bool async = true)
  {
    requestInterruption();
    m_syncer.wakeAll();
    if(!async)
      wait();
  }

  void deleteTicket(Ticket *ticket)
  {
    m_queueLocker.lock();
    m_mainQueue.append(ticket);
    m_queueLocker.unlock();
    m_syncer.wakeAll();
  }

protected:
  void run() override
  {
    m_syncLocker.lock();
    while(!isInterruptionRequested())
    {
      m_queueLocker.lock();
      m_workQueue = m_mainQueue;
      m_mainQueue.clear();
      m_queueLocker.unlock();

      for(Ticket *ticket:m_workQueue)
      {
        m_provider->ensureTicket(ticket);
        if(ticket->provider)
          delete ticket->provider;
        delete ticket;
      }
      m_workQueue.clear();

      m_syncer.wakeAll();
      m_syncer.wait(&m_syncLocker);
    }
    m_syncLocker.unlock();
  }

private:
  TicketProvider *m_provider;
  QVarLengthArray<Ticket*, 128> m_mainQueue;
  QVarLengthArray<Ticket*, 128> m_workQueue;

  QMutex m_syncLocker, m_queueLocker;
  QWaitCondition m_syncer;
};

struct PlayQueueItem
{
  QString path;
  QQueue<Ticket*> providerQueue;
  int availableProvider;
};


AVProvider::AVProvider(bool enableVideo, bool enableAudio)
{
  Q_ASSERT(enableVideo || enableAudio);
  m_maxPreloadCount = 3;
  m_enableVideo = enableVideo;
  m_enableAudio = enableAudio;
  m_ticketProvider = new TicketProvider(enableAudio, enableVideo);
  m_ticketDeleter = new TicketDeleter(m_ticketProvider);
  m_iCurrentPlaying = 0;

  m_ticketProvider->start();
  m_ticketDeleter->start();
}

AVProvider::~AVProvider()
{
  if(m_ticketDeleter)
    delete m_ticketDeleter;
  if(m_ticketProvider)
  {
    m_ticketProvider->requestStop(false);
    delete m_ticketProvider;
  }
}

void AVProvider::addToPlayQueue(const QString &path)
{ insertToPlayQueue(m_playQueue.size(), path); }

void AVProvider::insertToPlayQueue(int before, const QString &path)
{
  PlayQueueItem *item = new PlayQueueItem;
  item->path = path;
  item->availableProvider = 0;
  m_playQueue.insert(before, item);
  _preload();
}

void AVProvider::deleteFromPlayQueue(int i)
{
  PlayQueueItem *item = m_playQueue.at(i);

  for(auto ticket:item->providerQueue)
  {
    m_ticketProvider->ensureTicket(ticket);
    ticket->provider->stopDecoder(true);
  }
  for(auto ticket:item->providerQueue)
    m_ticketDeleter->deleteTicket(ticket);
}

int AVProvider::playQueueSize() const
{ return m_playQueue.size(); }

int AVProvider::currentPlayingIndex() const
{ return m_iCurrentPlaying; }

QString AVProvider::pathAt(int i) const
{ return m_playQueue.at(i)->path; }

void AVProvider::setMaxPreloadCount(int v)
{
  if(m_maxPreloadCount != v)
  {
    m_maxPreloadCount = v;
    _preload();
  }
}

int AVProvider::maxPreloadCount() const
{ return m_maxPreloadCount; }

bool AVProvider::enableVideo() const
{ return m_enableVideo; }

bool AVProvider::enableAudio() const
{ return m_enableAudio; }

AVFrameProvider *AVProvider::currentFrameProvider()
{
  auto ticket = m_playQueue.at(m_iCurrentPlaying)->providerQueue.first();
  m_ticketProvider->ensureTicket(ticket);
  return ticket->provider;
}

bool AVProvider::nextFrame()
{
  int result = currentFrameProvider()->nextFrame();
  if(!result)
  {
    auto ticket = m_playQueue.at(m_iCurrentPlaying)->providerQueue.dequeue();
    m_ticketDeleter->deleteTicket(ticket);
    m_iCurrentPlaying = (m_iCurrentPlaying + 1) % m_playQueue.size();

    _preload();
  }
  return result;
}

void AVProvider::_preload()
{
  int preloaded = 0;

  // clean flags
  for(PlayQueueItem *item:m_playQueue)
    item->availableProvider = 0;

  // keep provider
  {
    int i = m_iCurrentPlaying;
    while(preloaded < m_maxPreloadCount)
    {
      PlayQueueItem *item = m_playQueue.at(i);

      if(item->providerQueue.size() < ++item->availableProvider)
        item->providerQueue.enqueue(m_ticketProvider->createTicket(item->path));

      ++preloaded;
      i = (i + 1) % m_playQueue.size();
    }
  }

  // request stop unused
  for(PlayQueueItem *item:m_playQueue)
  {
    auto end = item->providerQueue.end();
    for(auto it = item->providerQueue.begin() + item->availableProvider; it < end; ++it)
    {
      auto ticket = *it;
      m_ticketProvider->ensureTicket(ticket);
      ticket->provider->stopDecoder(true);
    }
  }

  // clean used
  for(PlayQueueItem *item:m_playQueue)
  {
    auto begin = item->providerQueue.begin() + item->availableProvider;
    auto end = item->providerQueue.end();
    if(begin < end)
    {
      for(auto it = begin; it < end; ++it)
      {
        auto ticket = *it;
        m_ticketDeleter->deleteTicket(ticket);
      }
      item->providerQueue.erase(begin, end);
    }
  }
}
