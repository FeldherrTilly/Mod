#pragma once

#ifndef CV_HUMAN_LOG_H
#define CV_HUMAN_LOG_H

// WTP: log of the human players' actions (WTP_P00_English_human.log in the game's log folder): every order the player gives
// through the exe (missions, unit commands, city tasks and build lists, Europe and other player actions, diplomacy), the
// trade deals and combats he is part of and a summary at the start of each of his turns - each line with its situation,
// as data to tune the AI by. Switched by the global define WTP_LOG_HUMAN_ACTIONS.

class CvUnit;
class CvCity;
class CvPlayer;
class CvSelectionGroup;
struct TradeData;
template <class tVARTYPE> class CLinkList;

namespace HumanLog
{
	bool isActive(PlayerTypes ePlayer);

	void mission(const CvSelectionGroup& kGroup, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bAppend);
	void command(const CvUnit& kUnit, CommandTypes eCommand, int iData1, int iData2);
	void cityTask(const CvCity& kCity, TaskTypes eTask, int iData1, int iData2, bool bOption);
	void cityOrder(const CvCity& kCity, const char* szWhat, OrderTypes eOrder, int iData1, bool bAppend);
	void cityPopOrder(const CvCity& kCity, int iNum);
	CvString actionContext(const CvPlayer& kPlayer, PlayerActionTypes eAction, int iData1, int iData2, int iData3);
	void playerAction(const CvPlayer& kPlayer, PlayerActionTypes eAction, int iData1, int iData2, int iData3, int iGoldBefore, const CvString& szBefore);
	void diploEvent(const CvPlayer& kPlayer, DiploEventTypes eEvent, PlayerTypes eOther, int iData1, int iData2);
	void deal(PlayerTypes eWho, PlayerTypes eOtherWho, const CLinkList<TradeData>* pOurList, const CLinkList<TradeData>* pTheirList);
	void combat(const CvUnit& kAttacker, const CvUnit& kDefender, const CvPlot& kPlot, const char* szOutcome);
	void turnStart(const CvPlayer& kPlayer);
}

#endif
