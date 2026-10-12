#include "CvGameCoreDLL.h"

#include "BetterBTSAI.h"

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
	const char* const WTP_LOG_TOPIC_NAMES[NUM_WTP_LOG_TOPICS] = { "status", "buy", "settler", "ship", "plan", "military" };

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

	void wtpLogWrite(const CvString& szFile, const char* szText)
	{
		static char buf[2100];
		_snprintf(buf, 2100 - 4, "T%03d %s", GC.getGameINLINE().getGameTurn(), szText);
		gDLL->logMsg(szFile.c_str(), buf);
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
