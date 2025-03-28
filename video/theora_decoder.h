/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/scummsys.h"	// for USE_THEORADEC

#ifdef USE_THEORADEC

#ifndef VIDEO_THEORA_DECODER_H
#define VIDEO_THEORA_DECODER_H

#include "common/rational.h"
#include "video/video_decoder.h"
#include "audio/mixer.h"
#include "graphics/surface.h"

#include <theora/theoradec.h>

#ifdef USE_TREMOR
#include <tremor/ivorbiscodec.h>
#else
#include <vorbis/codec.h>
#endif

namespace Common {
class SeekableReadStream;
}

namespace Audio {
class AudioStream;
class QueuingAudioStream;
}

namespace Video {

/**
 *
 * Decoder for Theora videos.
 * Video decoder used in engines:
 *  - pegasus
 *  - sword25
 *  - wintermute
 */
class TheoraDecoder : public VideoDecoder {
public:
	TheoraDecoder();
	virtual ~TheoraDecoder();

	/**
	 * Load a video file
	 * @param stream  the stream to load
	 */
	bool loadStream(Common::SeekableReadStream *stream);
	void close();

	/** Frames per second of the loaded video. */
	Common::Rational getFrameRate() const;

protected:
	void readNextPacket();

private:
	class TheoraVideoTrack : public VideoTrack {
	public:
		TheoraVideoTrack(th_info &theoraInfo, th_setup_info *theoraSetup);
		~TheoraVideoTrack();

		bool endOfTrack() const { return _endOfVideo; }
		uint16 getWidth() const { return _width; }
		uint16 getHeight() const { return _height; }
		Graphics::PixelFormat getPixelFormat() const { return _pixelFormat; }
		bool setOutputPixelFormat(const Graphics::PixelFormat &format) {
			if (format.bytesPerPixel != 2 && format.bytesPerPixel != 4)
				return false;
			_pixelFormat = format;
			return true;
		}

		int getCurFrame() const { return _curFrame; }
		const Common::Rational &getFrameRate() const { return _frameRate; }
		uint32 getNextFrameStartTime() const { return (uint32)(_nextFrameStartTime * 1000); }
		const Graphics::Surface *decodeNextFrame() { return _displaySurface; }

		bool decodePacket(ogg_packet &oggPacket);
		void setEndOfVideo() { _endOfVideo = true; }

	private:
		int _curFrame;
		bool _endOfVideo;
		Common::Rational _frameRate;
		double _nextFrameStartTime;

		Graphics::Surface *_surface;
		Graphics::Surface *_displaySurface;
		Graphics::PixelFormat _pixelFormat;
		int _x;
		int _y;
		uint16 _width;
		uint16 _height;
		uint16 _surfaceWidth;
		uint16 _surfaceHeight;

		th_dec_ctx *_theoraDecode;
		th_pixel_fmt _theoraPixelFormat;

		void translateYUVtoRGBA(th_ycbcr_buffer &YUVBuffer);
	};

	class VorbisAudioTrack : public AudioTrack {
	public:
		VorbisAudioTrack(Audio::Mixer::SoundType soundType, vorbis_info &vorbisInfo);
		~VorbisAudioTrack();

		bool decodeSamples();
		bool hasAudio() const;
		bool needsAudio() const;
		void synthesizePacket(ogg_packet &oggPacket);
		void setEndOfAudio() { _endOfAudio = true; }

	protected:
		Audio::AudioStream *getAudioStream() const;

	private:
		// single audio fragment audio buffering
		int _audioBufferFill;
		ogg_int16_t *_audioBuffer;

		Audio::QueuingAudioStream *_audStream;

		vorbis_block _vorbisBlock;
		vorbis_dsp_state _vorbisDSP;

		bool _endOfAudio;
	};

	void queuePage(ogg_page *page);
	int bufferData();
	bool queueAudio();
	void ensureAudioBufferSize();

	Common::SeekableReadStream *_fileStream;

	ogg_sync_state _oggSync;
	ogg_page _oggPage;
	ogg_packet _oggPacket;

	ogg_stream_state _theoraOut, _vorbisOut;
	bool _hasVideo, _hasAudio;

	vorbis_info _vorbisInfo;

	TheoraVideoTrack *_videoTrack;
	VorbisAudioTrack *_audioTrack;
};

} // End of namespace Video

#endif

#endif
