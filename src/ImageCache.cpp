#include "global.h"

#include "ImageCache.h"
#include "RageDisplay.h"
#include "RageUtil.h"
#include "RageLog.h"
#include "RageSurface_Load.h"
#include "SongCacheIndex.h"
#include "Sprite.h"
#include "PrefsManager.h"
#include "RageDisplay.h"
#include "RageTexture.h"
#include "RageTextureManager.h"
#include "RageSurface.h"
#include "RageSurfaceUtils.h"
#include "RageSurfaceUtils_Palettize.h"
#include "RageSurfaceUtils_Dither.h"
#include "RageSurfaceUtils_Zoom.h"
#include "SpecialFiles.h"
#include "Banner.h"

#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

static Preference<bool> g_bPalettedImageCache( "PalettedImageCache", false );

/* Memory budget, in megabytes, for cached images kept in memory when the image
 * cache is LowResPreload. Images are then read from disk the first time they're
 * used (the music wheel reads the ones around the cursor ahead of time on a
 * background thread) and the least recently used are dropped when over budget.
 * 0 restores the old behavior: every cached image is loaded at startup and kept.
 * Big libraries with jackets and backgrounds cached took hundreds of megabytes
 * (more than a gigabyte with backgrounds) with the old behavior. */
static Preference<int> g_iImageCacheMemoryMB( "ImageCacheMemoryMB", 128 );

/* Neither a global or a file scope static can be used for this because
 * the order of initialization of nonlocal objects is unspecified. */
//const RString IMAGE_CACHE_INDEX = SpecialFiles::CACHE_DIR + "images.cache";
#define IMAGE_CACHE_INDEX (SpecialFiles::CACHE_DIR + "images.cache")

/* Call CacheImage to cache a image by path.  If the image is already
 * cached, it'll be recreated.  This is efficient if the image hasn't changed,
 * but we still only do this in TidyUpData for songs.
 *
 * Call LoadImage to load a cached image into main memory.  This will call
 * CacheImage only if needed.  This will not do a date/size check; call CacheImage
 * directly if you need that.  When the cache is loaded on use, LoadImage only
 * makes sure the cache file exists; the image is read when it's first used.
 *
 * Call LoadCachedImage to load a image into a texture and retrieve an ID
 * for it.  You can check if the image was actually preloaded by calling
 * TEXTUREMAN->IsTextureRegistered() on the ID; it might not be if the image cache
 * is missing or disabled.
 *
 * Note that each cache entries has two hashes.  The cache path is based soley
 * on the pathname; this way, loading the cache doesn't have to do a stat on every
 * image.  The full hash includes the file size and date, and is used only by
 * CacheImage to avoid doing extra work.
 */

ImageCache *IMAGECACHE; // global and accessible from anywhere in our program

/* Cached images in memory, by source image path. Textures made from an image
 * hold a reference to it, so an image dropped from this map stays alive until
 * its texture is deleted. Guarded by g_Mutex: the prefetch thread adds to it. */
struct CachedImage
{
	std::shared_ptr<RageSurface> pImage;
	std::size_t iBytes;
	std::uint64_t iLastUse;
};
static std::map<RString, CachedImage> g_ImagePathToImage;
static std::size_t g_iImageBytes = 0;
static std::uint64_t g_iUseClock = 0;
static std::mutex g_Mutex;
static int g_iDemandRefcount = 0;

// Prefetch requests (source path, cache file path); guarded by g_Mutex.
static std::deque< std::pair<RString, RString> > g_PrefetchQueue;
static std::condition_variable g_PrefetchCond;
/* Never destroyed at exit: a joinable std::thread destroyed without a join ends the process. */
static std::thread *g_pPrefetchThread = nullptr;
static bool g_bPrefetchStop = false;

static std::set<RString> g_WarnedMissing;

static std::size_t SurfaceBytes( const RageSurface *pImage )
{
	return std::size_t(pImage->pitch) * pImage->h;
}

static std::shared_ptr<RageSurface> FindImage_Locked( const RString &sImagePath )
{
	std::map<RString, CachedImage>::iterator it = g_ImagePathToImage.find( sImagePath );
	if( it == g_ImagePathToImage.end() )
		return std::shared_ptr<RageSurface>();
	it->second.iLastUse = ++g_iUseClock;
	return it->second.pImage;
}

static void RemoveImage_Locked( std::map<RString, CachedImage>::iterator it )
{
	g_iImageBytes -= it->second.iBytes;
	g_ImagePathToImage.erase( it );
}

/* Drop the least recently used images until the budget is met (down to 90% of
 * it, so this doesn't run on every insertion). */
static void TrimToBudget_Locked()
{
	const int iMB = g_iImageCacheMemoryMB.Get();
	if( iMB <= 0 || PREFSMAN->m_ImageCache != IMGCACHE_LOW_RES_PRELOAD )
		return;
	const std::size_t iBudget = std::size_t(iMB) * 1024 * 1024;
	if( g_iImageBytes <= iBudget )
		return;
	const std::size_t iTarget = iBudget / 10 * 9;
	while( g_iImageBytes > iTarget && g_ImagePathToImage.size() > 1 )
	{
		std::map<RString, CachedImage>::iterator oldest = g_ImagePathToImage.begin();
		for( std::map<RString, CachedImage>::iterator it = g_ImagePathToImage.begin(); it != g_ImagePathToImage.end(); ++it )
		{
			if( it->second.iLastUse < oldest->second.iLastUse )
				oldest = it;
		}
		RemoveImage_Locked( oldest );
	}
}

/* Store an image, replacing any older one of the same path. Returns the stored image. */
static std::shared_ptr<RageSurface> StoreImage_Locked( const RString &sImagePath, RageSurface *pImage )
{
	std::map<RString, CachedImage>::iterator it = g_ImagePathToImage.find( sImagePath );
	if( it != g_ImagePathToImage.end() )
		RemoveImage_Locked( it );

	CachedImage &ci = g_ImagePathToImage[sImagePath];
	ci.pImage.reset( pImage );
	ci.iBytes = SurfaceBytes( pImage );
	ci.iLastUse = ++g_iUseClock;
	g_iImageBytes += ci.iBytes;
	std::shared_ptr<RageSurface> ret = ci.pImage;
	TrimToBudget_Locked();
	return ret;
}

static void PrefetchThreadMain()
{
	std::unique_lock<std::mutex> lock( g_Mutex );
	for(;;)
	{
		g_PrefetchCond.wait( lock, []{ return g_bPrefetchStop || !g_PrefetchQueue.empty(); } );
		if( g_bPrefetchStop )
			return;

		const std::pair<RString, RString> job = g_PrefetchQueue.front();
		g_PrefetchQueue.pop_front();
		if( g_ImagePathToImage.find(job.first) != g_ImagePathToImage.end() )
			continue; // already in memory

		// Read the file without holding the lock.
		lock.unlock();
		RageSurface *pImage = RageSurfaceUtils::LoadSurface( job.second );
		lock.lock();

		if( pImage == nullptr )
			continue;
		if( g_ImagePathToImage.find(job.first) != g_ImagePathToImage.end() )
		{
			delete pImage; // the main thread read it meanwhile
			continue;
		}
		StoreImage_Locked( job.first, pImage );
	}
}

RString ImageCache::GetImageCachePath( RString sImageDir ,RString sImagePath )
{
	return SongCacheIndex::GetCacheFilePath( sImageDir, sImagePath );
}

bool ImageCache::IsLoadedOnUse()
{
	return PREFSMAN->m_ImageCache == IMGCACHE_LOW_RES_PRELOAD && g_iImageCacheMemoryMB.Get() > 0;
}

/* The cache file that holds an image. The index remembers it; cached images are
 * looked up by source path only, so an image cached for one category (say
 * Background) also serves a request made for another (say Jacket). */
RString ImageCache::GetCacheFileFor( const RString &sImageDir, const RString &sImagePath ) const
{
	RString sCachePath;
	if( ImageData.GetValue(sImagePath, "Path", sCachePath) && !sCachePath.empty() )
		return sCachePath;
	return GetImageCachePath( sImageDir, sImagePath );
}

/* If in on-demand mode, load all cached images.  This must be fast, so
 * cache files will not be created if they don't exist; that should be done
 * by CacheImage or LoadImage on startup. */
void ImageCache::Demand( RString sImageDir )
{
	++g_iDemandRefcount;
	if( g_iDemandRefcount > 1 )
		return;

	if( PREFSMAN->m_ImageCache != IMGCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	FOREACH_CONST_Child( &ImageData, p )
	{
		RString sImagePath = p->GetName();

		{
			std::lock_guard<std::mutex> lock( g_Mutex );
			if( g_ImagePathToImage.find(sImagePath) != g_ImagePathToImage.end() )
				continue; /* already loaded */
		}

		const RString sCachePath = GetImageCachePath(sImageDir,sImagePath);
		RageSurface *pImage = RageSurfaceUtils::LoadSurface( sCachePath );
		if( pImage == nullptr )
		{
			continue; /* doesn't exist */
		}

		std::lock_guard<std::mutex> lock( g_Mutex );
		StoreImage_Locked( sImagePath, pImage );
	}
}

/* Release images loaded on demand. */
void ImageCache::Undemand( RString sImageDir )
{
	--g_iDemandRefcount;
	if( g_iDemandRefcount != 0 )
		return;

	if( PREFSMAN->m_ImageCache != IMGCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	UnloadAllImages();
}

/* If in a low-res image mode, load a low-res image into memory, creating
 * the cache file if necessary.  Unlike CacheImage(), the original file will
 * not be examined unless the cached image doesn't exist, so the image will
 * not be updated if the original file changes, for efficiency.
 * When the cache is loaded on use, only make sure the cache file exists. */
void ImageCache::LoadImage( RString sImageDir, RString sImagePath )
{
	if( sImagePath == "" )
		return; // nothing to do
	if( PREFSMAN->m_ImageCache != IMGCACHE_LOW_RES_PRELOAD &&
	    PREFSMAN->m_ImageCache != IMGCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	if( IsLoadedOnUse() )
	{
		int iSourceWidth = 0;
		if( ImageData.GetValue(sImagePath, "Width", iSourceWidth) && iSourceWidth != 0 &&
			DoesFileExist(GetCacheFileFor(sImageDir, sImagePath)) )
			return; /* cached; it's read when it's used */

		/* Skip the up-to-date check; the cache file is missing, so it can't
		 * be up to date. */
		CacheImageInternal( sImageDir, sImagePath );
		return;
	}

	/* Load it. */
	const RString sCachePath = GetImageCachePath(sImageDir,sImagePath);

	for( int tries = 0; tries < 2; ++tries )
	{
		{
			std::lock_guard<std::mutex> lock( g_Mutex );
			if( g_ImagePathToImage.find(sImagePath) != g_ImagePathToImage.end() )
				return; /* already loaded */
		}

		CHECKPOINT_M( ssprintf( "ImageCache::LoadImage: %s", sCachePath.c_str() ) );
		RageSurface *pImage = RageSurfaceUtils::LoadSurface( sCachePath );
		if( pImage == nullptr )
		{
			if( tries == 0 )
			{
				/* The file doesn't exist.  It's possible that the image cache file is
				 * missing, so try to create it.  Don't do this first, for efficiency. */
				//LOG->Trace( "Cached image load of '%s' ('%s') failed, trying to cache ...", sImagePath.c_str(), sCachePath.c_str() );

				/* Skip the up-to-date check; it failed to load, so it can't be up
				 * to date. */
				CacheImageInternal( sImageDir, sImagePath );
				continue;
			}
			else
			{
				//LOG->Trace( "Cached image load of '%s' ('%s') failed", sImagePath.c_str(), sCachePath.c_str() );
				return;
			}
		}

		std::lock_guard<std::mutex> lock( g_Mutex );
		StoreImage_Locked( sImagePath, pImage );
	}
}

void ImageCache::Prefetch( const std::vector<RString> &vsImagePaths )
{
	if( !IsLoadedOnUse() )
		return;

	std::deque< std::pair<RString, RString> > jobs;
	std::set<RString> seen;
	{
		std::lock_guard<std::mutex> lock( g_Mutex );
		for( const RString &sImagePath : vsImagePaths )
		{
			if( sImagePath.empty() || !seen.insert(sImagePath).second )
				continue;
			// Images already in memory count as used, so the ones around the
			// cursor aren't the ones dropped.
			if( FindImage_Locked(sImagePath) )
				continue;
			RString sCachePath;
			if( !ImageData.GetValue(sImagePath, "Path", sCachePath) || sCachePath.empty() )
				continue; // not cached
			jobs.push_back( std::make_pair(sImagePath, sCachePath) );
		}
		g_PrefetchQueue.swap( jobs );
		if( g_PrefetchQueue.empty() )
			return;
		if( g_pPrefetchThread == nullptr )
		{
			g_bPrefetchStop = false;
			g_pPrefetchThread = new std::thread( PrefetchThreadMain );
		}
	}
	g_PrefetchCond.notify_one();
}

void ImageCache::StopPrefetchThread()
{
	{
		std::lock_guard<std::mutex> lock( g_Mutex );
		g_bPrefetchStop = true;
		g_PrefetchQueue.clear();
	}
	g_PrefetchCond.notify_all();
	if( g_pPrefetchThread != nullptr )
	{
		g_pPrefetchThread->join();
		delete g_pPrefetchThread;
		g_pPrefetchThread = nullptr;
	}
}

void ImageCache::OutputStats() const
{
	std::size_t iTotalSize = 0;
	{
		std::lock_guard<std::mutex> lock( g_Mutex );
		iTotalSize = g_iImageBytes;
	}
	LOG->Info( "%i bytes of images loaded", int(iTotalSize) );
}

void ImageCache::UnloadAllImages()
{
	std::lock_guard<std::mutex> lock( g_Mutex );
	g_PrefetchQueue.clear();
	g_ImagePathToImage.clear();
	g_iImageBytes = 0;
}

ImageCache::ImageCache()
	: delay_save_cache(false)
{
	ReadFromDisk();
}

ImageCache::~ImageCache()
{
	StopPrefetchThread();
	UnloadAllImages();
}

void ImageCache::ReadFromDisk()
{
	ImageData.ReadFile( IMAGE_CACHE_INDEX );	// don't care if this fails
}

struct ImageTexture: public RageTexture
{
	std::uintptr_t m_uTexHandle;
	std::uintptr_t GetTexHandle() const { return m_uTexHandle; };	// accessed by RageDisplay
	/* Shared with the image cache; it stays alive as long as this texture does,
	 * even if the cache drops it. */
	std::shared_ptr<RageSurface> m_pImage;
	RString m_sCacheFile;
	int m_iWidth, m_iHeight;

	ImageTexture( RageTextureID id, std::shared_ptr<RageSurface> pImage, const RString &sCacheFile, int iWidth, int iHeight ):
		RageTexture(id), m_pImage(pImage), m_sCacheFile(sCacheFile), m_iWidth(iWidth), m_iHeight(iHeight)
	{
		Create();
	}

	~ImageTexture()
	{
		Destroy();
	}

	void Create()
	{
		ASSERT( m_pImage != nullptr );

		/* The image is preprocessed; do as little work as possible. */
		const RageSurface *pImage = m_pImage.get();
		RageSurface *pResized = nullptr;

		/* The source width is the width of the original file. */
		m_iSourceWidth = m_iWidth;
		m_iSourceHeight = m_iHeight;

		/* The image width (within the texture) is always the entire texture.
		 * Only resize if the max texture size requires it; since these images
		 * are already scaled down, this shouldn't happen often.  The shared
		 * image isn't changed: a private copy is read and resized. */
		if( pImage->w > DISPLAY->GetMaxTextureSize() ||
			pImage->h > DISPLAY->GetMaxTextureSize() )
		{
			LOG->Warn( "Converted %s at runtime", GetID().filename.c_str() );
			int iWidth = std::min( pImage->w, DISPLAY->GetMaxTextureSize() );
			int iHeight = std::min( pImage->h, DISPLAY->GetMaxTextureSize() );
			pResized = RageSurfaceUtils::LoadSurface( m_sCacheFile );
			if( pResized == nullptr )
				pResized = RageSurfaceUtils::MakeDummySurface( iWidth, iHeight );
			RageSurfaceUtils::Zoom( pResized, iWidth, iHeight );
			pImage = pResized;
		}

		/* We did this when we cached it. */
		ASSERT( pImage->w == power_of_two(pImage->w) );
		ASSERT( pImage->h == power_of_two(pImage->h) );

		m_iTextureWidth = m_iImageWidth = pImage->w;
		m_iTextureHeight = m_iImageHeight = pImage->h;

		/* Find a supported texture format. If it happens to match the stored
		 * file, we won't have to do any conversion here, and that'll happen
		 * often with paletted images. */
		RagePixelFormat pf = pImage->format->BitsPerPixel == 8? RagePixelFormat_PAL: RagePixelFormat_RGB5A1;
		if( !DISPLAY->SupportsTextureFormat(pf) )
			pf = RagePixelFormat_RGBA4;

		ASSERT( DISPLAY->SupportsTextureFormat(pf) );

		ASSERT(pImage != nullptr);
		/* CreateTexture doesn't change the surface; it takes a non-const pointer only. */
		m_uTexHandle = DISPLAY->CreateTexture( pf, const_cast<RageSurface *>(pImage), false );
		delete pResized;

		CreateFrameRects();
	}

	void Destroy()
	{
		if( m_uTexHandle )
			DISPLAY->DeleteTexture( m_uTexHandle );
		m_uTexHandle = 0;
	}

	void Reload()
	{
		Destroy();
		Create();
	}

	void Invalidate()
	{
		m_uTexHandle = 0; /* don't Destroy() */
	}
};

/* If a image is cached, get its ID for use. */
RageTextureID ImageCache::LoadCachedImage( RString sImageDir, RString sImagePath )
{
	RageTextureID ID( GetImageCachePath(sImageDir,sImagePath) );

	std::size_t Found = sImagePath.find("_blank");
	if( sImagePath == "" || Found!=RString::npos )
		return ID;

	//LOG->Trace( "ImageCache::LoadCachedImage(%s): %s", sImagePath.c_str(), ID.filename.c_str() );

	/* Hack: make sure Image::Load doesn't change our return value and end up
	 * reloading. */
	if(sImageDir == "Banner")
		ID = Sprite::SongBannerTexture(ID);

	/* It's not in a texture.  Do we have it loaded? */
	std::shared_ptr<RageSurface> pImage;
	{
		std::lock_guard<std::mutex> lock( g_Mutex );
		pImage = FindImage_Locked( sImagePath );
	}

	/* Is the image already in a texture? */
	if( TEXTUREMAN->IsTextureRegistered(ID) )
		return ID; /* It's all set. */

	const RString sCacheFile = GetCacheFileFor( sImageDir, sImagePath );
	if( pImage == nullptr && IsLoadedOnUse() )
	{
		/* Not in memory yet: read the cache file now. */
		int iSourceWidth = 0;
		if( ImageData.GetValue(sImagePath, "Width", iSourceWidth) && iSourceWidth != 0 )
		{
			RageSurface *pLoaded = RageSurfaceUtils::LoadSurface( sCacheFile );
			if( pLoaded != nullptr )
			{
				std::lock_guard<std::mutex> lock( g_Mutex );
				pImage = FindImage_Locked( sImagePath ); // the prefetch thread may have read it meanwhile
				if( pImage == nullptr )
					pImage = StoreImage_Locked( sImagePath, pLoaded );
				else
					delete pLoaded;
			}
		}
	}

	if( pImage == nullptr )
	{
		/* Oops, the image is missing.  Warn and continue. */
		if( PREFSMAN->m_ImageCache != IMGCACHE_OFF && g_WarnedMissing.insert(sImagePath).second )
		{
			LOG->Warn( "Image cache for '%s' wasn't loaded", sImagePath.c_str() );
		}
		return ID;
	}

	int iSourceWidth = 0, iSourceHeight = 0;
	ImageData.GetValue( sImagePath, "Width", iSourceWidth );
	ImageData.GetValue( sImagePath, "Height", iSourceHeight );
	if( iSourceWidth == 0 || iSourceHeight == 0 )
	{
		LOG->UserLog( "Cache file", sImagePath, "couldn't be loaded." );
		return ID;
	}

	//LOG->Trace( "Loading image texture %s; src %ix%i; image %ix%i",
	//	    ID.filename.c_str(), iSourceWidth, iSourceHeight, pImage->w, pImage->h );
	RageTexture *pTexture = new ImageTexture( ID, pImage, sCacheFile, iSourceWidth, iSourceHeight );

	ID.Policy = RageTextureID::TEX_VOLATILE;
	TEXTUREMAN->RegisterTexture( ID, pTexture );
	TEXTUREMAN->UnloadTexture( pTexture );

	return ID;
}

static inline int closest( int num, int n1, int n2 )
{
	if( std::abs(num - n1) > std::abs(num - n2) )
		return n2;
	return n1;
}

/* Create or update the image cache file as necessary.  If in preload mode,
 * load the cache file, too.  (This is done at startup.) */
void ImageCache::CacheImage( RString sImageDir, RString sImagePath )
{
	if( PREFSMAN->m_ImageCache != IMGCACHE_LOW_RES_PRELOAD &&
	    PREFSMAN->m_ImageCache != IMGCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	CHECKPOINT_M( sImagePath );
	if( !DoesFileExist(sImagePath) )
		return;

	const RString sCachePath = GetImageCachePath(sImageDir, sImagePath);

	/* Check the full file hash.  If it's the loaded and identical, don't recache. */
	if( DoesFileExist(sCachePath) )
	{
		bool bCacheUpToDate = PREFSMAN->m_bFastLoad;
		if( !bCacheUpToDate )
		{
			unsigned CurFullHash;
			const unsigned FullHash = GetHashForFile( sImagePath );
			if( ImageData.GetValue( sImagePath, "FullHash", CurFullHash ) && CurFullHash == FullHash )
				bCacheUpToDate = true;
		}

		if( bCacheUpToDate )
		{
			/* It's identical.  Just load it, if in preload. */
			if( PREFSMAN->m_ImageCache == IMGCACHE_LOW_RES_PRELOAD )
				LoadImage( sImageDir, sImagePath );

			return;
		}
	}

	/* The cache file doesn't exist, or is out of date.  Cache it.  This
	 * will also load the cache into memory if in PRELOAD. */
	CacheImageInternal( sImageDir, sImagePath );
}

void ImageCache::CacheImageInternal( RString sImageDir, RString sImagePath )
{
	RString sError;
	RageSurface *pImage = RageSurfaceUtils::LoadFile( sImagePath, sError );
	if( pImage == nullptr )
	{
		LOG->UserLog( "Cache file", sImagePath, "couldn't be loaded: %s", sError.c_str() );
		return;
	}

	const int iSourceWidth = pImage->w, iSourceHeight = pImage->h;

	int iWidth = pImage->w / 2, iHeight = pImage->h / 2;
//	int iWidth = pImage->w, iHeight = pImage->h;

	/* Round to the nearest power of two.  This simplifies the actual texture load. */
	iWidth = closest( iWidth, power_of_two(iWidth), power_of_two(iWidth) / 2 );
	iHeight = closest( iHeight, power_of_two(iHeight), power_of_two(iHeight) / 2 );

	/* Don't resize the image to less than 32 pixels in either dimension or the next
	 * power of two of the source (whichever is smaller); it's already very low res. */
	iWidth = std::max( iWidth, std::min(32, power_of_two(iSourceWidth)) );
	iHeight = std::max( iHeight, std::min(32, power_of_two(iSourceHeight)) );

	//RageSurfaceUtils::ApplyHotPinkColorKey( pImage );

	RageSurfaceUtils::Zoom( pImage, iWidth, iHeight );

	/*
	 * When paletted image cache is enabled, cached images are paletted.  Cached
	 * 32-bit images take 1/16 as much memory, 16-bit images take 1/8, and paletted
	 * images take 1/4.
	 *
	 * When paletted image cache is disabled, cached images are stored in 16-bit
	 * RGBA.  Cached 32-bit images take 1/8 as much memory, cached 16-bit images
	 * take 1/4, and cached paletted images take 1/2.
	 *
	 * Paletted cache is disabled by default because palettization takes time, causing
	 * the initial cache run to take longer.  Also, newer ATI hardware doesn't supported
	 * paletted textures, which would slow down runtime, because we have to depalettize
	 * on use.  They'd still have the same memory benefits, though, since we only load
	 * one cached image into a texture at once, and the speed hit may not matter on
	 * newer ATI cards.  RGBA is safer, though.
	 */
	if( g_bPalettedImageCache )
	{
		if( pImage->fmt.BytesPerPixel != 1 )
			RageSurfaceUtils::Palettize( pImage );
	}
	else
	{
		/* Dither to the final format.  We use A1RGB5, since that's usually supported
		 * natively by both OpenGL and D3D. */
		RageSurface *dst = CreateSurface( pImage->w, pImage->h, 16,
			0x7C00, 0x03E0, 0x001F, 0x8000 );

		/* OrderedDither is still faster than ErrorDiffusionDither, and
		 * these images are very small and only displayed briefly. */
		RageSurfaceUtils::OrderedDither( pImage, dst );
		delete pImage;
		pImage = dst;
	}

	const RString sCachePath = GetImageCachePath(sImageDir,sImagePath);
	RageSurfaceUtils::SaveSurface( pImage, sCachePath );

	{
		std::lock_guard<std::mutex> lock( g_Mutex );
		/* If an old image is loaded, free it (a texture using it keeps its own reference). */
		std::map<RString, CachedImage>::iterator it = g_ImagePathToImage.find( sImagePath );
		if( it != g_ImagePathToImage.end() )
			RemoveImage_Locked( it );

		if( PREFSMAN->m_ImageCache == IMGCACHE_LOW_RES_PRELOAD )
		{
			/* Keep it; we're just going to load it anyway. */
			StoreImage_Locked( sImagePath, pImage );
		}
		else
			delete pImage;
	}

	/* Remember the original size. */
	ImageData.SetValue( sImagePath, "Path", sCachePath );
	ImageData.SetValue( sImagePath, "Width", iSourceWidth );
	ImageData.SetValue( sImagePath, "Height", iSourceHeight );
	ImageData.SetValue( sImagePath, "FullHash", GetHashForFile( sImagePath ) );
	if (!delay_save_cache)
		WriteToDisk();
}

void ImageCache::WriteToDisk()
{
	ImageData.WriteFile(IMAGE_CACHE_INDEX);
}

/*
 * (c) 2003 Glenn Maynard
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
