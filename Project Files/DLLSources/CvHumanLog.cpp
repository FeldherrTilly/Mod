// WTP: log of the human players' actions, see CvHumanLog.h

#include "CvGameCoreDLL.h"
#include "CvHumanLog.h"
#include "BetterBTSAI.h"
#include "CombatOdds.h"
#include "CvUnit.h"
#include "CvPlot.h"
#include "CvCity.h"
#include "CvGlobals.h"
#include "CvGameCoreUtils.h"
#include "CvGameAI.h"
#include "CvMap.h"
#include "CvPlayerAI.h"
#include "CvTeamAI.h"
#include "CvSelectionGroupAI.h"
#include "CvInfos.h"

namespace
{
	const char* const TASK_NAMES[] = { "RAZE", "GIFT", "SET_ALL_CITIZENS_AUTOMATED", "SET_CITIZEN_AUTOMATED", "SET_AUTOMATED_PRODUCTION",
		"SET_EMPHASIZE", "CHANGE_WORKING_PLOT", "CHANGE_PROFESSION", "EJECT_TO_TRANSPORT", "REPLACE_CITIZEN", "CLEAR_WORKING_OVERRIDE",
		"HURRY", "CLEAR_ORDERS", "RALLY_PLOT", "CLEAR_RALLY_PLOT", "LIBERATE", "YIELD_IMPORT", "YIELD_EXPORT", "YIELD_LEVEL",
		"CLEAR_SPECIALTY", "EDUCATE", "PUSH_TRAIN_UNIT", "PUSH_CONSTRUCT_BUILDING", "CHEAT", "CHANGE_WORKING_BUILDING",
		"CHANGE_CUSTOM_HOUSE_SETTINGS", "CHANGE_ORDERED_STUDENTS", "YIELD_TRADEROUTE", "AUTO_TRADEROUTE", "TRADEROUTE_PRESET",
		"CHOOSE_CITY_PLOT_YIELD", "GRANT_FREEDOM" };
	// TASK_NAMES: one name per TaskTypes value

	const char* const ACTION_NAMES[] = { "BUY_EUROPE_UNIT", "SELL_YIELD_UNIT", "BUY_YIELD_UNIT", "LOAD_UNIT_FROM_EUROPE",
		"UNLOAD_UNIT_TO_EUROPE", "TRANSFER_UNIT_IN_EUROPE", "SET_OPTION", "AUTO_MOVES", "TURN_COMPLETE", "APPLY_EVENT",
		"CONVINCE_FATHER", "SET_CIVIC", "RECEIVE_GOODY", "BUY_UNITS_FROM_KING", "HURRY_IMMIGRANT", "EUROPE_CHANGE_PROFESSION", "FEAT",
		"BUY_AFRICA_UNIT", "SELL_YIELD_UNIT_AFRICA", "BUY_YIELD_UNIT_AFRICA", "LOAD_UNIT_FROM_AFRICA", "UNLOAD_UNIT_TO_AFRICA",
		"TRANSFER_UNIT_IN_AFRICA", "BUY_PORT_ROYAL_UNIT", "SELL_YIELD_UNIT_PORT_ROYAL", "BUY_YIELD_UNIT_PORT_ROYAL",
		"LOAD_UNIT_FROM_PORT_ROYAL", "UNLOAD_UNIT_TO_PORT_ROYAL", "TRANSFER_UNIT_IN_PORT_ROYAL", "NETWORK_DESYNC_LOG_WRITE",
		"NETWORK_canDoEvent", "NETWORK_canDoGoody", "ASYNC_RANDOM", "UPDATE_NATIVE_BARGAIN_EFFECT", "SET_DIPLO_YIELD_AMOUNT" };
	// ACTION_NAMES: one name per PlayerActionTypes value

	const char* const TRADE_ITEM_NAMES[] = { "GOLD", "MAPS", "OPEN_BORDERS", "DEFENSIVE_PACT", "PERMANENT_ALLIANCE", "PEACE_TREATY",
		"YIELD", "CITIES", "PEACE", "WAR", "EMBARGO" };
	BOOST_STATIC_ASSERT(sizeof(TRADE_ITEM_NAMES) / sizeof(TRADE_ITEM_NAMES[0]) == NUM_TRADEABLE_ITEMS);

	// the type name without its prefix (YIELD_FOOD -> FOOD)
	const char* shortType(const char* szType)
	{
		const char* const szCut = (szType != NULL) ? strchr(szType, '_') : NULL;
		return (szCut != NULL) ? szCut + 1 : (szType != NULL ? szType : "?");
	}

	CvString str(const wchar* szText)
	{
		CvString szResult;
		szResult.Format("%S", szText != NULL ? szText : L"");
		return szResult;
	}

	CvString unitText(const CvUnit& kUnit)
	{
		CvString szProfession = (kUnit.getProfession() != NO_PROFESSION) ? CvString::format("/%s", shortType(GC.getProfessionInfo(kUnit.getProfession()).getType())) : CvString();
		CvString szText = CvString::format("[%s%s id=%d str=%d.%02d hp=%d/%d mv=%d/%d xp=%d lvl=%d", shortType(kUnit.getUnitInfo().getType()), szProfession.c_str(),
			kUnit.getID(), kUnit.currCombatStr(NULL, NULL) / 100, kUnit.currCombatStr(NULL, NULL) % 100, kUnit.currHitPoints(), kUnit.maxHitPoints(),
			kUnit.movesLeft(), kUnit.maxMoves(), kUnit.getExperience(), kUnit.getLevel());
		if (kUnit.getYield() != NO_YIELD && kUnit.getYieldStored() > 0)
		{
			szText += CvString::format(" cargo=%s:%d", shortType(GC.getYieldInfo(kUnit.getYield()).getType()), kUnit.getYieldStored());
		}
		if (kUnit.getCargo() > 0)
		{
			szText += CvString::format(" carrying=%d", kUnit.getCargo());
		}
		szText += "]";
		return szText;
	}

	CvString groupText(const CvSelectionGroup& kGroup)
	{
		CvString szText = CvString::format("group(%d) at %d,%d:", kGroup.getNumUnits(), kGroup.getX(), kGroup.getY());
		int iCount = 0;
		for (CLLNode<IDInfo>* pNode = kGroup.headUnitNode(); pNode != NULL; pNode = kGroup.nextUnitNode(pNode))
		{
			const CvUnit* const pUnit = ::getUnit(pNode->m_data);
			if (pUnit != NULL)
			{
				if (++iCount > 12)
				{
					szText += " ...";
					break;
				}
				szText += " " + unitText(*pUnit);
			}
		}
		return szText;
	}

	CvString plotText(const CvPlot& kPlot, PlayerTypes ePlayer)
	{
		CvString szText = CvString::format("plot %d,%d %s", kPlot.getX_INLINE(), kPlot.getY_INLINE(),
			kPlot.isWater() ? "water" : (kPlot.isPeak() ? "peak" : (kPlot.isHills() ? "hills" : "flat")));
		if (kPlot.getTerrainType() != NO_TERRAIN)
		{
			szText += CvString::format(" %s", shortType(GC.getTerrainInfo(kPlot.getTerrainType()).getType()));
		}
		if (kPlot.getFeatureType() != NO_FEATURE)
		{
			szText += CvString::format(" %s", shortType(GC.getFeatureInfo(kPlot.getFeatureType()).getType()));
		}
		if (kPlot.isRiver())
		{
			szText += " river";
		}
		szText += CvString::format(" def=%d owner=%d", kPlot.defenseModifier(GET_PLAYER(ePlayer).getTeam()), (int)kPlot.getOwnerINLINE());
		if (kPlot.isCity())
		{
			szText += " city=" + str(kPlot.getPlotCity()->getName().GetCString());
		}
		const int iEnemies = kPlot.plotCount(PUF_isEnemy, ePlayer, false);
		if (iEnemies > 0 && kPlot.isVisible(GET_PLAYER(ePlayer).getTeam(), false))
		{
			szText += CvString::format(" enemies=%d", iEnemies);
		}
		return szText;
	}

	CvString yieldsText(const CvCity& kCity)
	{
		CvString szText;
		for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_YIELD_TYPES; ++eYield)
		{
			const int iStored = kCity.getYieldStored(eYield);
			const int iNet = kCity.calculateNetYield(eYield);
			if (iStored != 0 || iNet != 0)
			{
				szText += CvString::format(" %s=%d%+d", shortType(GC.getYieldInfo(eYield).getType()), iStored, iNet);
			}
		}
		return szText;
	}

	CvString cityText(const CvCity& kCity)
	{
		const CvPlot* const pPlot = kCity.plot();
		return CvString::format("city %s (%d,%d) pop=%d prod=%s %d/%d defenders=%d rebel=%d%% cap=%d", str(kCity.getName().GetCString()).c_str(),
			kCity.getX_INLINE(), kCity.getY_INLINE(), kCity.getPopulation(), str(kCity.getProductionName()).c_str(), kCity.getProduction(),
			kCity.getProductionNeeded(YIELD_HAMMERS), pPlot->plotCount(PUF_canDefend, -1, -1, kCity.getOwnerINLINE()), kCity.getRebelPercent(),
			kCity.getMaxYieldCapacity()) + " yields:" + yieldsText(kCity);
	}

	CvString playerText(const CvPlayer& kPlayer)
	{
		return CvString::format("gold=%d tax=%d cities=%d pop=%d units=%d", kPlayer.getGold(), kPlayer.getTaxRate(), kPlayer.getNumCities(),
			kPlayer.getTotalPopulation(), kPlayer.getNumUnits());
	}

	// the situation at the target of a move: the danger there, and for an attack the odds and the strengths
	CvString targetText(const CvSelectionGroup& kGroup, const CvPlot& kTarget)
	{
		const PlayerTypes ePlayer = kGroup.getOwnerINLINE();
		const CvPlayerAI& kPlayer = GET_PLAYER(ePlayer);
		CvString szText = CvString::format(" dist=%d danger=%d", plotDistance(kGroup.getX(), kGroup.getY(), kTarget.getX_INLINE(), kTarget.getY_INLINE()),
			kPlayer.AI_getPlotDanger(const_cast<CvPlot*>(&kTarget), 2, false));
		if (kTarget.isVisibleEnemyUnit(ePlayer))
		{
			const CvSelectionGroupAI& kGroupAI = static_cast<const CvSelectionGroupAI&>(kGroup);
			int iOdds = 0;
			const CvUnit* const pAttacker = kGroupAI.AI_getBestGroupAttacker(&kTarget, false, iOdds);
			szText += CvString::format(" attack: odds=%d attacker=%s ratio=%d ourStr=%d enemyStr=%d enemyDefenders=%d", iOdds,
				(pAttacker != NULL) ? unitText(*pAttacker).c_str() : "none", kGroupAI.AI_compareStacks(&kTarget, false),
				kGroupAI.AI_sumStrength(&kTarget, kTarget.isWater() ? DOMAIN_SEA : DOMAIN_LAND), kPlayer.AI_localDefenceStrength(&kTarget, NO_TEAM),
				kTarget.plotCount(PUF_canDefendEnemy, ePlayer, false));
			const CvUnit* const pDefender = kTarget.getBestDefender(NO_PLAYER, ePlayer, pAttacker, true);
			if (pDefender != NULL)
			{
				szText += " defender=" + unitText(*pDefender);
			}
		}
		return szText;
	}

	void write(PlayerTypes ePlayer, const CvString& szText)
	{
		logWTP(WTP_LOG_HUMAN, ePlayer, "%s", szText.c_str());
	}
}

bool HumanLog::isActive(PlayerTypes ePlayer)
{
	return GLOBAL_DEFINE_WTP_LOG_HUMAN_ACTIONS > 0 && ePlayer >= 0 && ePlayer < MAX_PLAYERS && GET_PLAYER(ePlayer).isHuman();
}

void HumanLog::mission(const CvSelectionGroup& kGroup, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bAppend)
{
	if (!isActive(kGroup.getOwnerINLINE()))
	{
		return;
	}
	CvString szText = CvString::format("MISSION %s data=%d,%d flags=%d%s ", (eMission != NO_MISSION) ? shortType(GC.getMissionInfo(eMission).getType()) : "NONE",
		iData1, iData2, iFlags, bAppend ? " append" : "");
	szText += groupText(kGroup);
	const CvPlot* const pFrom = kGroup.plot();
	if (pFrom != NULL)
	{
		szText += " from: " + plotText(*pFrom, kGroup.getOwnerINLINE());
	}
	// missions with a target plot
	if (GC.getMap().isPlot(iData1, iData2) && (eMission == MISSION_MOVE_TO || eMission == MISSION_ROUTE_TO || eMission == MISSION_BOMBARD))
	{
		const CvPlot* const pTarget = GC.getMap().plotINLINE(iData1, iData2);
		szText += " to: " + plotText(*pTarget, kGroup.getOwnerINLINE()) + targetText(kGroup, *pTarget);
	}
	else if (eMission == MISSION_BUILD && iData1 >= 0 && iData1 < GC.getNumBuildInfos())
	{
		szText += CvString::format(" build=%s", shortType(GC.getBuildInfo((BuildTypes)iData1).getType()));
	}
	write(kGroup.getOwnerINLINE(), szText);
}

void HumanLog::command(const CvUnit& kUnit, CommandTypes eCommand, int iData1, int iData2)
{
	if (!isActive(kUnit.getOwnerINLINE()))
	{
		return;
	}
	CvString szText = CvString::format("COMMAND %s data=%d,%d ", (eCommand != NO_COMMAND) ? shortType(GC.getCommandInfo(eCommand).getType()) : "NONE", iData1, iData2);
	if (eCommand == COMMAND_PROFESSION && iData1 >= 0 && iData1 < GC.getNumProfessionInfos())
	{
		szText += CvString::format("profession=%s ", shortType(GC.getProfessionInfo((ProfessionTypes)iData1).getType()));
	}
	szText += unitText(kUnit);
	if (kUnit.plot() != NULL)
	{
		szText += " at: " + plotText(*kUnit.plot(), kUnit.getOwnerINLINE());
		if (kUnit.plot()->getPlotCity() != NULL && kUnit.plot()->getPlotCity()->getOwnerINLINE() == kUnit.getOwnerINLINE())
		{
			szText += " in: " + cityText(*kUnit.plot()->getPlotCity());
		}
	}
	write(kUnit.getOwnerINLINE(), szText);
}

void HumanLog::cityTask(const CvCity& kCity, TaskTypes eTask, int iData1, int iData2, bool bOption)
{
	if (!isActive(kCity.getOwnerINLINE()))
	{
		return;
	}
	CvString szText = CvString::format("CITY_TASK %s data=%d,%d%s ", ((int)eTask >= 0 && (int)eTask < (int)NUM_TASK_TYPES) ? TASK_NAMES[(int)eTask] : "?", iData1, iData2, bOption ? " option" : "");
	switch (eTask)
	{
	case TASK_CHANGE_PROFESSION:
	case TASK_REPLACE_CITIZEN:
	case TASK_EJECT_TO_TRANSPORT:
	case TASK_CHANGE_WORKING_PLOT:
	case TASK_EDUCATE:
		{
			const CvUnit* const pUnit = GET_PLAYER(kCity.getOwnerINLINE()).getUnit(iData1);
			if (pUnit != NULL)
			{
				szText += "unit=" + unitText(*pUnit) + " ";
			}
			if (eTask == TASK_CHANGE_PROFESSION && iData2 >= 0 && iData2 < GC.getNumProfessionInfos())
			{
				szText += CvString::format("profession=%s ", shortType(GC.getProfessionInfo((ProfessionTypes)iData2).getType()));
			}
		}
		break;
	case TASK_YIELD_IMPORT:
	case TASK_YIELD_EXPORT:
	case TASK_YIELD_LEVEL:
		if (iData1 >= 0 && iData1 < NUM_YIELD_TYPES)
		{
			szText += CvString::format("yield=%s ", shortType(GC.getYieldInfo((YieldTypes)iData1).getType()));
		}
		break;
	case TASK_PUSH_TRAIN_UNIT:
		if (iData1 >= 0 && iData1 < GC.getNumUnitInfos())
		{
			szText += CvString::format("unit=%s ", shortType(GC.getUnitInfo((UnitTypes)iData1).getType()));
		}
		break;
	case TASK_PUSH_CONSTRUCT_BUILDING:
		if (iData1 >= 0 && iData1 < GC.getNumBuildingInfos())
		{
			szText += CvString::format("building=%s ", shortType(GC.getBuildingInfo((BuildingTypes)iData1).getType()));
		}
		break;
	default:
		break;
	}
	szText += cityText(kCity) + " " + playerText(GET_PLAYER(kCity.getOwnerINLINE()));
	write(kCity.getOwnerINLINE(), szText);
}

void HumanLog::cityOrder(const CvCity& kCity, const char* szWhat, OrderTypes eOrder, int iData1, bool bAppend)
{
	if (!isActive(kCity.getOwnerINLINE()))
	{
		return;
	}
	CvString szItem = CvString::format("%d", iData1);
	if (eOrder == ORDER_TRAIN && iData1 >= 0 && iData1 < GC.getNumUnitInfos())
	{
		szItem = shortType(GC.getUnitInfo((UnitTypes)iData1).getType());
	}
	else if (eOrder == ORDER_CONSTRUCT && iData1 >= 0 && iData1 < GC.getNumBuildingInfos())
	{
		szItem = shortType(GC.getBuildingInfo((BuildingTypes)iData1).getType());
	}
	write(kCity.getOwnerINLINE(), CvString::format("CITY_ORDER %s order=%d item=%s%s queue=%d ", szWhat, (int)eOrder, szItem.c_str(), bAppend ? " append" : "",
		kCity.getOrderQueueLength()) + cityText(kCity));
}

void HumanLog::cityPopOrder(const CvCity& kCity, int iNum)
{
	if (isActive(kCity.getOwnerINLINE()))
	{
		write(kCity.getOwnerINLINE(), CvString::format("CITY_ORDER remove index=%d queue=%d ", iNum, kCity.getOrderQueueLength()) + cityText(kCity));
	}
}

// the situation before a player action: what it buys or sells at which price (a sold cargo is gone afterwards)
CvString HumanLog::actionContext(const CvPlayer& kPlayer, PlayerActionTypes eAction, int iData1, int iData2, int iData3)
{
	if (!isActive(kPlayer.getID()))
	{
		return CvString();
	}
	CvString szText;
	switch (eAction)
	{
	case PLAYER_ACTION_BUY_EUROPE_UNIT:
	case PLAYER_ACTION_BUY_AFRICA_UNIT:
	case PLAYER_ACTION_BUY_PORT_ROYAL_UNIT:
		if (iData1 >= 0 && iData1 < GC.getNumUnitInfos())
		{
			szText = CvString::format("unit=%s europePrice=%d ", shortType(GC.getUnitInfo((UnitTypes)iData1).getType()),
				kPlayer.getEuropeUnitBuyPrice((UnitTypes)iData1, false));
		}
		break;
	case PLAYER_ACTION_BUY_YIELD_UNIT:
	case PLAYER_ACTION_BUY_YIELD_UNIT_AFRICA:
	case PLAYER_ACTION_BUY_YIELD_UNIT_PORT_ROYAL:
		if (iData1 >= 0 && iData1 < NUM_YIELD_TYPES)
		{
			szText = CvString::format("yield=%s amount=%d buyPrice=%d sellPrice=%d ", shortType(GC.getYieldInfo((YieldTypes)iData1).getType()), iData2,
				kPlayer.getYieldBuyPrice((YieldTypes)iData1), kPlayer.getYieldSellPrice((YieldTypes)iData1));
		}
		break;
	case PLAYER_ACTION_SELL_YIELD_UNIT:
	case PLAYER_ACTION_SELL_YIELD_UNIT_AFRICA:
	case PLAYER_ACTION_SELL_YIELD_UNIT_PORT_ROYAL:
		{
			const CvUnit* const pUnit = kPlayer.getUnit(iData3);
			if (pUnit != NULL && pUnit->getYield() != NO_YIELD)
			{
				szText = CvString::format("yield=%s amount=%d sellPrice=%d ", shortType(GC.getYieldInfo(pUnit->getYield()).getType()), iData2,
					kPlayer.getYieldSellPrice(pUnit->getYield()));
			}
		}
		break;
	case PLAYER_ACTION_HURRY: // pays for an immigrant on the docks of Europe at once
		if (iData1 >= 0 && iData1 < GC.getNumHurryInfos())
		{
			szText = CvString::format("hurry=%s ", shortType(GC.getHurryInfo((HurryTypes)iData1).getType()));
			const UnitTypes eDockUnit = kPlayer.getDocksNextUnit(iData2);
			if (eDockUnit != NO_UNIT)
			{
				szText += CvString::format("dock=%d immigrant=%s price=%d crosses=%d/%d ", iData2, shortType(GC.getUnitInfo(eDockUnit).getType()),
					kPlayer.getHurryGold((HurryTypes)iData1, iData2), kPlayer.getCrossesStored(), kPlayer.immigrationThreshold());
			}
		}
		break;
	case PLAYER_ACTION_CONVINCE_FATHER:
		if (iData1 >= 0 && iData1 < GC.getNumFatherInfos())
		{
			szText = CvString::format("father=%s ", shortType(GC.getFatherInfo((FatherTypes)iData1).getType()));
		}
		break;
	default:
		break;
	}
	return szText;
}

void HumanLog::playerAction(const CvPlayer& kPlayer, PlayerActionTypes eAction, int iData1, int iData2, int iData3, int iGoldBefore, const CvString& szBefore)
{
	if (!isActive(kPlayer.getID()))
	{
		return;
	}
	// network bookkeeping and interface settings, no decisions
	if (eAction == PLAYER_ACTION_NETWORK_DESYNC_LOG_WRITE || eAction == PLAYER_ACTION_NETWORK_canDoEvent || eAction == PLAYER_ACTION_NETWORK_canDoGoody
		|| eAction == PLAYER_ACTION_ASYNC_RANDOM || eAction == PLAYER_ACTION_SET_OPTION || eAction == PLAYER_ACTION_AUTO_MOVES)
	{
		return;
	}
	CvString szText = CvString::format("ACTION %s data=%d,%d,%d ", ((int)eAction >= 0 && (int)eAction < (int)NUM_PLAYER_ACTION_TYPES) ? ACTION_NAMES[(int)eAction] : "?", iData1, iData2, iData3);
	szText += szBefore;
	szText += CvString::format("goldBefore=%d goldChange=%+d ", iGoldBefore, kPlayer.getGold() - iGoldBefore) + playerText(kPlayer);
	write(kPlayer.getID(), szText);
}

void HumanLog::diploEvent(const CvPlayer& kPlayer, DiploEventTypes eEvent, PlayerTypes eOther, int iData1, int iData2)
{
	if (isActive(kPlayer.getID()))
	{
		write(kPlayer.getID(), CvString::format("DIPLO event=%d with=%d (%s) data=%d,%d ", (int)eEvent, (int)eOther,
			(eOther != NO_PLAYER) ? str(GET_PLAYER(eOther).getCivilizationShortDescription()).c_str() : "", iData1, iData2) + playerText(kPlayer));
	}
}

void HumanLog::deal(PlayerTypes eWho, PlayerTypes eOtherWho, const CLinkList<TradeData>* pOurList, const CLinkList<TradeData>* pTheirList)
{
	const PlayerTypes aePlayers[2] = { eWho, eOtherWho };
	for (int iSide = 0; iSide < 2; ++iSide)
	{
		const PlayerTypes eHuman = aePlayers[iSide];
		if (!isActive(eHuman))
		{
			continue;
		}
		CvString szText = CvString::format("DEAL between %d (%s) and %d (%s):", (int)eWho, str(GET_PLAYER(eWho).getCivilizationShortDescription()).c_str(),
			(int)eOtherWho, str(GET_PLAYER(eOtherWho).getCivilizationShortDescription()).c_str());
		const CLinkList<TradeData>* const apLists[2] = { pOurList, pTheirList };
		for (int iList = 0; iList < 2; ++iList)
		{
			szText += CvString::format(" %d gives:", (int)aePlayers[iList]);
			if (apLists[iList] == NULL)
			{
				continue;
			}
			for (CLLNode<TradeData> const* pNode = apLists[iList]->head(); pNode != NULL; pNode = apLists[iList]->next(pNode))
			{
				const TradeData& kData = pNode->m_data;
				const char* const szItem = (kData.m_eItemType >= 0 && kData.m_eItemType < NUM_TRADEABLE_ITEMS) ? TRADE_ITEM_NAMES[kData.m_eItemType] : "?";
				if (kData.m_eItemType == TRADE_YIELD && kData.m_eYield >= 0 && kData.m_eYield < NUM_YIELD_TYPES)
				{
					szText += CvString::format(" %s=%s", szItem, shortType(GC.getYieldInfo(kData.m_eYield).getType()));
				}
				else
				{
					szText += CvString::format(" %s=%d", szItem, kData.m_iData1);
				}
			}
		}
		write(eHuman, szText + " " + playerText(GET_PLAYER(eHuman)));
	}
}

void HumanLog::combat(const CvUnit& kAttacker, const CvUnit& kDefender, const CvPlot& kPlot, const char* szOutcome)
{
	const PlayerTypes aeSides[2] = { kAttacker.getOwnerINLINE(), kDefender.getOwnerINLINE() };
	for (int iSide = 0; iSide < 2; ++iSide)
	{
		if (isActive(aeSides[iSide]))
		{
			write(aeSides[iSide], CvString::format("COMBAT %s attacker(p%d)=", (iSide == 0) ? "we attacked" : "we were attacked", (int)kAttacker.getOwnerINLINE())
				+ unitText(kAttacker) + CvString::format(" from %d,%d defender(p%d)=", kAttacker.getX_INLINE(), kAttacker.getY_INLINE(), (int)kDefender.getOwnerINLINE())
				+ unitText(kDefender) + " at: " + plotText(kPlot, aeSides[iSide]) + " attacker " + szOutcome);
		}
	}
}

void HumanLog::turnStart(const CvPlayer& kPlayer)
{
	if (!isActive(kPlayer.getID()))
	{
		return;
	}
	const CvTeam& kTeam = GET_TEAM(kPlayer.getTeam());
	CvString szText = "TURN " + playerText(kPlayer) + CvString::format(" bells=%d fatherPoints:", kPlayer.getBellsStored());
	for (int iPoint = 0; iPoint < GC.getNumFatherPointInfos(); ++iPoint)
	{
		szText += CvString::format(" %d", kTeam.getFatherPoints((FatherPointTypes)iPoint));
	}
	write(kPlayer.getID(), szText);
	int iLoop;
	for (const CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL; pCity = kPlayer.nextCity(&iLoop))
	{
		write(kPlayer.getID(), "TURN " + cityText(*pCity));
	}
}
