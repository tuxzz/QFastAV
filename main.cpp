#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QElapsedTimer>
#include <QThread>
#include "avframeprovider.hpp"
#include "avprovider.hpp"
extern "C"
{
  #include <libavutil/avutil.h>
  #include <libavformat/avformat.h>
  #include <libavcodec/avcodec.h>
}

int main(int argc, char *argv[])
{
  av_register_all();
  QGuiApplication app(argc, argv);

  AVProvider provider(true, true);
  provider.addToPlayQueue("D:/muz/muz0/例大祭11 Rebirth Story Ⅱ/Disc 1/05.Once Upon a Love.flac");
  provider.addToPlayQueue("D:/muz/muz0/センスレス·ワンダー/センスレス·ワンダー.flac");
  QElapsedTimer t;
  double peak = 0.0;
  double peakDur = 1.0;

  while(true)
  {
    t.start();
    double dur = provider.currentFrameProvider()->duration();
    while(provider.nextFrame())
      (void)0;
    double e = static_cast<double>(t.nsecsElapsed()) / 1e9;
    if(e > peak)
    {
      peakDur = dur;
      peak = e;
    }
    qDebug("Instant:%lf(%lfx) Peak:%lf(%lfx))", e, dur / e , peak, peakDur / peak);
  }
  return 0;
}
