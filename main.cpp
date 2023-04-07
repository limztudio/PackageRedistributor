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
	std::error_code Error;
	
	std::filesystem::remove(LogPath, Error);
	if(Error)
	{
		return false;
	}

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
	struct Hasher
	{
		size_t operator()(const std::filesystem::path& Val) const noexcept
		{
			return std::filesystem::hash_value(Val);
		}
	};
	struct EqualTo{
		bool operator()(const std::filesystem::path& Lhs, const std::filesystem::path& Rhs) const noexcept
		{
			std::error_code Error;
			
			const size_t Val = std::filesystem::equivalent(Lhs, Rhs, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Error occurred while comparing paths \"%s\" and \"%s\"\n"), Lhs.string<TCHAR>().c_str(), Rhs.string<TCHAR>().c_str());
				return false;
			}
			return Val;
		}
	};

	class FileIO
	{
	public:
		FileIO() : File(nullptr) {}
		FileIO(const std::filesystem::path& _FilePath, const TCHAR* Mode) : File(nullptr), FilePath(_FilePath)
		{
			_tfopen_s(&File, FilePath.string<TCHAR>().c_str(), Mode);
		}
		FileIO(std::filesystem::path&& _FilePath, const TCHAR* Mode) : File(nullptr), FilePath(std::move(_FilePath))
		{
			_tfopen_s(&File, FilePath.string<TCHAR>().c_str(), Mode);
		}
		FileIO(const FileIO& Rhs) = delete; 
		FileIO(FileIO&& Rhs) noexcept : File(Rhs.File), FilePath(std::move(Rhs.FilePath)) { Rhs.File = nullptr; }

		~FileIO()
		{
			if (File)
			{
				Close();
			}
		}

	public:
		FileIO& operator=(const FileIO& Rhs) = delete;
		FileIO& operator=(FileIO&& Rhs) noexcept
		{
			if (File)
			{
				Close();
			}
			
			File = Rhs.File;
			FilePath = std::move(Rhs.FilePath);

			Rhs.File = nullptr;

			return *this;
		}

	public:
		operator bool() const noexcept
		{
			return (File != nullptr);
		}
		FILE* Get() const noexcept
		{
			return File;
		}

	public:
		bool CloseWithReturn()
		{
			if (fclose(File))
			{
				return false;
			}
			File = nullptr;
			return true;
		}
		void Close()
		{
			if (!CloseWithReturn())
			{
				PushLog(_T("!!Error: Failed to close file \"%s\"\n"), FilePath.string<TCHAR>().c_str());
			}
		}
		
	private:
		FILE* File;
		std::filesystem::path FilePath;
	};

	unsigned char CopyBuffer[1 * 1024 * 1024 * 1024];
};

typedef __hidden_File::FileIO FilePtr;

using PathSet = std::unordered_set<std::filesystem::path, __hidden_File::Hasher, __hidden_File::EqualTo>;
template <typename Value>
using PathMap = std::unordered_map<std::filesystem::path, Value, __hidden_File::Hasher, __hidden_File::EqualTo>;

std::basic_string<TCHAR> ReadFileStringLine(FilePtr& File)
{
	std::basic_string<TCHAR> TmpString;
	while (!feof(File.Get()))
	{
		TCHAR Char = _fgettc(File.Get());
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
	std::error_code Error;
	
	std::basic_string<TCHAR> TmpString(ReadFileStringLine(File));
	if (TmpString.empty())
	{
		return std::filesystem::path();
	}

	std::filesystem::path Path = TmpString;
	Path = std::filesystem::canonical(Path, Error);
	if (Error)
	{
		PushLog(_T("!!Error: Error occurred while canonicalizing \"%s\"\n"), TmpString.c_str());
		return std::filesystem::path();
	}
	
	return std::move(Path);
}
std::filesystem::path ReadFileLine(FilePtr& File, bool& bExclude)
{
	std::error_code Error;
	
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
	Path = std::filesystem::canonical(Path, Error);
	if (Error)
	{
		PushLog(_T("!!Error: Error occurred while canonicalizing \"%s\"\n"), TmpString.c_str());
		return std::filesystem::path();
	}
	
	return std::move(Path);
}

bool CheckIfFileContained(const std::deque<std::filesystem::path>& Table, const std::filesystem::path& Path)
{
	std::error_code Error;
	
	for (const auto& Compare : Table)
	{
		const bool bIsEqual = std::filesystem::equivalent(Compare, Path, Error);
		if (Error)
		{
			PushLog(_T("!!Error: Error occurred while comparing paths \"%s\" and \"%s\"\n"), Compare.string<TCHAR>().c_str(), Path.string<TCHAR>().c_str());
			return false;
		}
		if (bIsEqual)
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

bool BufferFileCopy(const std::filesystem::path& FromPath, const std::filesystem::path& ToPath)
{
	std::error_code Error;
	
	FilePtr FromFile(FromPath.string<TCHAR>().c_str(), _T("rb"));
	if (!FromFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), FromPath.string<TCHAR>().c_str());
		return false;
	}

	bool bShouldCreate = true;
	const std::filesystem::path ToParentPath = ToPath.parent_path();
	const bool bExists = std::filesystem::exists(ToParentPath, Error);
	if (Error)
	{
		PushLog(_T("!!Error: Error occurred while checking if \"%s\" exists\n"), ToParentPath.string<TCHAR>().c_str());
		return false;
	}
	if (bExists)
	{
		const bool bIsDirectory = std::filesystem::is_directory(ToParentPath, Error);
		if (Error)
		{
			PushLog(_T("!!Error: Error occurred while checking if \"%s\" is a directory\n"), ToParentPath.string<TCHAR>().c_str());
			return false;
		}
		if (bIsDirectory)
		{
			bShouldCreate = false;
		}
	}
	if (bShouldCreate)
	{
		const bool bCreateDirectory = std::filesystem::create_directories(ToParentPath, Error);
		if (Error)
		{
			PushLog(_T("!!Error: Error occurred while creating directory \"%s\"\n"), ToParentPath.string<TCHAR>().c_str());
			return false;
		}
		if (!bCreateDirectory)
		{
			PushLog(_T("!!Error: Failed to create directory \"%s\"\n"), ToParentPath.string<TCHAR>().c_str());
			return false;
		}
	}

	FilePtr ToFile(ToPath.string<TCHAR>().c_str(), _T("wb"));
	if (!ToFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), ToPath.string<TCHAR>().c_str());
		return false;
	}

	while (const size_t ReadSize = fread_s(__hidden_File::CopyBuffer, sizeof(__hidden_File::CopyBuffer), sizeof(unsigned char), sizeof(__hidden_File::CopyBuffer), FromFile.Get()))
	{
		if (ReadSize <= 0)
		{
			return true;
		}

		if (fwrite(__hidden_File::CopyBuffer, sizeof(unsigned char), ReadSize, ToFile.Get()) != ReadSize)
		{
			PushLog(_T("!!Error: Failed to write \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), ToPath.string<TCHAR>().c_str());
			return false;
		}
	}

	if (!FromFile.CloseWithReturn())
	{
		PushLog(_T("!!Error: Cannot close file \"%s\"\n"), FromPath.string<TCHAR>().c_str());
		return false;
	}
	if (!ToFile.CloseWithReturn())
	{
		PushLog(_T("!!Error: Cannot close file \"%s\"\n"), ToPath.string<TCHAR>().c_str());
		return false;
	}

	return true;
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
	
	while (const size_t Read = fread_s(__hidden_Hash::TmpMover, __hidden_Hash::ReadHashSize, sizeof(unsigned char), __hidden_Hash::ReadHashSize, File.Get()))
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
	std::error_code Error;
	
	PathSet PathsToExclude;
	std::deque<std::filesystem::path> PathsToHashMaking;
	
	const std::filesystem::path ListPath(SrcPath / ListFileName);
	FilePtr ListFile(ListPath, _T("rt, ccs=UTF-8"));
	if (!ListFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), ListPath.string<TCHAR>().c_str());
		return;
	}

	const std::filesystem::path HashPath(SrcPath / HashFileName);
	std::filesystem::remove(HashPath, Error);
	if (Error)
	{
		PushLog(_T("!!Error: Cannot delete \"%s\"\n"), HashPath.string<TCHAR>().c_str());
		return;
	}
	
	FilePtr HashFile(HashPath, _T("wt, ccs=UTF-8"));
	if (!HashFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), HashPath.string<TCHAR>().c_str());
		return;
	}

	size_t TotalErrorCount = 0;

	{
		PushLog(_T("\n* Read file list to making hash:\n"));
		size_t LocalErrorCount = 0;
		
		SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());
		while (!feof(ListFile.Get()))
		{
			bool bExclude;
			std::filesystem::path CurPath(ReadFileLine(ListFile, bExclude));
			if (CurPath.empty())
			{
				continue;
			}
			const bool bExists = std::filesystem::exists(CurPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Cannot check the existence of \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (!bExists)
			{
				PushLog(_T("!!Error: No such file or directory \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}

			bool bIsDirectory = std::filesystem::is_directory(CurPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Cannot check if \"%s\" directory\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (bIsDirectory)
			{
				for (auto const& ChildPath : std::filesystem::recursive_directory_iterator{ CurPath })
				{
					bIsDirectory = std::filesystem::is_directory(ChildPath, Error);
					if (Error)
					{
						PushLog(_T("!!Error: Cannot check if \"%s\" directory\n"), ChildPath.path().string<TCHAR>().c_str());
						++LocalErrorCount;
						continue;
					}
					if (bIsDirectory)
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
		if (!ListFile.CloseWithReturn())
		{
			PushLog(_T("!!Error: Failed to close file \"%s\"\n"), ListPath.string<TCHAR>().c_str());
			++LocalErrorCount;
		}

		if (LocalErrorCount > 0)
		{
			PushLog(_T("* %u error occurred\n"), static_cast<unsigned>(LocalErrorCount));
			TotalErrorCount += LocalErrorCount;
		}
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
		PushLog(_T("\n* Following files will be hashed:\n"));
		
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
		PushLog(_T("\n* Hash making started:\n"));
		size_t LocalErrorCount = 0;
		
		std::basic_string<TCHAR> TmpString;
		for (const auto& Path : PathsToHashMaking)
		{
			RawHash Hash;
			{
				FilePtr CurFile(Path, _T("rb"));
				if (!CurFile)
				{
					PushLog(_T("!!Error: Cannot open \"%s\"\n"), Path.string<TCHAR>().c_str());
					memset(Hash.Raw, 0xff, sizeof(RawHash::Raw));
					++LocalErrorCount;
				}
				else
				{
					ConvertToHash(CurFile, Hash);
				}

				if (!CurFile.CloseWithReturn())
				{
					PushLog(_T("!!Error: Failed to close file \"%s\"\n"), Path.string<TCHAR>().c_str());
					++LocalErrorCount;
				}
			}

			TmpString = std::filesystem::relative(Path, SrcPath, Error).string<TCHAR>();
			if (Error)
			{
				PushLog(_T("!!Error: Failed to calculate relative path of \"%s\"\n"), Path.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			
			TmpString += _T("\n");
			TmpString += ConvertToString(Hash);
			TmpString += _T("\n");

			_fputts(TmpString.c_str(), HashFile.Get());
		}
		if (!HashFile.CloseWithReturn())
		{
			PushLog(_T("!!Error: Failed to close file \"%s\"\n"), HashPath.string<TCHAR>().c_str());
			++LocalErrorCount;
		}

		if (LocalErrorCount > 0)
		{
			PushLog(_T("* %u error occurred\n"), static_cast<unsigned>(LocalErrorCount));
			TotalErrorCount += LocalErrorCount;
		}
		PushLog(_T("* Done\n"));
	}

	if (TotalErrorCount > 0)
	{
		PushLog(_T("\n* %u error occurred in total\n"), static_cast<unsigned>(TotalErrorCount));
	}
	else
	{
		PushLog(_T("\n* All tasks done successfully\n"));
	}
}

void CopyPackage(const std::filesystem::path& SrcPath, const std::filesystem::path& DestPath)
{
	std::error_code Error;
	
	const std::filesystem::path ListPath(SrcPath / ListFileName);
	FilePtr ListFile(ListPath, _T("rt, ccs=UTF-8"));
	if (!ListFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), ListPath.string<TCHAR>().c_str());
		return;
	}

	const std::filesystem::path SrcHashPath(SrcPath / HashFileName);
	FilePtr SrcHashFile(SrcHashPath, _T("rt, ccs=UTF-8"));
	if (!SrcHashFile)
	{
		PushLog(_T("!!Error: Cannot open \"%s\"\n"), SrcHashPath.string<TCHAR>().c_str());
		return;
	}
	
	const std::filesystem::path DestHashPath(DestPath / HashFileName);
	FilePtr DestHashFile(DestHashPath, _T("rt, ccs=UTF-8"));

	size_t TotalErrorCount = 0;

	std::deque<std::filesystem::path> ExcludeForDeletion;
	{
		PushLog(_T("\n* Read list for excluding from update:\n"));
		size_t LocalErrorCount = 0;
		
		SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());
		
		while (!feof(ListFile.Get()))
		{
			bool bExclude;
			std::filesystem::path CurPath(ReadFileLine(ListFile, bExclude));
			if (CurPath.empty())
			{
				continue;
			}
			if (!bExclude)
			{
				continue;
			}
			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, SrcPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Failed to calculate relative path of \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			
			ExcludeForDeletion.emplace_back(std::move(RelativePath));
		}
		if (!ListFile.CloseWithReturn())
		{
			PushLog(_T("!!Error: Failed to close file \"%s\"\n"), ListPath.string<TCHAR>().c_str());
			++LocalErrorCount;
		}

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
		PushLog(_T("\n* Read destination file hash:\n"));
		size_t LocalErrorCount = 0;
		
		SetCurrentDirectory(DestPath.string<TCHAR>().c_str());

		while (!feof(DestHashFile.Get()))
		{
			std::filesystem::path CurPath(ReadFileLine(DestHashFile));
			if (CurPath.empty())
			{
				continue;
			}
			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, DestPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Failed to calculate relative path of \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (CheckIfFileContained(ExcludeForDeletion, RelativePath))
			{
				continue;
			}

			RawHash CurHash;
			if (!ConvertToHash(ReadFileStringLine(DestHashFile), CurHash))
			{
				PushLog(_T("!!Error: Invalid hash format \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}

			DestHashes.emplace(std::move(RelativePath), std::move(CurHash));
		}
		if (!DestHashFile.CloseWithReturn())
		{
			PushLog(_T("!!Error: Failed to close file \"%s\"\n"), DestHashPath.string<TCHAR>().c_str());
			++LocalErrorCount;
		}

		if (LocalErrorCount > 0)
		{
			PushLog(_T("* %u error occurred\n"), static_cast<unsigned>(LocalErrorCount));
			TotalErrorCount += LocalErrorCount;
		}
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
		PushLog(_T("\n* Read source file hash:\n"));
		size_t LocalErrorCount = 0;
		
		SetCurrentDirectory(SrcPath.string<TCHAR>().c_str());

		while (!feof(SrcHashFile.Get()))
		{
			std::filesystem::path CurPath(ReadFileLine(SrcHashFile));
			if (CurPath.empty())
			{
				continue;
			}
			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, SrcPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Failed to calculate relative path of \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (CheckIfFileContained(ExcludeForDeletion, RelativePath))
			{
				continue;
			}

			RawHash CurHash;
			if (!ConvertToHash(ReadFileStringLine(SrcHashFile), CurHash))
			{
				PushLog(_T("!!Error: Invalid hash format \"%s\"\n"), CurPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}

			SrcHashes.emplace(std::move(RelativePath), std::move(CurHash));
		}
		if (!SrcHashFile.CloseWithReturn())
		{
			PushLog(_T("!!Error: Failed to close file \"%s\"\n"), SrcHashPath.string<TCHAR>().c_str());
			++LocalErrorCount;
		}

		if (LocalErrorCount > 0)
		{
			PushLog(_T("* %u error occurred\n"), static_cast<unsigned>(LocalErrorCount));
			TotalErrorCount += LocalErrorCount;
		}
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
		PushLog(_T("\n* Remove files or directories that no longer exist on source location:\n"));
		size_t LocalErrorCount = 0;
		
		size_t NumDeleted = 0;

		SetCurrentDirectory(DestPath.string<TCHAR>().c_str());
		
		for (auto const& CurPath : std::filesystem::recursive_directory_iterator{ DestPath })
		{
			bool bIsDirectory = std::filesystem::is_directory(CurPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Cannot check if \"%s\" directory\n"), CurPath.path().string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (bIsDirectory)
			{
				continue;
			}

			std::filesystem::path RelativePath = std::filesystem::relative(CurPath, DestPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Failed to calculate relative path of \"%s\"\n"), CurPath.path().string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (CheckIfFileContained(ExcludeForDeletion, RelativePath))
			{
				continue;
			}

			const bool bEqual = std::filesystem::equivalent(RelativePath, HashFileName, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Failed to check if \"%s\" is \"%s\"\n"), RelativePath.string<TCHAR>().c_str(), HashFileName);
				++LocalErrorCount;
				continue;
			}
			if (bEqual)
			{
				continue;
			}

			if (SrcHashes.find(RelativePath) == SrcHashes.end())
			{
				const bool bRemoved = std::filesystem::remove(RelativePath, Error);
				if (Error || (!bRemoved))
				{
					PushLog(_T("!!Error: Failed to remove \"%s\"\n"), RelativePath.string<TCHAR>().c_str());
					++LocalErrorCount;
				}
				else if (bRemoved)
				{
					++NumDeleted;
					PushLog(_T("%s\n"), RelativePath.string<TCHAR>().c_str());
				}
			}

			std::filesystem::path ParentPath = CurPath.path().parent_path();
			bool bAllowToContinue = false;
			{
				bIsDirectory = std::filesystem::is_directory(ParentPath, Error);
				if (Error)
				{
					PushLog(_T("!!Error: Cannot check if \"%s\" directory\n"), ParentPath.string<TCHAR>().c_str());
					++LocalErrorCount;
					continue;
				}
				if (bIsDirectory)
				{
					const bool bIsEmpty = std::filesystem::is_empty(ParentPath, Error);
					if (Error)
					{
						PushLog(_T("!!Error: Cannot check if \"%s\" empty\n"), ParentPath.string<TCHAR>().c_str());
						++LocalErrorCount;
						continue;
					}
					if (bIsEmpty)
					{
						bAllowToContinue = true;
					}
				}
			}
			
			if (bAllowToContinue)
			{
				const std::filesystem::path RelativeParentPath = std::filesystem::relative(ParentPath, DestPath, Error);
				if (Error)
				{
					PushLog(_T("!!Error: Failed to calculate relative path of \"%s\"\n"), ParentPath.string<TCHAR>().c_str());
					++LocalErrorCount;
					continue;
				}
				
				const std::filesystem::path SrcParentPath = SrcPath / RelativeParentPath;

				const bool bExists = std::filesystem::exists(SrcParentPath, Error);
				if (Error)
				{
					PushLog(_T("!!Error: Cannot check the existence of \"%s\"\n"), SrcParentPath.string<TCHAR>().c_str());
					++LocalErrorCount;
					continue;
				}
				if (bExists)
				{
					bIsDirectory = std::filesystem::is_directory(SrcParentPath, Error);
					if (Error)
					{
						PushLog(_T("!!Error: Cannot check if \"%s\" directory\n"), SrcParentPath.string<TCHAR>().c_str());
						++LocalErrorCount;
						continue;
					}
					if (bIsDirectory)
					{
						continue;
					}
				}

				const bool bRemoved = std::filesystem::remove(ParentPath, Error);
				if (Error || (!bRemoved))
				{
					PushLog(_T("!!Error: Failed to remove \"%s\"\n"), ParentPath.string<TCHAR>().c_str());
					++LocalErrorCount;
				}
				else if (bRemoved)
				{
					++NumDeleted;
					PushLog(_T("%s\n"), ParentPath.string<TCHAR>().c_str());
				}
			}
		}

		if (LocalErrorCount > 0)
		{
			PushLog(_T("* %u error occurred\n"), static_cast<unsigned>(LocalErrorCount));
			TotalErrorCount += LocalErrorCount;
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
		PushLog(_T("\n* Collect files which need update:\n"));
		
		const size_t TotalCount = SrcHashes.size();

		if (!DestHashes.empty())
		{
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

				if (memcmp(std::get<0>(Wrapped)->second.Raw, Found->second.Raw, sizeof(RawHash::Raw)) == 0)
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
		}

		const size_t UpdateCount = SrcHashes.size();

		if (UpdateCount <= 0)
		{
			PushLog(_T("* No files need update\n"));	
		}
		else
		{
			PushLog(_T("* (%u/%u) file need update\n"), static_cast<unsigned>(UpdateCount), static_cast<unsigned>(TotalCount));
		}
	}
	
	if (!SrcHashes.empty())
	{
		PushLog(_T("\n* Update started:\n"));
		size_t LocalErrorCount = 0;

		for (const auto& Wrapped : SrcHashes)
		{
			std::filesystem::path FromPath = SrcPath / Wrapped.first;
			std::filesystem::path ToPath = DestPath / Wrapped.first;

			bool bIsSymbolic = std::filesystem::is_symlink(FromPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Failed to check if \"%s\" symbolic link\n"), FromPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (bIsSymbolic)
			{
				std::filesystem::path OrgPath = std::filesystem::read_symlink(FromPath, Error);
				if (Error)
				{
					PushLog(_T("!!Error: Failed to read symbolic link \"%s\"\n"), FromPath.string<TCHAR>().c_str());
					++LocalErrorCount;
					continue;
				}
				
				PushLog(_T("Symbolic link conversion: \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), OrgPath.string<TCHAR>().c_str());
				FromPath = std::move(OrgPath);
			}

			const bool bExists = std::filesystem::exists(ToPath, Error);
			if (Error)
			{
				PushLog(_T("!!Error: Cannot check the existence of \"%s\"\n"), ToPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			if (bExists)
			{
				bIsSymbolic = std::filesystem::is_symlink(ToPath, Error);
				if (Error)
				{
					PushLog(_T("!!Error: Failed to check if \"%s\" symbolic link\n"), ToPath.string<TCHAR>().c_str());
					++LocalErrorCount;
					continue;
				}
				if (bIsSymbolic)
				{
					std::filesystem::path OrgPath = std::filesystem::read_symlink(ToPath, Error);
					if (Error)
					{
						PushLog(_T("!!Error: Failed to read symbolic link \"%s\"\n"), ToPath.string<TCHAR>().c_str());
						++LocalErrorCount;
						continue;
					}

					PushLog(_T("Symbolic link conversion: \"%s\" to \"%s\"\n"), ToPath.string<TCHAR>().c_str(), OrgPath.string<TCHAR>().c_str());
					ToPath = std::move(OrgPath);
				}
			}
			
			if (!BufferFileCopy(FromPath, ToPath))
			{
				PushLog(_T("!!Error: Failed to copy from \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), ToPath.string<TCHAR>().c_str());
				++LocalErrorCount;
				continue;
			}
			else
			{
				PushLog(_T("File copied from \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), ToPath.string<TCHAR>().c_str());
			}
		}

		if (LocalErrorCount > 0)
		{
			PushLog(_T("* %u error occurred\n"), static_cast<unsigned>(LocalErrorCount));
			TotalErrorCount += LocalErrorCount;
		}
		PushLog(_T("* Done\n"));
	}

	if (TotalErrorCount <= 0)
	{
		PushLog(_T("\n* Update hash list:\n"));
		size_t LocalErrorCount = 0;
		
		const std::filesystem::path FromPath = SrcPath / HashFileName;
		const std::filesystem::path ToPath = DestPath / HashFileName;
		
		if (!BufferFileCopy(FromPath, ToPath))
		{
			PushLog(_T("!!Error: Failed to copy from \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), ToPath.string<TCHAR>().c_str());
			++LocalErrorCount;
		}
		else
		{
			PushLog(_T("File copied from \"%s\" to \"%s\"\n"), FromPath.string<TCHAR>().c_str(), ToPath.string<TCHAR>().c_str());
		}

		if (LocalErrorCount > 0)
		{
			PushLog(_T("* %u error occurred\n"), static_cast<unsigned>(LocalErrorCount));
			TotalErrorCount += LocalErrorCount;
		}
		PushLog(_T("* Done\n"));
	}

	if (TotalErrorCount > 0)
	{
		PushLog(_T("\n* %u error occurred in total\n"), static_cast<unsigned>(TotalErrorCount));
	}
	else
	{
		PushLog(_T("\n* All tasks done successfully\n"));
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int _tmain(int Argc, TCHAR* Argv[])
{
	std::error_code Error;

	std::locale::global(std::locale(".UTF-8"));
	
	switch(Argc)
	{
	case 2:
	{
		std::filesystem::path SrcPath(Argv[1]);
		SrcPath = std::filesystem::canonical(SrcPath, Error);
		if (Error)
		{
			_tprintf_s(_T("!!Error: Error occurred while canonicalizing \"%s\"\n"), Argv[1]);
			return -1;
		}
		const bool bIsDirectory = std::filesystem::is_directory(SrcPath, Error);
		if (Error)
		{
			_tprintf_s(_T("!!Error: Cannot check if \"%s\" directory\n"), SrcPath.string<TCHAR>().c_str());
			return -1;
		}
		if (!bIsDirectory)
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
		std::filesystem::path SrcPath(Argv[1]);
		SrcPath = std::filesystem::canonical(SrcPath, Error);
		if (Error)
		{
			_tprintf_s(_T("!!Error: Error occurred while canonicalizing \"%s\"\n"), Argv[1]);
			return -1;
		}
		bool bIsDirectory = std::filesystem::is_directory(SrcPath, Error);
		if (Error)
		{
			_tprintf_s(_T("!!Error: Cannot check if \"%s\" directory\n"), SrcPath.string<TCHAR>().c_str());
			return -1;
		}
		if (!bIsDirectory)
		{
			_tprintf_s(_T("!!Error: No such directory \"%s\"\n"), SrcPath.string<TCHAR>().c_str());
			return -1;
		}

		std::filesystem::path DestPath(Argv[2]);
		DestPath = std::filesystem::canonical(DestPath, Error);
		if (Error)
		{
			_tprintf_s(_T("!!Error: Error occurred while canonicalizing \"%s\"\n"), Argv[2]);
			return -1;
		}
		bIsDirectory = std::filesystem::is_directory(DestPath, Error);
		if (Error)
		{
			_tprintf_s(_T("!!Error: Cannot check if \"%s\" directory\n"), DestPath.string<TCHAR>().c_str());
			return -1;
		}
		if (!bIsDirectory)
		{
			_tprintf_s(_T("!!Error: No such directory \"%s\"\n"), DestPath.string<TCHAR>().c_str());
			return -1;
		}

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
	{
		_tprintf_s(_T("exe [src]: Read copy list named \"%s\"\n"), ListFileName);
		_tprintf_s(_T("exe [src] [dest]: Copy \"Src\" into \"Dest\" based on \"%s\" which defined at \"Src\". By comparing hash value, only different file will be updated.\n"), ListFileName);
	}
	break;
	}
	
	return 0;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

