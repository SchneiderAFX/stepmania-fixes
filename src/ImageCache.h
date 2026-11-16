#ifndef IMAGE_CACHE_H
#define IMAGE_CACHE_H

#include "IniFile.h"

#include "RageTexture.h"

#include <vector>

class LoadingWindow;
/** @brief Maintains a cache of reduced-quality images. */
class ImageCache
{
public:
	ImageCache();
	~ImageCache();
	void ReadFromDisk();
	void WriteToDisk();

	RageTextureID LoadCachedImage( RString sImageDir, RString sImagePath );
	void CacheImage( RString sImageDir, RString sImagePath );
	void LoadImage( RString sImageDir, RString sImagePath );

	void Demand( RString sImageDir );
	void Undemand( RString sImageDir );

	/* Read the cached images of these source paths into memory on a background
	 * thread, so that LoadCachedImage finds them without touching the disk.
	 * Replaces any previous request that hasn't been read yet; nearest first.
	 * Does nothing unless the cache is loaded on use (see IsLoadedOnUse). */
	void Prefetch( const std::vector<RString> &vsImagePaths );

	void OutputStats() const;

	bool delay_save_cache;

private:
	static RString GetImageCachePath( RString sImageDir, RString sImagePath );
	/* True when cached images are read from disk the first time they're used
	 * and kept within a memory budget, instead of all being loaded at startup. */
	static bool IsLoadedOnUse();
	RString GetCacheFileFor( const RString &sImageDir, const RString &sImagePath ) const;
	void UnloadAllImages();
	void CacheImageInternal( RString sImageDir, RString sImagePath );
	void StopPrefetchThread();

	IniFile ImageData;
};

extern ImageCache *IMAGECACHE; // global and accessible from anywhere in our program

#endif

/**
 * @file
 * @author Glenn Maynard (c) 2003
 * @section LICENSE
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