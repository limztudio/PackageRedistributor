#include <tchar.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <deque>

#include "sha2.h"

#include <Windows.h>


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static constexpr TCHAR LogFileName[] = _T("RedistributrLog.log");
static constexpr TCHAR ListFileName[] = _T("RedistributeList.pr");
static constexpr TCHAR HashFileName[] = _T("RedistributeHash.pr");


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


namespace __hidden_Log
{
	static TCHAR TmpString[1 << 16];
	static std::deque<std::basic_string<TCHAR>> Queue;
	static FILE* File = nullptr;
};

bool CreateLog(const std::filesystem::path& LogPath)
{
	std::filesystem::remove(LogPath);

	__hidden_Log::File = nullptr;
	_tfopen_s(&__hidden_Log::File, LogPath.string<TCHAR>().c_str(), _T("wb"));
	if (!__hidden_Log::File)
	{
		return false;
	}

	__hidden_Log::Queue.clear();
	return true;
}
void CloseLog()
{
	for (const auto& String : __hidden_Log::Queue)
	{
		_ftprintf_s(__hidden_Log::File, _T("%s"), String.c_str());
	}
	
	fclose(__hidden_Log::File);
	__hidden_Log::File = nullptr;

	__hidden_Log::Queue.clear();
}
void PushLog(const TCHAR* Format, ...)
{
	va_list ArgList;
	va_start(ArgList, Format);
	_vstprintf_s(__hidden_Log::TmpString, Format, ArgList);
	va_end(ArgList);
	
	__hidden_Log::Queue.emplace_back(__hidden_Log::TmpString);
	_tprintf_s(_T("%s"), __hidden_Log::TmpString);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


typedef std::unique_ptr<FILE, decltype(&fclose)> FilePtr;

FilePtr OpenFile(const std::filesystem::path& FilePath, const TCHAR* Mode)
{
	FILE* File = nullptr;
	_tfopen_s(&File, FilePath.string<TCHAR>().c_str(), Mode);
	return FilePtr(File, fclose);
}
std::filesystem::path ReadFileLine(FilePtr& File)
{
	std::basic_string<TCHAR> TmpString;
	while (!feof(File.get()))
	{
		TCHAR Char = _fgettc(File.get());
		if (Char == _T('\r'))
		{
			continue;
		}
		else if (Char == _T('\n'))
		{
			break;
		}
		else if (Char == static_cast<TCHAR>(-1))
		{
			break;
		}
		TmpString.push_back(Char);
	}

	std::filesystem::path Path = TmpString;
	Path = std::filesystem::absolute(Path);
	return std::move(Path);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


struct RawHash
{
	unsigned char Raw[512];
};

namespace __hidden_Hash
{
	static constexpr size_t ReadHashSize = 100 * 1024 * 1024;
	static unsigned char TmpMover[__hidden_Hash::ReadHashSize];
};

void ConvertToHash(FilePtr& File, RawHash& Hash)
{
	sha512_ctx CTX;
	sha512_init(&CTX);
	
	while (const size_t Read = fread_s(__hidden_Hash::TmpMover, __hidden_Hash::ReadHashSize, sizeof(unsigned char), __hidden_Hash::ReadHashSize, File.get()))
	{
		sha512_update(&CTX, __hidden_Hash::TmpMover, static_cast<unsigned int>(Read));
	}
	
	sha512_final(&CTX, Hash.Raw);
}
template <typename I> std::string n2hexstr(I w, size_t hex_len = sizeof(I)<<1) {
	static const char* digits = "0123456789ABCDEF";
	std::string rc(hex_len,'0');
	for (size_t i=0, j=(hex_len-1)*4 ; i<hex_len; ++i,j-=4)
		rc[i] = digits[(w>>j) & 0x0f];
	return rc;
}
std::basic_string<TCHAR> ConvertToString(const RawHash& Hash)
{
	static constexpr size_t Len = sizeof(RawHash::Raw) << 1;
	static constexpr TCHAR Digits[] = _T("0123456789abcdef");
	std::basic_string<TCHAR> TmpString(Len, _T('0'));

	for(size_t i = 0; i < (Len >> 1); ++i)
	{
		TmpString[(i << 1) + 0] = Digits[(Hash.Raw[i] >> 4) & 0x0f];
		TmpString[(i << 1) + 1] = Digits[(Hash.Raw[i] >> 0) & 0x0f];
	}

	return std::move(TmpString);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void CreateHash(const std::filesystem::path& SrcPath)
{
	std::deque<std::filesystem::path> PathsToHashMaking;
	
	std::filesystem::path ListPath(SrcPath / ListFileName);
	FilePtr ListFile(OpenFile(ListPath, _T("r")));
	if (!ListFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), ListPath.string<TCHAR>().c_str());
		return;
	}

	std::filesystem::path HashPath(SrcPath / HashFileName);
	std::filesystem::remove(HashPath);
	FilePtr HashFile(OpenFile(HashPath, _T("w")));
	if (!HashFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), HashPath.string<TCHAR>().c_str());
		return;
	}

	SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());
	while (!feof(ListFile.get()))
	{
		std::filesystem::path CurPath = ReadFileLine(ListFile);
		if (!std::filesystem::exists(CurPath))
		{
			PushLog(_T("!!Error: No such file or directory \"%s\"\n"), CurPath.string<TCHAR>().c_str());
			continue;
		}

		if (std::filesystem::is_directory(CurPath))
		{
			for (auto const& ChildPath : std::filesystem::recursive_directory_iterator{ CurPath }) 
			{
				if (std::filesystem::is_directory(ChildPath))
				{
					continue;
				}
				PathsToHashMaking.emplace_back(ChildPath);
			}
		}
		else
		{
			PathsToHashMaking.emplace_back(std::move(CurPath));
		}
	}

	PushLog(_T("* Following files will be hashed:\n"));
	for (const auto& CurPath : PathsToHashMaking)
	{
		PushLog(_T("%s\n"), CurPath.string<TCHAR>().c_str());
	}
	PushLog(_T("\n"));

	std::basic_string<TCHAR> TmpString;
	for (const auto& Path : PathsToHashMaking)
	{
		RawHash Hash;
		{
			FilePtr CurFile(OpenFile(Path, _T("rb")));
			if (!CurFile)
			{
				PushLog(_T("!!Error: Cannot open \"%s\"\n"), Path.string<TCHAR>().c_str());
				memset(Hash.Raw, 0xff, sizeof(RawHash::Raw));
			}
			else
			{
				ConvertToHash(CurFile, Hash);
			}
		}

		TmpString = std::filesystem::relative(Path, SrcPath).string<TCHAR>();
		TmpString += _T("\n");
		TmpString += ConvertToString(Hash);
		TmpString += _T("\n");

		_fputts(TmpString.c_str(), HashFile.get());
	}
	
	PushLog(_T("* Done"));
}

void CopyPackage(const std::filesystem::path& SrcPath, const std::filesystem::path& DestPath)
{
	std::deque<std::filesystem::path> PathsToCopy;
	
	std::filesystem::path ListPath(SrcPath / ListFileName);
	FilePtr ListFile(OpenFile(ListPath, _T("r")));
	if (!ListFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), ListPath.string<TCHAR>().c_str());
		return;
	}

	SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());
	while (!feof(ListFile.get()))
	{
		std::filesystem::path CurPath = ReadFileLine(ListFile);
		if (!std::filesystem::exists(CurPath))
		{
			PushLog(_T("!!Error: No such file or directory \"%s\"\n"), CurPath.string<TCHAR>().c_str());
			continue;
		}

		if (std::filesystem::is_directory(CurPath))
		{
			for (auto const& ChildPath : std::filesystem::recursive_directory_iterator{ CurPath }) 
			{
				PathsToCopy.emplace_back(ChildPath);
			}
		}
		else
		{
			PathsToCopy.emplace_back(std::move(CurPath));
		}
	}

	PushLog(_T("* Following files will be copied:\n"));
	for (const auto& CurPath : PathsToCopy)
	{
		PushLog(_T("%s"), CurPath.string<TCHAR>().c_str());
	}
	PushLog(_T("\n"));

	
	
	PushLog(_T("* Done"));
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int _tmain(int Argc, TCHAR* Argv[])
{
	switch(Argc)
	{
	case 2:
	{
		std::filesystem::path SrcPath(std::filesystem::absolute(Argv[1]));
		if (!std::filesystem::is_directory(SrcPath))
		{
			_tprintf_s(_T("!!Error: No such directory \"%s\"\n"), SrcPath.string<TCHAR>().c_str());
			return -1;
		}

		std::filesystem::path LogPath(SrcPath / LogFileName);
		if(!CreateLog(LogPath))
		{
			_tprintf_s(_T("!!Error: Cannot open/create \"%s\"\n"), LogPath.string<TCHAR>().c_str());
			return -1;
		}

		CreateHash(SrcPath);

		CloseLog();
	}
	break;

	case 3:
	{
		std::filesystem::path SrcPath(std::filesystem::absolute(Argv[1]));
		if (!std::filesystem::is_directory(SrcPath))
		{
			_tprintf_s(_T("!!Error: No such directory \"%s\"\n"), SrcPath.string<TCHAR>().c_str());
			return -1;
		}
		
		std::filesystem::path DestPath(std::filesystem::absolute(Argv[2]));

		std::filesystem::path LogPath(SrcPath / LogFileName);
		if(!CreateLog(LogPath))
		{
			_tprintf_s(_T("!!Error: Cannot open/create \"%s\"\n"), LogPath.string<TCHAR>().c_str());
			return -1;
		}

		CopyPackage(SrcPath, DestPath);

		CloseLog();
	}	
	break;

	default:
		_tprintf_s(_T("exe [src]: Read copy list named \"%s\"\n"), ListFileName);
		_tprintf_s(_T("exe [src] [dest]: Copy \"Src\" into \"Dest\" based on \"%s\" which defined at \"Src\". By comparing hash value, only different file will be updated.\n"), ListFileName);
		return -1;
	}
	
	return 0;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

