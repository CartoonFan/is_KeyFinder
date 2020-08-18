/*************************************************************************

  Copyright 2011-2015 Ibrahim Sha'ath

  This file is part of KeyFinder.

  KeyFinder is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  KeyFinder is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with KeyFinder.  If not, see <http://www.gnu.org/licenses/>.

*************************************************************************/

#ifndef LIBAVDECODER_H
#define LIBAVDECODER_H

#include <iomanip>

#include <QFile>
#include <QMutex>
#include <QString>

#include "keyfinder/audiodata.h"
#include "keyfinder/exception.h"

#include "strings.h"

#ifndef INT64_C
#define UINT64_C(c) (c##ULL)
#endif
constexpr int INBUF_SIZE = 4096;
constexpr int AUDIO_INBUF_SIZE = 20480;
constexpr int AUDIO_REFILL_THRESH = 4096;
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#ifdef Q_OS_WIN
#include "os_windows.h"
#endif

class AudioFileDecoder {
public:
  AudioFileDecoder(const QString &, int);
  ~AudioFileDecoder();
  auto decodeNextAudioPacket() -> KeyFinder::AudioData *;

private:
  void free();
  char *filePathCh;
  uint8_t *frameBuffer;
  uint8_t *frameBufferConverted;
  int frameBufferSize;
  int audioStream;
  int badPacketCount;
  int badPacketThreshold;
  AVCodec *codec;
  AVFormatContext *fCtx;
  AVCodecContext *cCtx;
  AVDictionary *dict; // stays NULL, just here for legibility
  SwrContext *rsCtx;
  auto decodePacket(AVPacket *, KeyFinder::AudioData *) -> bool;
};

#endif
