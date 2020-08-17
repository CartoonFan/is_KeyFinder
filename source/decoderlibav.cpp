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

#include "decoderlibav.h"

QMutex codecMutex;
const int max_bad_packets = 100;

AudioFileDecoder::AudioFileDecoder(const QString& filePath, const int maxDuration) : filePathCh(nullptr), frameBufferSize(((AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2) * sizeof(uint8_t)), audioStream(-1), badPacketCount(0), badPacketThreshold(max_bad_packets), codec(nullptr), fCtx(nullptr), cCtx(nullptr), dict(nullptr), rsCtx(nullptr) {
  // convert filepath
#ifdef Q_OS_WIN
  const wchar_t* filePathWc = reinterpret_cast<const wchar_t*>(filePath.constData());
  filePathCh = qstrdup(utf16_to_utf8(filePathWc));
#else
  QByteArray encodedPath = QFile::encodeName(filePath);
  filePathCh = qstrdup(encodedPath.constData());
#endif

  frameBuffer = static_cast<uint8_t*>(av_malloc(frameBufferSize));
  frameBufferConverted = static_cast<uint8_t*>(av_malloc(frameBufferSize));

  QMutexLocker codecMutexLocker(&codecMutex); // mutex the libAV preparation

  // open file
  int openInputResult = avformat_open_input(&fCtx, filePathCh, nullptr, nullptr);
  if (openInputResult != 0) {
    qWarning("Could not open file %s (%d)", filePathCh, openInputResult);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotOpenFile(openInputResult).toUtf8().constData());
  }

  if (avformat_find_stream_info(fCtx, nullptr) < 0) {
    qWarning("Could not find stream information for file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotFindStreamInformation().toUtf8().constData());
  }

  for (int i=0; i<static_cast<signed>(fCtx->nb_streams); i++) {
    if (fCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
      audioStream = i;
      break;
    }
  }

  if (audioStream == -1) {
    qWarning("Could not find an audio stream for file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotFindAudioStream().toUtf8().constData());
  }

  // Determine duration
  const int seconds_in_minute = 60;
  const int twelve_hour_limit = 720;
  int durationSeconds = fCtx->duration / AV_TIME_BASE;
  int durationMinutes = durationSeconds / seconds_in_minute;
  // First condition is a hack for bizarre overestimation of some MP3s
  if (durationMinutes < twelve_hour_limit && durationSeconds > maxDuration * seconds_in_minute) {
    qWarning("Duration of file %s (%d:%d) exceeds specified maximum (%d:00)", filePathCh, durationMinutes, durationSeconds % seconds_in_minute, maxDuration);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->durationExceedsPreference(durationMinutes, durationSeconds % seconds_in_minute, maxDuration).toUtf8().constData());
  }

  // Determine stream codec
  cCtx = fCtx->streams[audioStream]->codec;
  codec = avcodec_find_decoder(cCtx->codec_id);
  if (codec == nullptr) {
    qWarning("Audio stream has unsupported codec in file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavUnsupportedCodec().toUtf8().constData());
  }

  // Open codec
  int codecOpenResult = avcodec_open2(cCtx, codec, &dict);
  if (codecOpenResult < 0) {
    qWarning("Could not open audio codec %s (%d) for file %s", codec->long_name, codecOpenResult, filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotOpenCodec(codec->long_name, codecOpenResult).toUtf8().constData());
  }

  rsCtx = av_audio_resample_init(cCtx->channels, cCtx->channels, cCtx->sample_rate, cCtx->sample_rate, AV_SAMPLE_FMT_S16, cCtx->sample_fmt, 0, 0, 0, 0);
  if (rsCtx == nullptr) {
    qWarning("Could not create ReSampleContext for file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotCreateResampleContext().toUtf8().constData());
  }

  qDebug("Decoder prepared for %s (%s, %d)", filePathCh, av_get_sample_fmt_name(cCtx->sample_fmt), cCtx->sample_rate);
}

void AudioFileDecoder::free() {
  av_free(frameBuffer);
  av_free(frameBufferConverted);
  if (rsCtx != nullptr) audio_resample_close(rsCtx);
  if (cCtx != nullptr) {
    int codecCloseResult = avcodec_close(cCtx);
    if (codecCloseResult < 0) {
      qCritical("Error closing audio codec: %s (%d)", codec->long_name, codecCloseResult);
    }
  }
  if (fCtx != nullptr) av_close_input_file(fCtx);
  delete[] filePathCh; 
}

AudioFileDecoder::~AudioFileDecoder() {
  QMutexLocker codecMutexLocker(&codecMutex);
  free();
}

auto AudioFileDecoder::decodeNextAudioPacket() -> KeyFinder::AudioData* {
  // Prep buffer
  KeyFinder::AudioData* audio = nullptr;
  // Decode stream
  AVPacket avpkt;
  do {
    av_init_packet(&avpkt);
    if (av_read_frame(fCtx, &avpkt) < 0) { return audio; }
    if (avpkt.stream_index != audioStream) { av_free_packet(&avpkt); }
  } while (avpkt.data == nullptr);
  try {
    audio = new KeyFinder::AudioData();
    audio->setFrameRate(static_cast<unsigned int>(cCtx->sample_rate));
    audio->setChannels(cCtx->channels);
    if (!decodePacket(&avpkt, audio)) {
      badPacketCount++;
      if (badPacketCount > badPacketThreshold) {
        av_free_packet(&avpkt);
        qWarning("Too many bad packets (%d) while decoding file %s", badPacketCount, filePathCh);
        throw KeyFinder::Exception(GuiStrings::getInstance()->libavTooManyBadPackets(badPacketThreshold).toUtf8().constData());
      }
    }
  } catch (KeyFinder::Exception& e) {
    av_free_packet(&avpkt);
    qWarning("Encountered KeyFinder::Exception (%s) while decoding file %s", e.what(), filePathCh);
    throw e;
  }
  av_free_packet(&avpkt);
  return audio;
}

auto AudioFileDecoder::decodePacket(AVPacket* originalPacket, KeyFinder::AudioData* audio) -> bool {
  // copy packet so we can shift data pointer about without endangering garbage collection
  AVPacket tempPacket;
  tempPacket.size = originalPacket->size;
  tempPacket.data = originalPacket->data;
  // loop in case audio packet contains multiple frames
  while (tempPacket.size > 0) {
    int dataSize = frameBufferSize;
    int16_t* dataBuffer = reinterpret_cast<int16_t*>(frameBuffer);
    int bytesConsumed = 0;
    bytesConsumed = avcodec_decode_audio4(cCtx, dataBuffer, &dataSize, &tempPacket);
    if (bytesConsumed < 0) { // error
      tempPacket.size = 0;
      return false;
    }
    tempPacket.data += bytesConsumed;
    tempPacket.size -= bytesConsumed;
    if (dataSize <= 0) { { continue; // nothing decoded
}
}
    int newSamplesDecoded = dataSize / av_get_bytes_per_sample(cCtx->sample_fmt);
    // Resample if necessary
    if (cCtx->sample_fmt != AV_SAMPLE_FMT_S16) {
      int resampleResult = 0;
      resampleResult = audio_resample(rsCtx, (short*)frameBufferConverted, (short*)frameBuffer, newSamplesDecoded);
      if (resampleResult < 0) {
        throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotResample().toUtf8().constData());
      }
      dataBuffer = reinterpret_cast<int16_t*>(frameBufferConverted);
    }
    int oldSampleCount = audio->getSampleCount();
    audio->addToSampleCount(newSamplesDecoded);
    audio->resetIterators();
    audio->advanceWriteIterator(oldSampleCount);
    for (int i = 0; i < newSamplesDecoded; i++) {
      audio->setSampleAtWriteIterator(static_cast<double>(dataBuffer[i]));
      audio->advanceWriteIterator();
    }
  }
  return true;
}
