#include <tchar.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <deque>
#include <unordered_set>
#include <unordered_map>

#include "sha2.h"

#include <ppl.h>
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


namespace __hidden_File
{
	struct EqualTo{
		bool operator()(const std::filesystem::path& Lhs, const std::filesystem::path& Rhs) const
		{
			return std::filesystem::equivalent(Lhs, Rhs);
		}
	};
};

typedef std::unique_ptr<FILE, decltype(&fclose)> FilePtr;

using PathSet = std::unordered_set<std::filesystem::path, std::hash<std::filesystem::path>, __hidden_File::EqualTo>;
template <typename Value>
using PathMap = std::unordered_map<std::filesystem::path, Value, std::hash<std::filesystem::path>, __hidden_File::EqualTo>;

FilePtr OpenFile(const std::filesystem::path& FilePath, const TCHAR* Mode)
{
	FILE* File = nullptr;
	_tfopen_s(&File, FilePath.string<TCHAR>().c_str(), Mode);
	return FilePtr(File, fclose);
}
std::basic_string<TCHAR> ReadFileStringLine(FilePtr& File)
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

	if (TmpString.empty())
	{
		return std::filesystem::path();
	}

	return std::move(TmpString);
}
std::filesystem::path ReadFileLine(FilePtr& File)
{
	std::basic_string<TCHAR> TmpString(ReadFileStringLine(File));
	if (TmpString.empty())
	{
		return std::filesystem::path();
	}

	std::filesystem::path Path = TmpString;
	Path = std::filesystem::absolute(Path);
	return std::move(Path);
}
std::filesystem::path ReadFileLine(FilePtr& File, bool& bExclude)
{
	std::basic_string<TCHAR> TmpString(ReadFileStringLine(File));
	if (TmpString.empty())
	{
		return std::filesystem::path();
	}

	if (TmpString[0] == _T('~'))
	{
		bExclude = true;
		TmpString = TmpString.substr(1);
	}
	else
	{
		bExclude = false;
	}

	std::filesystem::path Path = TmpString;
	Path = std::filesystem::absolute(Path);
	return std::move(Path);
}

bool CheckIfFileContained(const std::deque<std::filesystem::path>& Table, const std::filesystem::path& Path)
{
	for (const auto& Compare : Table)
	{
		if (std::filesystem::equivalent(Compare, Path))
		{
			return true;
		}

		auto Lhs = Compare.string<TCHAR>();
		auto Rhs = Path.string<TCHAR>();

		std::transform(Lhs.begin(), Lhs.end(), Lhs.begin(), ::tolower);
		std::transform(Rhs.begin(), Rhs.end(), Rhs.begin(), ::tolower);

		if (Rhs.starts_with(Lhs))
		{
			return true;
		}
	}
	return false;
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
bool ConvertToHash(const std::basic_string<TCHAR>& Str, RawHash& Hash)
{
	if (Str.length() < (sizeof(RawHash::Raw) << 1))
		return false;
	
	for (size_t i = 0; i < (sizeof(RawHash::Raw) << 1); ++i)
	{
		TCHAR Char = Str[i];
		if (Char >= _T('0') && Char <= _T('9'))
		{
			Char -= _T('0');
		}
		else if (Char >= _T('a') && Char <= _T('f'))
		{
			Char -= _T('a') - 10;
		}
		else if (Char >= _T('A') && Char <= _T('F'))
		{
			Char -= _T('A') - 10;
		}
		else
		{
			Char = 0;
		}

		if (i & 1)
		{
			Hash.Raw[i >> 1] |= Char;
		}
		else
		{
			Hash.Raw[i >> 1] = Char << 4;
		}
	}

	return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void CreateHash(const std::filesystem::path& SrcPath)
{
	PathSet PathsToExclude;
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

	{
		PushLog(_T("* Read file list to making hash:\n"));
		
		SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());
		while (!feof(ListFile.get()))
		{
			bool bExclude;
			std::filesystem::path CurPath(ReadFileLine(ListFile, bExclude));
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

					if (bExclude)
					{
						PathsToExclude.emplace(ChildPath);
					}
					else
					{
						PathsToHashMaking.emplace_back(ChildPath);
					}
				}
			}
			else
			{
				if (bExclude)
				{
					PathsToExclude.emplace(std::move(CurPath));
				}
				else
				{
					PathsToHashMaking.emplace_back(std::move(CurPath));
				}
			}
		}
		ListFile.release();

		if (PathsToHashMaking.empty())
		{
			PushLog(_T("* No file\n"));	
		}
		else
		{
			PushLog(_T("* %u file(s) found\n"), static_cast<unsigned>(PathsToHashMaking.size()));
		}
	}

	if (PathsToHashMaking.empty())
	{
		return;
	}
	
	{
		PushLog(_T("* Following files will be hashed:\n"));
		
		for (auto It = PathsToHashMaking.begin(); It != PathsToHashMaking.end();)
		{
			if (PathsToExclude.find(*It) != PathsToExclude.end())
			{
				It = PathsToHashMaking.erase(It);
			}
			else
			{
				PushLog(_T("%s\n"), It->string<TCHAR>().c_str());
				++It;
			}
		}
		
		PushLog(_T("\n"));
	}

	{
		PushLog(_T("* Hash making started:\n"));
		
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
}

void CopyPackage(const std::filesystem::path& SrcPath, const std::filesystem::path& DestPath)
{
	std::filesystem::path ListPath(SrcPath / ListFileName);
	FilePtr ListFile(OpenFile(ListPath, _T("r")));
	if (!ListFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), ListPath.string<TCHAR>().c_str());
		return;
	}

	std::filesystem::path SrcHashPath(SrcPath / HashFileName);
	FilePtr SrcHashFile(OpenFile(SrcHashPath, _T("r")));
	if (!SrcHashFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), SrcHashPath.string<TCHAR>().c_str());
		return;
	}
	
	std::filesystem::path DestHashPath(DestPath / HashFileName);
	FilePtr DestHashFile(OpenFile(DestHashPath, _T("r")));

	std::deque<std::filesystem::path> ExcludeForDeletion;
	{
		PushLog(_T("* Read list for excluding from update:\n"));
		
		SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());
		
		while (!feof(ListFile.get()))
		{
			bool bExclude;
			std::filesystem::path CurPath(ReadFileLine(ListFile, bExclude));
			if (!bExclude)
			{
				continue;
			}
			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, SrcPath);
			
			ExcludeForDeletion.emplace_back(std::move(RelativePath));
		}
		ListFile.release();

		if (ExcludeForDeletion.empty())
		{
			PushLog(_T("* No files or directories\n"));	
		}
		else
		{
			PushLog(_T("* %u files or directories found\n"), static_cast<unsigned>(ExcludeForDeletion.size()));
		}
	}

	PathMap<RawHash> DestHashes;
	if (DestHashFile)
	{
		PushLog(_T("* Read destination file hash:\n"));
		
		SetCurrentDirectory(DestPath.string<TCHAR>().c_str());

		while (!feof(DestHashFile.get()))
		{
			std::filesystem::path CurPath(ReadFileLine(DestHashFile));
			if (CurPath.empty())
			{
				continue;
			}
			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, DestPath);
			if (CheckIfFileContained(ExcludeForDeletion, RelativePath))
			{
				continue;
			}

			RawHash CurHash;
			if (!ConvertToHash(ReadFileStringLine(DestHashFile), CurHash))
			{
				PushLog(_T("!!Error: Invalid hash format \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				continue;
			}

			DestHashes.emplace(std::move(RelativePath), std::move(CurHash));
		}
		DestHashFile.release();

		if (DestHashes.empty())
		{
			PushLog(_T("* No file hash\n"));	
		}
		else
		{
			PushLog(_T("* %u file hash found\n"), static_cast<unsigned>(DestHashes.size()));
		}
	}

	PathMap<RawHash> SrcHashes;
	{
		PushLog(_T("* Read source file hash:\n"));
		
		SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());

		while (!feof(SrcHashFile.get()))
		{
			std::filesystem::path CurPath(ReadFileLine(SrcHashFile));
			if (CurPath.empty())
			{
				continue;
			}
			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, SrcPath);
			if (CheckIfFileContained(ExcludeForDeletion, RelativePath))
			{
				continue;
			}

			RawHash CurHash;
			if (!ConvertToHash(ReadFileStringLine(SrcHashFile), CurHash))
			{
				PushLog(_T("!!Error: Invalid hash format \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				continue;
			}

			SrcHashes.emplace(std::move(RelativePath), std::move(CurHash));
		}
		SrcHashFile.release();

		if (SrcHashes.empty())
		{
			PushLog(_T("* No file hash\n"));	
		}
		else
		{
			PushLog(_T("* %u file hash found\n"), static_cast<unsigned>(SrcHashes.size()));
		}
	}

	{
		PushLog(_T("* Remove files or directories that no longer exist on source location:\n"));

		size_t NumDeleted = 0;
		
		for (auto const& CurPath : std::filesystem::recursive_directory_iterator{ DestPath })
		{
			if (std::filesystem::is_directory(CurPath))
			{
				continue;
			}

			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, DestPath);

			if (SrcHashes.find(RelativePath) == SrcHashes.end())
			{
				if (std::filesystem::remove(RelativePath))
				{
					++NumDeleted;
					PushLog(_T("%s\n"), RelativePath.string<TCHAR>().c_str());
				}
				else
				{
					PushLog(_T("!!Error: Failed to remove \"%s\"\n"), RelativePath.string<TCHAR>().c_str());
				}
			}

			std::filesystem::path ParentPath = CurPath.path().parent_path();
			if (std::filesystem::is_directory(ParentPath) && std::filesystem::is_empty(ParentPath))
			{
				if (std::filesystem::remove(ParentPath))
				{
					++NumDeleted;
					PushLog(_T("%s\n"), ParentPath.string<TCHAR>().c_str());
				}
				else
				{
					PushLog(_T("!!Error: Failed to remove \"%s\"\n"), ParentPath.string<TCHAR>().c_str());
				}
			}
		}

		if (NumDeleted <= 0)
		{
			PushLog(_T("* No removed file or directory\n"));	
		}
		else
		{
			PushLog(_T("* %u file or directory has been removed\n"), static_cast<unsigned>(NumDeleted));
		}
	}

	{
		PushLog(_T("* Collect files which need update:\n"));
		
		const size_t TotalCount = SrcHashes.size();

		std::vector<std::tuple<PathMap<RawHash>::iterator, bool>> HashesToCompare;
		HashesToCompare.reserve(TotalCount);
		for (auto It = SrcHashes.begin(), Et = SrcHashes.end(); It != Et; ++It)
		{
			HashesToCompare.emplace_back(std::make_tuple(It, false));
		}
		concurrency::parallel_for_each(HashesToCompare.begin(), HashesToCompare.end(), [&DestHashes](decltype(HashesToCompare)::value_type& Wrapped)
		{
			auto Found = DestHashes.find(std::get<0>(Wrapped)->first);
			if (Found == DestHashes.end())
			{
				return;
			}

			if (memcmp(std::get<0>(Wrapped)->second.Raw, Found->second.Raw, sizeof(RawHash::Raw)) != 0)
			{
				std::get<1>(Wrapped) = true;
			}
		});
		for (const auto& Wrapped : HashesToCompare)
		{
			if (!std::get<1>(Wrapped))
			{
				continue;
			}

			SrcHashes.erase(std::get<0>(Wrapped));
		}

		const size_t UpdateCount = SrcHashes.size();

		if (UpdateCount <= 0)
		{
			PushLog(_T("* No file need update\n"));	
		}
		else
		{
			PushLog(_T("* (%u/%u) file need update\n"), static_cast<unsigned>(UpdateCount), static_cast<unsigned>(TotalCount));
		}
	}
	
	{
		PushLog(_T("* Update started:\n"));

		for (const auto& Wrapped : SrcHashes)
		{
			const std::filesystem::path FromPath = SrcPath / Wrapped.first;
			const std::filesystem::path ToPath = DestPath / Wrapped.first;

			std::error_code Error;
			std::filesystem::copy(FromPath, ToPath, std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::copy_symlinks, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Failed to copy from \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), ToPath.string<TCHAR>().c_str());
			}
			else
			{
				PushLog(_T("File copied from \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), ToPath.string<TCHAR>().c_str());
			}
		}

		PushLog(_T("* Done"));
	}
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

