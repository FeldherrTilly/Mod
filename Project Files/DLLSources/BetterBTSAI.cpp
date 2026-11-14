#include "CvGameCoreDLL.h"

#include "BetterBTSAI.h"
#include "CvInitCore.h"
#include <shlobj.h>
#include <time.h>
#include <string>

// AI decision making logging

void logBBAI(char const* format, ... )
{
#ifdef LOG_AI
	static char buf[2048];
	va_list args;
	va_start(args, format);
	_vsnprintf(buf, 2048-4, format, args);
	va_end(args); // kmodx
	gDLL->logMsg("BBAI.log", buf);
#endif
}

// WTP: split AI logs
namespace
{
	const char* const WTP_LOG_TOPIC_NAMES[NUM_WTP_LOG_TOPICS] = { "status", "buy", "settler", "ship", "plan", "military", "trade", "diag", "human", "pioneer" };

	// file names only take letters and digits, everything else becomes '_'
	CvString wtpLogFileName(const wchar* szName)
	{
		CvString szResult;
		for (const wchar* p = szName; p != NULL && *p != 0; ++p)
		{
			const wchar c = *p;
			const bool bPlain = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
			szResult += bPlain ? (char)c : '_';
		}
		return szResult;
	}

	CvString wtpLogPlayerPrefix(PlayerTypes ePlayer)
	{
		CvString szPrefix;
		if (ePlayer >= 0 && ePlayer < MAX_PLAYERS)
		{
			szPrefix.Format("WTP_P%02d_%s", (int)ePlayer, wtpLogFileName(GET_PLAYER(ePlayer).getCivilizationAdjective(0)).c_str());
		}
		else
		{
			szPrefix = "WTP_none";
		}
		return szPrefix;
	}

	// WTP: every game has a log folder of its own: Logs\WTP_Games\<start date and time>_<map>\ (the start time is saved with
	// the game, a loaded game goes on writing into its folder). The files are kept open; a lock guards against the parallel
	// city code.
	struct WtpLogFiles
	{
		CRITICAL_SECTION kLock;
		int iGameStart;
		std::string szFolder;
		std::map<std::string, FILE*> mapFiles;

		WtpLogFiles() : iGameStart(-1)
		{
			InitializeCriticalSection(&kLock);
		}
		~WtpLogFiles()
		{
			closeAll();
			DeleteCriticalSection(&kLock);
		}
		void closeAll()
		{
			for (std::map<std::string, FILE*>::iterator it = mapFiles.begin(); it != mapFiles.end(); ++it)
			{
				fclose(it->second);
			}
			mapFiles.clear();
		}
		// the folder of the current game, created if needed; empty if the documents folder is not known
		const std::string& folder()
		{
			const int iStart = GC.getGameINLINE().getLogGameStart();
			if (iStart == iGameStart)
			{
				return szFolder;
			}
			closeAll();
			iGameStart = iStart;
			szFolder.clear();
			char szDocuments[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, szDocuments)))
			{
				return szFolder;
			}
			std::string szPath = std::string(szDocuments) + "\\My Games\\Civ4Colonization\\Logs\\WTP_Games";
			CreateDirectoryA(szPath.c_str(), NULL);
			// the start time and the map (the part after the last path separator, at most 40 characters)
			char szTime[32] = "unknown";
			const time_t tStart = (time_t)iStart;
			const struct tm* pTime = localtime(&tStart);
			if (pTime != NULL)
			{
				strftime(szTime, sizeof(szTime), "%Y-%m-%d_%H-%M", pTime);
			}
			CvString szMap = wtpLogFileName(GC.getInitCore().getMapScriptName().GetCString());
			std::string szMapName = szMap.c_str();
			const std::string::size_type iCut = szMapName.find_last_of("\\/");
			if (iCut != std::string::npos)
			{
				szMapName = szMapName.substr(iCut + 1);
			}
			if (szMapName.length() > 40)
			{
				szMapName = szMapName.substr(0, 40);
			}
			szPath += "\\" + std::string(szTime) + (szMapName.empty() ? "" : "_" + szMapName);
			CreateDirectoryA(szPath.c_str(), NULL);
			szFolder = szPath;
			return szFolder;
		}
		void write(const CvString& szFile, const char* szText)
		{
			EnterCriticalSection(&kLock);
			const std::string& szDir = folder();
			if (szDir.empty())
			{
				gDLL->logMsg(szFile.c_str(), szText); // fallback: the old flat Logs folder
			}
			else
			{
				FILE*& pFile = mapFiles[szFile.c_str()];
				if (pFile == NULL)
				{
					pFile = fopen((szDir + "\\" + szFile.c_str()).c_str(), "a");
					// a new session of the game (it may have been loaded from an earlier turn)
					if (pFile != NULL)
					{
						fprintf(pFile, "===== session from turn %d =====\n", GC.getGameINLINE().getGameTurn());
					}
				}
				if (pFile != NULL)
				{
					fputs(szText, pFile);
					fputc('\n', pFile);
					fflush(pFile);
				}
			}
			LeaveCriticalSection(&kLock);
		}
	};
	WtpLogFiles g_kWtpLogFiles;

	void wtpLogWrite(const CvString& szFile, const char* szText)
	{
		static char buf[2100];
		_snprintf(buf, 2100 - 4, "T%03d %s", GC.getGameINLINE().getGameTurn(), szText);
		g_kWtpLogFiles.write(szFile, buf);
	}
}

void logWTP(WtpLogTopic eTopic, PlayerTypes ePlayer, char const* format, ... )
{
#ifdef LOG_AI
	static char buf[2048];
	va_list args;
	va_start(args, format);
	_vsnprintf(buf, 2048-4, format, args);
	va_end(args);
	CvString szFile;
	szFile.Format("%s_%s.log", wtpLogPlayerPrefix(ePlayer).c_str(), WTP_LOG_TOPIC_NAMES[eTopic]);
	wtpLogWrite(szFile, buf);
#endif
}

void logWTPCity(const CvCity& kCity, char const* format, ... )
{
#ifdef LOG_AI
	static char buf[2048];
	va_list args;
	va_start(args, format);
	_vsnprintf(buf, 2048-4, format, args);
	va_end(args);
	CvString szFile;
	szFile.Format("%s_colony_%s.log", wtpLogPlayerPrefix(kCity.getOwnerINLINE()).c_str(), wtpLogFileName(kCity.getName().GetCString()).c_str());
	wtpLogWrite(szFile, buf);
#endif
}
