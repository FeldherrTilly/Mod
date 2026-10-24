#pragma once

#ifndef BETTERBTSAI_H
#define BETTERBTSAI_H

// AI decision making logging

/*  advc.mak: Uncomment to enable BBAI logging. NB: Should use this mostly in
	Debug mode b/c of K-Mod's CvPlayer::concealUnknownCivs. */
#define LOG_AI
// Log levels:
// 0 - None
// 1 - Important decisions only
// 2 - Many decisions
// 3 - All logging
#ifdef LOG_AI
#define gPlayerLogLevel 3
#define gTeamLogLevel   3
#define gCityLogLevel   3
#define gUnitLogLevel   3
#define gMapLogLevel    3 // K-Mod
#else
#define gPlayerLogLevel 0
#define gTeamLogLevel   0
#define gCityLogLevel   0
#define gUnitLogLevel   0
#define gMapLogLevel    0 // K-Mod
#endif

void logBBAI(char const* format, ... );

// WTP: the WTP AI logs are split by topic and player (one file each, e.g. WTP_P14_Danish_ship.log) and by colony
// (WTP_P14_Danish_colony_Trankebar.log), so that a single topic can be read without the whole BBAI.log
enum WtpLogTopic
{
	WTP_LOG_STATUS,
	WTP_LOG_BUY,
	WTP_LOG_SETTLER,
	WTP_LOG_SHIP,
	WTP_LOG_PLAN,
	WTP_LOG_MILITARY,
	WTP_LOG_TRADE,
	WTP_LOG_DIAG, // workforce plan diagnosis: every plot's job alternatives, every colonist's options
	WTP_LOG_HUMAN, // the human player's actions with their situation (CvHumanLog.h)
	WTP_LOG_PIONEER, // the AI pioneers: each new task with its reason, finished improvements
	NUM_WTP_LOG_TOPICS
};
class CvCity;
void logWTP(WtpLogTopic eTopic, PlayerTypes ePlayer, char const* format, ... );
void logWTPCity(const CvCity& kCity, char const* format, ... );

#endif  //BETTERBTSAI_H
