#include "global.h"
#include "MovieTexture_FFMpeg.h"

#include "RageDisplay.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "RageFile.h"
#include "RageSurface.h"

#include <algorithm>
#include <cstdint>
#include <thread>

// Memory allowed for decoded frames waiting to be shown, per movie. The number
// of frames follows from the frame size, within the limits below: about 90
// frames (3 s at 30 fps) for a 640x360 movie and 30 frames for a 1080p one.
static const std::size_t kMovieQueueBytes = 96 * 1024 * 1024;
static const std::size_t kMinQueuedFrames = 8;
static const std::size_t kMaxQueuedFrames = 90;

static RString averr_ssprintf(int err, const char* fmt, ...);

static void FixLilEndian()
{
	if constexpr (!Endian::little) {
		return;
	}

	static bool Initialized = false;
	if (Initialized)
		return;
	Initialized = true;

	for (int i = 0; i < AVPixelFormats[i].bpp; ++i)
	{
		AVPixelFormat_t& pf = AVPixelFormats[i];

		if (!pf.bByteSwapOnLittleEndian)
			continue;

		for (int mask = 0; mask < 4; ++mask)
		{
			int m = pf.masks[mask];
			switch (pf.bpp)
			{
			case 24: m = Swap24(m); break;
			case 32: m = Swap32(m); break;
			default:
				FAIL_M(ssprintf("Unsupported BPP value: %i", pf.bpp));
			}
			pf.masks[mask] = m;
		}
	}
}

static int FindCompatibleAVFormat(bool bHighColor)
{
	for (int i = 0; AVPixelFormats[i].bpp; ++i)
	{
		AVPixelFormat_t& fmt = AVPixelFormats[i];
		if (fmt.YUV != PixelFormatYCbCr_Invalid)
		{
			EffectMode em = MovieTexture_Generic::GetEffectMode(fmt.YUV);
			if (!DISPLAY->IsEffectModeSupported(em))
				continue;
		}
		else if (fmt.bHighColor != bHighColor)
		{
			continue;
		}

		RagePixelFormat pixfmt = DISPLAY->FindPixelFormat(fmt.bpp,
			fmt.masks[0],
			fmt.masks[1],
			fmt.masks[2],
			fmt.masks[3],
			true /* realtime */
		);

		if (pixfmt == RagePixelFormat_Invalid)
			continue;

		return i;
	}

	return -1;
}

RageSurface* RageMovieTextureDriver_FFMpeg::AVCodecCreateCompatibleSurface(int iTextureWidth, int iTextureHeight, bool bPreferHighColor, int& iAVTexfmt, MovieDecoderPixelFormatYCbCr& fmtout)
{
	FixLilEndian();

	int iAVTexfmtIndex = FindCompatibleAVFormat(bPreferHighColor);
	if (iAVTexfmtIndex == -1)
		iAVTexfmtIndex = FindCompatibleAVFormat(!bPreferHighColor);

	if (iAVTexfmtIndex == -1)
	{
		/* No dice.  Use the first avcodec format of the preferred bit depth,
		 * and let the display system convert. */
		for (iAVTexfmtIndex = 0; AVPixelFormats[iAVTexfmtIndex].bpp; ++iAVTexfmtIndex)
			if (AVPixelFormats[iAVTexfmtIndex].bHighColor == bPreferHighColor)
				break;
		ASSERT(AVPixelFormats[iAVTexfmtIndex].bpp != 0);
	}

	const AVPixelFormat_t* pfd = &AVPixelFormats[iAVTexfmtIndex];
	iAVTexfmt = pfd->pf;
	fmtout = pfd->YUV;

	LOG->Trace("Texture pixel format: %i %i (%ibpp, %08x %08x %08x %08x)", iAVTexfmt, fmtout,
		pfd->bpp, pfd->masks[0], pfd->masks[1], pfd->masks[2], pfd->masks[3]);

	if (pfd->YUV == PixelFormatYCbCr_YUYV422)
		iTextureWidth /= 2;

	return CreateSurface(iTextureWidth, iTextureHeight, pfd->bpp,
		pfd->masks[0], pfd->masks[1], pfd->masks[2], pfd->masks[3]);
}

MovieDecoder_FFMpeg::MovieDecoder_FFMpeg()
{
	FixLilEndian();

	av_format_context_ = nullptr;
	av_stream_ = nullptr;
	current_packet_offset_ = -1;

	Init();
}

MovieDecoder_FFMpeg::~MovieDecoder_FFMpeg()
{
	if (av_sws_context_)
	{
		avcodec::sws_freeContext(av_sws_context_);
		av_sws_context_ = nullptr;
	}
	if (av_io_context_ != nullptr)
	{
		RageFile* file = (RageFile*)av_io_context_->opaque;
		file->Close();
		delete file;
		avcodec::av_free(av_io_context_);
	}
	if (av_buffer_ != nullptr)
	{
		avcodec::av_free(av_buffer_);
	}
	if (av_stream_codec_ != nullptr)
	{
		avcodec::avcodec_free_context(&av_stream_codec_);
	}
	frame_queue_.clear();
}

void MovieDecoder_FFMpeg::Init()
{
	end_of_file_ = 0;
	total_frames_ = 0;
	av_sws_context_ = nullptr;
	av_io_context_ = nullptr;
	av_buffer_ = nullptr;
	max_queued_frames_ = kMinQueuedFrames;
	last_timestamp_ = 0;
	last_frame_delay_ = 0;
	first_frame_ = true;

	// Init() runs before the decoding thread starts and after it has stopped.
	std::lock_guard<std::mutex> lock(queue_mutex_);
	frame_queue_.clear();
	reached_end_ = false;
	rewind_pending_ = false;
	frames_shown_ = 0;
}

float MovieDecoder_FFMpeg::GetTimestamp() const
{
	// Always display the first frame.
	if (frames_shown_ == 0) {
		return 0;
	}

	std::lock_guard<std::mutex> lock(queue_mutex_);
	// Display is outpacing decoding.
	if (frame_queue_.empty()) {
		return 0;
	}
	return frame_queue_.front()->frame_timestamp;
}

bool MovieDecoder_FFMpeg::IsCurrentFrameReady()
{
	std::lock_guard<std::mutex> lock(queue_mutex_);
	// We're displaying faster than decoding, or the movie has ended.
	if (frame_queue_.empty()) {
		return false;
	}
	if (frame_queue_.front()->skip || frames_shown_ == 0) {
		return true;
	}
	// Only show a frame once it is known whether it is the last one, so that
	// GetFrame() can tell the movie texture to loop.
	return frame_queue_.size() >= 2 || reached_end_;
}

int MovieDecoder_FFMpeg::DecodeNextFrame()
{
	auto holder = std::make_unique<FrameHolder>();
	int status = SendPacketToBuffer(*holder);
	if (status < 0) {
		return status;
	}
	if (end_of_file_) {
		// Don't queue an EoF frame.
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if (decode_generation_ == generation_) {
			reached_end_ = true;
		}
		return 0;
	}

	status = DecodePacketInBuffer(*holder);
	if (first_frame_) {
		first_frame_ = false;
	}
	if (status < 0) {
		return status;
	}

	// The compressed data is not needed any more; only the picture is kept until it is shown.
	avcodec::av_packet_unref(holder->packet);

	std::lock_guard<std::mutex> lock(queue_mutex_);
	// A rewind while this frame was being decoded makes it stale.
	if (decode_generation_ == generation_) {
		frame_queue_.push_back(std::move(holder));
	}
	return status;
}

int MovieDecoder_FFMpeg::DecodeMovie()
{
	// The first frame was decoded and drawn already, by MovieTexture_Generic::Init().
	while (true)
	{
		bool rewind = false;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			// Sleep while enough frames are waiting to be shown, or after the last frame.
			queue_cv_.wait(lock, [this]() {
				return cancel_ || rewind_pending_ ||
					(!reached_end_ && frame_queue_.size() < max_queued_frames_);
			});
			if (cancel_) {
				return -2;
			}
			if (rewind_pending_) {
				rewind_pending_ = false;
				rewind = true;
			}
			decode_generation_ = generation_;
		}

		if (rewind && !SeekToStart())
		{
			// Keep showing the last frame instead of looping.
			std::lock_guard<std::mutex> lock(queue_mutex_);
			if (decode_generation_ == generation_) {
				reached_end_ = true;
			}
			continue;
		}

		// If cancelled (quitting a song, scrolling the banner), or fatal error,
		// stop decoding.
		int status = DecodeNextFrame();
		if (status < 0) {
			return status;
		}
	}
}

bool MovieDecoder_FFMpeg::SeekToStart()
{
	std::int64_t start = av_stream_->start_time != AV_NOPTS_VALUE ? av_stream_->start_time : 0;
	int ret = avcodec::av_seek_frame(av_format_context_, av_stream_->index, start, AVSEEK_FLAG_BACKWARD);
	if (ret < 0) {
		// Some files can only be rewound by bytes.
		ret = avcodec::av_seek_frame(av_format_context_, av_stream_->index, 0, AVSEEK_FLAG_BYTE);
	}
	if (ret < 0) {
		LOG->Warn("%s", averr_ssprintf(ret, "Couldn't rewind the movie").c_str());
		return false;
	}

	avcodec::avcodec_flush_buffers(av_stream_codec_);
	end_of_file_ = 0;
	current_packet_offset_ = -1;
	first_frame_ = true;
	last_timestamp_ = 0;
	last_frame_delay_ = 0;
	return true;
}

int MovieDecoder_FFMpeg::SendPacketToBuffer(FrameHolder& holder)
{
	if (cancel_) {
		return -2;
	}
	if (end_of_file_ > 0) {
		return 0;
	}

	while (true)
	{
		int ret = avcodec::av_read_frame(av_format_context_, holder.packet);
		/* XXX: why is avformat returning AVERROR_NOMEM on EOF? */
		if (ret < 0)
		{
			/* EOF. */
			end_of_file_ = 1;
			return 0;
		}

		if (holder.packet->stream_index == av_stream_->index)
		{
			current_packet_offset_ = 0;
			return 1;
		}
		/* It's not for the video stream; ignore it. */
		avcodec::av_packet_unref(holder.packet);
	}
}

int MovieDecoder_FFMpeg::DecodePacketInBuffer(FrameHolder& holder) {
	if (cancel_) {
		return -2;
	}
	if (end_of_file_ == 0 && current_packet_offset_ == -1) {
		return 0; /* no packet */
	}

	while (end_of_file_ == 0 && current_packet_offset_ <= holder.packet->size)
	{
		/* If we have no data on the first frame, just return EOF; passing an empty packet
		 * to avcodec_decode_video in this case is crashing it.  However, passing an empty
		 * packet is normal with B-frames, to flush.  This may be unnecessary in newer
		 * versions of avcodec, but I'm waiting until a new stable release to upgrade. */
		if (holder.packet->size == 0 && first_frame_) {
			return 0; /* eof */
		}

		/* Hack: we need to send size = 0 to flush frames at the end, but we have
		 * to give it a buffer to read from since it tries to read anyway. */
		holder.packet->data = holder.packet->size ? holder.packet->data : nullptr;
		int len = holder.packet->size;
		avcodec::avcodec_send_packet(av_stream_codec_, holder.packet);
		int avcodec_return = avcodec::avcodec_receive_frame(av_stream_codec_, holder.frame);

		if (len < 0)
		{
			LOG->Warn("avcodec_decode_video2 fatal error, packet size negative: %i", len);
			return -1;
		}

		current_packet_offset_ += len;

		if (avcodec_return != 0)
		{
			LOG->Warn(
				"Frame not successfully decoded into buffer. avcodec_receive_frame status: %i",
				avcodec_return);
			continue;
		}

		if (holder.frame->pkt_dts != AV_NOPTS_VALUE)
		{
			holder.frame_timestamp = (float)(holder.frame->pkt_dts * av_q2d(av_stream_->time_base));
		}
		else
		{
			/* If the timestamp is zero, this frame is to be played at the
			 * time of the last frame plus the length of the last frame. */
			if (!first_frame_) {
				holder.frame_timestamp = last_timestamp_ + last_frame_delay_;
			}
			else {
				holder.frame_timestamp = 0;
			}
		}

		// Length of this frame, only used as a fallback for getting the frame
		// timestamp above.
		holder.frame_delay = (float)av_q2d(av_stream_->time_base);
		holder.frame_delay += holder.frame->repeat_pict * (holder.frame_delay * 0.5f);
		holder.decoded = true;

		last_timestamp_ = holder.frame_timestamp;
		last_frame_delay_ = holder.frame_delay;
		return 1;
	}

	// This if statement means the packet did not decode correctly. This is not
	// necessarily fatal for video playback, but out of caution the frame should
	// be skipped.
	if (!holder.decoded) {
		holder.skip = true;
	}

	return 0; /* packet done */
}

bool MovieDecoder_FFMpeg::SkipNextFrame() {
	std::lock_guard<std::mutex> lock(queue_mutex_);
	if (frame_queue_.empty()) {
		return true;
	}
	if (frame_queue_.front()->skip) {
		frame_queue_.pop_front();
		queue_cv_.notify_all();
		return true;
	}
	return false;
}

bool MovieDecoder_FFMpeg::GetFrame(RageSurface* pSurface)
{
	avcodec::AVFrame pict;
	pict.data[0] = (unsigned char*)pSurface->pixels;
	pict.linesize[0] = pSurface->pitch;

	/* XXX 1: Do this in one of the Open() methods instead?
	 * XXX 2: The problem of doing this in Open() is that m_AVTexfmt is not
	 * already initialized with its correct value.
	 */
	if (av_sws_context_ == nullptr)
	{
		av_sws_context_ = avcodec::sws_getCachedContext(av_sws_context_,
			GetWidth(), GetHeight(), av_stream_codec_->pix_fmt,
			GetWidth(), GetHeight(), av_pixel_format_,
			kSwsFlags, nullptr, nullptr, nullptr);
		if (av_sws_context_ == nullptr)
		{
			LOG->Warn("Cannot initialize sws conversion context for (%d,%d) %d->%d", GetWidth(), GetHeight(), av_stream_codec_->pix_fmt, av_pixel_format_);
			return false;
		}
	}

	// Take the frame out of the queue: it is freed once it has been drawn, and
	// the decoding thread may decode another one meanwhile.
	std::unique_ptr<FrameHolder> holder;
	bool last_frame = false;
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if (frame_queue_.empty()) {
			return false;
		}
		holder = std::move(frame_queue_.front());
		frame_queue_.pop_front();
		last_frame = frame_queue_.empty() && reached_end_;
		queue_cv_.notify_all();
	}

	avcodec::sws_scale(av_sws_context_,
		holder->frame->data, holder->frame->linesize, 0, GetHeight(),
		pict.data, pict.linesize);

	++frames_shown_;
	return last_frame;
}

void MovieDecoder_FFMpeg::Cancel()
{
	std::lock_guard<std::mutex> lock(queue_mutex_);
	cancel_ = true;
	queue_cv_.notify_all();
}

static RString averr_ssprintf(int err, const char* fmt, ...)
{
	ASSERT(err < 0);

	va_list     va;
	va_start(va, fmt);
	RString s = vssprintf(fmt, va);
	va_end(va);

	std::size_t errbuf_size = 512;
	char* errbuf = new char[errbuf_size];
	avcodec::av_strerror(err, errbuf, errbuf_size);
	RString Error = ssprintf("%i: %s", err, errbuf);
	delete[] errbuf;

	return s + " (" + Error + ")";
}

static int AVIORageFile_ReadPacket(void* opaque, std::uint8_t* buf, int buf_size)
{
	RageFile* f = (RageFile*)opaque;
	int n = f->Read(buf, buf_size);
	if (n == 0)
		return AVERROR_EOF;
	return n;
}

static std::int64_t AVIORageFile_Seek(void* opaque, std::int64_t offset, int whence)
{
	RageFile* f = (RageFile*)opaque;
	if (whence == AVSEEK_SIZE)
		return f->GetFileSize();

	if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
	{
		LOG->Trace("Error: unsupported seek whence: %d", whence);
		return -1;
	}

	return f->Seek((int)offset, whence);
}

RString MovieDecoder_FFMpeg::Open(RString sFile)
{
	av_format_context_ = avcodec::avformat_alloc_context();
	if (!av_format_context_)
		return "AVCodec: Couldn't allocate context";

	RageFile* f = new RageFile;

	if (!f->Open(sFile, RageFile::READ))
	{
		RString errorMessage = f->GetError();
		RString error = ssprintf("MovieDecoder_FFMpeg: Error opening \"%s\": %s", sFile.c_str(), errorMessage.c_str());
		delete f;
		return error;
	}

	av_buffer_ = (unsigned char*)avcodec::av_malloc(STEPMANIA_FFMPEG_BUFFER_SIZE);
	av_io_context_ = avcodec::avio_alloc_context(av_buffer_, STEPMANIA_FFMPEG_BUFFER_SIZE, 0, f, AVIORageFile_ReadPacket, nullptr, AVIORageFile_Seek);
	av_format_context_->pb = av_io_context_;
	int ret = avcodec::avformat_open_input(&av_format_context_, sFile.c_str(), nullptr, nullptr);
	if (ret < 0)
		return RString(averr_ssprintf(ret, "AVCodec: Couldn't open \"%s\"", sFile.c_str()));

	ret = avcodec::avformat_find_stream_info(av_format_context_, nullptr);
	if (ret < 0)
		return RString(averr_ssprintf(ret, "AVCodec (%s): Couldn't find codec parameters", sFile.c_str()));

	int stream_idx = avcodec::av_find_best_stream(av_format_context_, avcodec::AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (stream_idx < 0 ||
		static_cast<unsigned int>(stream_idx) >= av_format_context_->nb_streams ||
		av_format_context_->streams[stream_idx] == nullptr)
		return "Couldn't find any video streams";
	av_stream_ = av_format_context_->streams[stream_idx];
	av_stream_codec_ = avcodec::avcodec_alloc_context3(nullptr);
	if (avcodec::avcodec_parameters_to_context(av_stream_codec_, av_stream_->codecpar) < 0)
		return ssprintf("Could not get context from parameters");

	if (av_stream_codec_->codec_id == avcodec::AV_CODEC_ID_NONE)
		return ssprintf("Unsupported codec %08x", av_stream_codec_->codec_tag);

	RString sError = OpenCodec();
	if (!sError.empty())
		return ssprintf("AVCodec (%s): %s", sFile.c_str(), sError.c_str());

	LOG->Trace("Bitrate: %i", static_cast<int>(av_stream_codec_->bit_rate));
	LOG->Trace("Codec pixel format: %s", avcodec::av_get_pix_fmt_name(av_stream_codec_->pix_fmt));
	total_frames_ = av_stream_->nb_frames;
	if (total_frames_ <= 0) {
		// Sometimes we might not get a correct frame count.
		// In that case, approximate and fix it later.
		total_frames_ = av_format_context_->duration // microseconds
			* (av_stream_->avg_frame_rate.num) / (av_stream_->avg_frame_rate.den) / (1000000);
		LOG->Trace("Number of frames provided is inaccurate, estimating.");
	}
	LOG->Trace("Number of frames detected: %i", total_frames_);

	// How many decoded frames may wait to be shown: as many as fit in kMovieQueueBytes.
	int frame_bytes = avcodec::av_image_get_buffer_size(av_stream_codec_->pix_fmt, GetWidth(), GetHeight(), 1);
	if (frame_bytes <= 0) {
		frame_bytes = GetWidth() * GetHeight() * 4;
	}
	max_queued_frames_ = kMinQueuedFrames;
	if (frame_bytes > 0) {
		max_queued_frames_ = std::clamp(kMovieQueueBytes / static_cast<std::size_t>(frame_bytes), kMinQueuedFrames, kMaxQueuedFrames);
	}
	LOG->Trace("Frames decoded ahead: up to %i", static_cast<int>(max_queued_frames_));

	return RString();
}

RString MovieDecoder_FFMpeg::OpenCodec()
{
	Init();

	ASSERT(av_stream_ != nullptr);
	if (av_stream_codec_->codec)
		avcodec::avcodec_close(av_stream_codec_);

	const avcodec::AVCodec* pCodec = avcodec::avcodec_find_decoder(av_stream_codec_->codec_id);
	if (pCodec == nullptr)
		return ssprintf("Couldn't find decoder %i", av_stream_codec_->codec_id);

	av_stream_codec_->workaround_bugs = 1;
	av_stream_codec_->idct_algo = FF_IDCT_AUTO;
	av_stream_codec_->error_concealment = 3;

	LOG->Trace("Opening codec %s", pCodec->name);

	int ret = avcodec::avcodec_open2(av_stream_codec_, pCodec, nullptr);
	if (ret < 0)
		return RString(averr_ssprintf(ret, "Couldn't open codec \"%s\"", pCodec->name));
	ASSERT(av_stream_codec_->codec != nullptr);

	return RString();
}

void MovieDecoder_FFMpeg::Close()
{
	if (av_stream_ && av_stream_codec_->codec)
	{
		avcodec::avcodec_close(av_stream_codec_);
		av_stream_ = nullptr;
	}

	if (av_format_context_)
	{
		avcodec::avformat_close_input(&av_format_context_);
		av_format_context_ = nullptr;
	}

	Init();
}

void MovieDecoder_FFMpeg::Rewind()
{
	// The frames are not kept after they are shown, so the decoding thread
	// seeks back to the start of the file and decodes the movie again.
	std::lock_guard<std::mutex> lock(queue_mutex_);
	frame_queue_.clear();
	reached_end_ = false;
	rewind_pending_ = true;
	++generation_;
	frames_shown_ = 0;
	queue_cv_.notify_all();
}

RageSurface* MovieDecoder_FFMpeg::CreateCompatibleSurface(int iTextureWidth, int iTextureHeight, bool bPreferHighColor, MovieDecoderPixelFormatYCbCr& fmtout)
{
	return RageMovieTextureDriver_FFMpeg::AVCodecCreateCompatibleSurface(iTextureWidth, iTextureHeight, bPreferHighColor, *ConvertValue<int>(&av_pixel_format_), fmtout);
}

MovieTexture_FFMpeg::MovieTexture_FFMpeg(RageTextureID ID) :
	MovieTexture_Generic(ID, new MovieDecoder_FFMpeg)
{
}

RageMovieTexture* RageMovieTextureDriver_FFMpeg::Create(RageTextureID ID, RString& sError)
{
	MovieTexture_FFMpeg* pRet = new MovieTexture_FFMpeg(ID);
	sError = pRet->Init();
	if (!sError.empty())
		SAFE_DELETE(pRet);
	return pRet;
}

REGISTER_MOVIE_TEXTURE_CLASS(FFMpeg);

/*
 * (c) 2003-2005 Glenn Maynard
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
