#include "precompiled.h"

#include "ColladaManager.h"

#include "graphics/ModelDef.h"
#include "lib/fnv_hash.h"
#include "lib/res/file/vfs.h"
#include "lib/res/handle.h"
#include "ps/CLogger.h"
#include "ps/CStr.h"
#include "ps/CVFSFile.h"
#include "ps/DllLoader.h"

namespace Collada
{
	#include "collada/DLL.h"
}

namespace
{
	struct VFSOutputCB
	{
		VFSOutputCB(Handle hf) : hf(hf) {}
		void operator() (const char* data, unsigned int length)
		{
			FileIOBuf buf = (FileIOBuf)data;
			const ssize_t ret = vfs_io(hf, length, &buf);
			// TODO: handle errors sensibly
		}

		Handle hf;
	};

	void ColladaLog(int severity, const char* text)
	{
		LOG(severity == LOG_INFO ? NORMAL :
			severity == LOG_WARNING ? WARNING : ERROR,
			"collada", "%s", text);
	}

	void ColladaOutput(void* cb_data, const char* data, unsigned int length)
	{
		VFSOutputCB* cb = static_cast<VFSOutputCB*>(cb_data);
		(*cb)(data, length);
	}
}

class CColladaManagerImpl
{
	DllLoader dll;

	void (*set_logger)(Collada::LogFn logger);
	int (*set_skeleton_definitions)(const char* xml, int length);
	int (*convert_dae_to_pmd)(const char* dae, Collada::OutputFn pmd_writer, void* cb_data);
	int (*convert_dae_to_psa)(const char* dae, Collada::OutputFn psa_writer, void* cb_data);

public:
	CColladaManagerImpl()
		: dll("Collada")
	{
	}

	~CColladaManagerImpl()
	{
		if (dll.IsLoaded())
			set_logger(NULL); // unregister the log handler
	}

	bool Convert(const CStr& daeFilename, const CStr& pmdFilename, CColladaManager::FileType type)
	{
		// To avoid always loading the DLL when it's usually not going to be
		// used (and to do the same on Linux where delay-loading won't help),
		// and to avoid compile-time dependencies (because it's a minor pain
		// to get all the right libraries to build the COLLADA DLL), we load
		// it dynamically when it is required, instead of using the exported
		// functions and binding at link-time.
		if (! dll.IsLoaded())
		{
			if (! dll.LoadDLL())
			{
				LOG_ONCE(ERROR, "collada", "Failed to load COLLADA conversion DLL");
				return false;
			}

			try
			{
				dll.LoadSymbol("set_logger", set_logger);
				dll.LoadSymbol("set_skeleton_definitions", set_skeleton_definitions);
				dll.LoadSymbol("convert_dae_to_pmd", convert_dae_to_pmd);
				dll.LoadSymbol("convert_dae_to_psa", convert_dae_to_psa);
			}
			catch (PSERROR_DllLoader&)
			{
				LOG(ERROR, "collada", "Failed to load symbols from COLLADA conversion DLL");
				dll.Unload();
				return false;
			}

			set_logger(ColladaLog);

			CVFSFile skeletonFile;
			if (skeletonFile.Load("art/skeletons/skeletons.xml") != PSRETURN_OK)
			{
				LOG(ERROR, "collada", "Failed to read skeleton definitions");
				dll.Unload();
				return false;
			}

			int ok = set_skeleton_definitions((const char*)skeletonFile.GetBuffer(), (int)skeletonFile.GetBufferSize());
			if (ok < 0)
			{
				LOG(ERROR, "collada", "Failed to load skeleton definitions");
				dll.Unload();
				return false;
			}

			// TODO: the cached PMD/PSA files should probably be invalidated when
			// the skeleton definition file is changed, else people will get confused
			// as to why it's not picking up their changes
		}

		// We need to null-terminate the buffer, so do it (possibly inefficiently)
		// by converting to a CStr
		CStr daeData;

		{
			CVFSFile daeFile;
			if (daeFile.Load(daeFilename) != PSRETURN_OK)
				return false;

			daeData = daeFile.GetAsString();

			// scope closes daeFile - necessary if we don't use FILE_LONG_LIVED
		}

		// Prepare the output file

		Handle hf = vfs_open(pmdFilename, FILE_WRITE|FILE_NO_AIO);
		if (hf < 0)
			return false;

		// Do the conversion

		VFSOutputCB cb (hf);

		switch (type)
		{
		case CColladaManager::PMD: convert_dae_to_pmd(daeData.c_str(), ColladaOutput, static_cast<void*>(&cb)); break;
		case CColladaManager::PSA: convert_dae_to_psa(daeData.c_str(), ColladaOutput, static_cast<void*>(&cb)); break;
		}
		
		vfs_close(hf);

		return true;
	}
};

CColladaManager::CColladaManager()
: m(new CColladaManagerImpl())
{
}

CColladaManager::~CColladaManager()
{
	delete m;
}

CStr CColladaManager::GetLoadableFilename(const CStr& sourceName, FileType type)
{
	const char* extn = NULL;
	switch (type)
	{
	case PMD: extn = ".pmd"; break;
	case PSA: extn = ".psa"; break;
		// no other alternatives
	}

	/*

	If there is a .dae file:
		* Calculate a hash to identify it.
		* Look for a cached .pmd file matching that hash.
		* If it exists, load it. Else, convert the .dae into .pmd and load it.
	Otherwise, if there is a (non-cache) .pmd file:
		* Load it.
	Else, fail.

	The hash calculation ought to be fast, since normally (during development)
	the .dae file will exist but won't have changed recently and so the cache
	would be used. Hence, just hash the file's size, mtime, and the converter
	version number (so updates of the converter can cause regeneration of .pmds)
	instead of the file's actual contents.

	TODO (maybe): The .dae -> .pmd conversion may fail (e.g. if the .dae is
	invalid or unsupported), but it may take a long time to start the conversion
	then realise it's not going to work. That will delay the loading of the game
	every time, which is annoying, so maybe it should cache the error message
	until the .dae is updated and fixed. (Alternatively, avoid having that many
	broken .daes in the game.)

	*/

	// (TODO: the comments and variable names say "pmd" but actually they can
	// be "psa" too.)

	CStr dae = sourceName + ".dae";
	if (! vfs_exists(dae))
	{
		// No .dae - got to use the .pmd, assuming there is one
		return sourceName + extn;
	}

	// There is a .dae - see if there's an up-to-date cached copy

	struct stat fileStat;
	if (vfs_stat(dae, &fileStat) < 0)
	{
		// This shouldn't occur for any sensible reasons
		LOG(ERROR, "collada", "Failed to stat DAE file '%s'", dae.c_str());
		return "";
	}

	// Build a struct of all the data we want to hash.
	// (Use ints and not time_t/off_t because we don't care about overflow
	// but do care about the fields not being 64-bit aligned)
	// (Remove the lowest bit of mtime because some things round it to a
	// resolution of 2 seconds)
	struct { int version; int mtime; int size; } hashSource
		= { COLLADA_CONVERTER_VERSION, (int)fileStat.st_mtime & ~1, (int)fileStat.st_size };
	cassert(sizeof(hashSource) == sizeof(int) * 3); // no padding, because that would be bad

	// Calculate the hash, convert to hex
	u32 hash = fnv_hash(static_cast<void*>(&hashSource), sizeof(hashSource));
	char hashString[9];
	sprintf(hashString, "%08x", hash);

	// realDaePath is "mods/whatever/art/meshes/whatever.dae"
	char realDaePath[PATH_MAX];
	vfs_realpath(dae, realDaePath);

	// cachedPmdVfsPath is "cache/mods/whatever/art/meshes/whatever_{hash}.pmd"
	CStr cachedPmdVfsPath = "cache/";
	cachedPmdVfsPath += realDaePath;
	// Remove the .dae extension (which will certainly be there)
	cachedPmdVfsPath = cachedPmdVfsPath.substr(0, cachedPmdVfsPath.length()-4);
	// Add a _hash.pmd extension
	cachedPmdVfsPath += "_";
	cachedPmdVfsPath += hashString;
	cachedPmdVfsPath += extn;

	// If it's not in the cache, we'll have to create it first
	if (! vfs_exists(cachedPmdVfsPath))
	{
		if (! m->Convert(dae, cachedPmdVfsPath, type))
			return ""; // failed to convert
	}

	return cachedPmdVfsPath;
}
