// playerAI.cpp

#include "CvGameCoreDLL.h"
#include "CvPlayerAI.h"
#include "CvRandom.h"
#include "CvGlobals.h"
#include "CvGameCoreUtils.h"
#include "CvMap.h"
#include "CvArea.h"
#include "CvPlot.h"
#include "CvGameAI.h"
#include "CvTeamAI.h"
#include "CvDiploParameters.h"
#include "CvInitCore.h"
#include "CyArgsList.h"
#include "CvDLLInterfaceIFaceBase.h"
#include "CvDLLEntityIFaceBase.h"
#include "CvDLLPythonIFaceBase.h"
#include "CvDLLEngineIFaceBase.h"
#include "CvDLLEventReporterIFaceBase.h"
#include "CvInfos.h"
#include "CvPopupInfo.h"
#include "FProfiler.h"
#include "CvDLLFAStarIFaceBase.h"
#include "FAStarNode.h"
#include "CvTradeRoute.h"

#include "CvSavegame.h"
#include "BetterBTSAI.h"
#include "DesyncMonitor.h"

#include "TBB.h"

#include "DeferredPopEject.h"

DeferredPopEjectQueue* g_pDeferredPopEjectQ = NULL;

namespace
{
	struct DeferredPopEjectKey
	{
		int iOwner;
		int iCityID;
		int iUnitID;
		CvCity* pCity;
		CvUnit* pUnit;
	};

	struct DeferredPopEjectKeyLess
	{
		bool operator()(const DeferredPopEjectKey& a, const DeferredPopEjectKey& b) const
		{
			if (a.iOwner != b.iOwner)  return a.iOwner < b.iOwner;
			if (a.iCityID != b.iCityID) return a.iCityID < b.iCityID;
			return a.iUnitID < b.iUnitID;
		}
	};

	static void applyDeferredPopEjects(DeferredPopEjectQueue& q)
	{
		std::vector<DeferredPopEjectKey> v;
		v.reserve(128);

		for (;;)
		{
			DeferredPopEject it;
			if (!q.try_pop(it))
				break;

			if (it.pCity == NULL || it.pUnit == NULL)
				continue;

			DeferredPopEjectKey k;
			k.iOwner = (int)it.pCity->getOwnerINLINE();
			k.iCityID = it.pCity->getID();
			k.iUnitID = it.pUnit->getID();
			k.pCity = it.pCity;
			k.pUnit = it.pUnit;
			v.push_back(k);
		}

		if (v.empty())
			return;

		std::sort(v.begin(), v.end(), DeferredPopEjectKeyLess());

		// Process by (owner, city). Enforce "leave 1 colonist" here (authoritative).
		for (size_t i = 0; i < v.size(); )
		{
			const int owner = v[i].iOwner;
			const int cityID = v[i].iCityID;

			size_t j = i + 1;
			while (j < v.size() && v[j].iOwner == owner && v[j].iCityID == cityID)
				++j;

			CvPlayer& kOwner = GET_PLAYER((PlayerTypes)owner);

			CvCity* pCity = v[i].pCity;
			if (pCity == NULL || pCity->getOwnerINLINE() != (PlayerTypes)owner || pCity->getID() != cityID)
			{
				pCity = kOwner.getCity(cityID);
			}

			if (pCity != NULL)
			{
				int maxRemove = pCity->getPopulation() - 1;
				if (maxRemove > 0)
				{
					const int iDefaultProf =
						GC.getCivilizationInfo(kOwner.getCivilizationType()).getDefaultProfession();

					int lastUnitID = -1;

					for (size_t k = i; k < j && maxRemove > 0; ++k)
					{
						// Skip duplicates of same unit within the sorted block.
						if (v[k].iUnitID == lastUnitID)
							continue;
						lastUnitID = v[k].iUnitID;

						CvUnit* pUnit = v[k].pUnit;
						if (pUnit == NULL)
							continue;

						// Cheap eligibility check.
						if (pUnit->getOwnerINLINE() != (PlayerTypes)owner)
							continue;
						if (pUnit->getProfession() != NO_PROFESSION)
							continue;

						// removePopulationUnit is expected to fail safely if the unit is not in this city anymore.
						const bool ok = pCity->removePopulationUnit(
							CREATE_ASSERT_DATA,
							pUnit,
							false,
							(ProfessionTypes)iDefaultProf);

						if (ok)
							--maxRemove;
					}
				}
			}

			i = j;
		}
	}
}

#define DANGER_RANGE				(4)
#define GREATER_FOUND_RANGE			(5)
#define CIVIC_CHANGE_DELAY			(25)

// statics

CvPlayerAI* CvPlayerAI::m_aPlayers = NULL;

void CvPlayerAI::initStatics()
{
	m_aPlayers = new CvPlayerAI[MAX_PLAYERS];
	for (int iI = 0; iI < MAX_PLAYERS; iI++)
	{
		m_aPlayers[iI].m_eID = ((PlayerTypes)iI);
	}
}

void CvPlayerAI::freeStatics()
{
	SAFE_DELETE_ARRAY(m_aPlayers);
}

bool CvPlayerAI::areStaticsInitialized()
{
	if(m_aPlayers == NULL)
	{
		return false;
	}

	return true;
}

DllExport CvPlayerAI& CvPlayerAI::getPlayerNonInl(PlayerTypes ePlayer)
{
	return getPlayer(ePlayer);
}

// Public Functions...

CvPlayerAI::CvPlayerAI()
{
	AI_reset();
}


CvPlayerAI::~CvPlayerAI()
{
	AI_uninit();
	m_aTradeGroups.clear();
}


void CvPlayerAI::AI_init()
{
	AI_reset();

	//--------------------------------
	// Init other game data
	if ((GC.getInitCore().getSlotStatus(getID()) == SS_TAKEN) || (GC.getInitCore().getSlotStatus(getID()) == SS_COMPUTER))
	{
		FAssert(getPersonalityType() != NO_LEADER);
	}
}


void CvPlayerAI::AI_uninit()
{
}


void CvPlayerAI::AI_reset()
{
	AI_uninit();
	AI_resetSavedData();
}

void CvPlayerAI::AI_doTurnPre()
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_doTurnPre (native)" : "CvPlayerAI::AI_doTurnPre");
#endif
	PROFILE_FUNC();

	FAssertMsg(getPersonalityType() != NO_LEADER, "getPersonalityType() is not expected to be equal with NO_LEADER");
	FAssertMsg(getLeaderType() != NO_LEADER, "getLeaderType() is not expected to be equal with NO_LEADER");
	FAssertMsg(getCivilizationType() != NO_CIVILIZATION, "getCivilizationType() is not expected to be equal with NO_CIVILIZATION");

	AI_invalidateCloseBordersAttitudeCache();

	AI_doCounter();

	AI_doEnemyUnitData();

	if (isHuman())
	{
		return;
	}

	m_unitPriorityHeap.clear();
	int iLoop;
	for (CvUnit* pUnit = firstUnit(&iLoop); pUnit != NULL; pUnit = nextUnit(&iLoop))
	{
		pUnit->AI_setMovePriority(0);
	}

	AI_doEmotions();

	AI_doUnitAIWeights();

	AI_doMilitary();

	AI_doStrategy();

	AI_updateYieldValues();

	AI_doCitizenRelocation();

	AI_doPeaceWarships();

	AI_updateFullStorage();

	AI_doWorkforcePlan();

	AI_doReclaimTerritory();

	AI_doSurplusPioneers();

	AI_doNativeMercDefense();

	AI_logStatusSummary();
}

// WTP: per turn summary of a colonial AI, to find bad decisions (no expansion, no growth, ...) in BBAI.log
void CvPlayerAI::AI_logStatusSummary()
{
	if (gPlayerLogLevel < 1 || isHuman() || isNative() || isEurope() || !isAlive() || getParent() == NO_PLAYER)
	{
		return;
	}
	logWTP(WTP_LOG_STATUS, getID(), "WTP status: Player %d (%S) turn %d gold %d cities %d pop %d desiredCities %d settlers %d workers %d/%d needed, europeUnits %d, crosses %d/%d, warPlans %d",
		getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), getGold(), getNumCities(), getTotalPopulation(),
		AI_desiredCityCount(), AI_totalUnitAIs(UNITAI_SETTLER), AI_totalUnitAIs(UNITAI_WORKER), AI_neededWorkers(NULL),
		getNumEuropeUnits(), getCrossesStored(), immigrationThreshold(), GET_TEAM(getTeam()).getAnyWarPlanCount());

	CvString szUnitAIs;
	for (int iUnitAI = 0; iUnitAI < NUM_UNITAI_TYPES; ++iUnitAI)
	{
		const UnitAITypes eUnitAI = (UnitAITypes)iUnitAI;
		const int iCount = AI_totalUnitAIs(eUnitAI);
		if (iCount > 0)
		{
			szUnitAIs += CvString::format(" %d:%d", (int)eUnitAI, iCount);
		}
	}
	logWTP(WTP_LOG_STATUS, getID(), "WTP status:   unitAIs (type:count)%s", szUnitAIs.c_str());

	int iLoop;
	for (CvArea* pArea = GC.getMap().firstArea(&iLoop); pArea != NULL; pArea = GC.getMap().nextArea(&iLoop))
	{
		if (!pArea->isWater() && pArea->getCitiesPerPlayer(getID()) > 0)
		{
			logWTP(WTP_LOG_SETTLER, getID(), "WTP status:   area %d tiles %d ownCities %d allCities %d bestFoundValue %d",
				pArea->getID(), pArea->getNumTiles(), pArea->getCitiesPerPlayer(getID()), pArea->getNumCities(), pArea->getBestFoundValue(getID()));
		}
	}

	// settlers: where they are and what they are heading for
	for (CvUnit* pSettler = firstUnit(&iLoop); pSettler != NULL; pSettler = nextUnit(&iLoop))
	{
		if (pSettler->AI_getUnitAIType() != UNITAI_SETTLER || !pSettler->isOnMap())
		{
			continue;
		}
		CvSelectionGroup* const pGroup = pSettler->getGroup();
		const CvPlot* const pTarget = (pGroup != NULL) ? pGroup->AI_getMissionAIPlot() : NULL;
		logWTP(WTP_LOG_SETTLER, getID(), "WTP status:   settler %S at %d,%d%s missionAI %d target %d,%d since turn %d",
			pSettler->getNameAndProfession().GetCString(), pSettler->getX_INLINE(), pSettler->getY_INLINE(), pSettler->isCargo() ? " (on ship)" : "",
			(pGroup != NULL) ? (int)pGroup->AI_getMissionAIType() : -1,
			(pTarget != NULL) ? pTarget->getX_INLINE() : -1, (pTarget != NULL) ? pTarget->getY_INLINE() : -1, static_cast<const CvUnitAI*>(pSettler)->AI_getLastAIChangeTurn());
	}

	for (CvUnit* pShip = firstUnit(&iLoop); pShip != NULL; pShip = nextUnit(&iLoop))
	{
		if (pShip->getDomainType() != DOMAIN_SEA)
		{
			continue;
		}
		CvSelectionGroup* const pGroup = pShip->getGroup();
		const CvPlot* const pMissionPlot = (pGroup != NULL) ? pGroup->AI_getMissionAIPlot() : NULL;
		logWTP(WTP_LOG_SHIP, getID(), "WTP status:   ship %S unitAI %d travelState %d timer %d at %d,%d cargo %d/%d (goods %d settlers %d colonists %d) missionAI %d target %d,%d",
			pShip->getName().GetCString(), (int)pShip->AI_getUnitAIType(), (int)pShip->getUnitTravelState(), pShip->getUnitTravelTimer(),
			pShip->getX_INLINE(), pShip->getY_INLINE(), pShip->getCargo(), pShip->cargoSpace(),
			pShip->getUnitAICargo(UNITAI_YIELD), pShip->getUnitAICargo(UNITAI_SETTLER), pShip->getUnitAICargo(UNITAI_COLONIST),
			(pGroup != NULL) ? (int)pGroup->AI_getMissionAIType() : -1,
			(pMissionPlot != NULL) ? pMissionPlot->getX_INLINE() : -1, (pMissionPlot != NULL) ? pMissionPlot->getY_INLINE() : -1);
	}

	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		// professions of the citizens, e.g. "Farmer 2, Carpenter 1"
		CvWString szJobs;
		std::vector<std::pair<ProfessionTypes, int> > aJobs;
		for (int i = 0; i < pCity->getPopulation(); ++i)
		{
			const CvUnit* const pCitizen = pCity->getPopulationUnitByIndex(i);
			const ProfessionTypes eJob = (pCitizen != NULL) ? pCitizen->getProfession() : NO_PROFESSION;
			bool bFound = false;
			for (uint j = 0; j < aJobs.size() && !bFound; ++j)
			{
				if (aJobs[j].first == eJob)
				{
					++aJobs[j].second;
					bFound = true;
				}
			}
			if (!bFound)
			{
				aJobs.push_back(std::make_pair(eJob, 1));
			}
		}
		for (uint j = 0; j < aJobs.size(); ++j)
		{
			szJobs += CvWString::format(L"%s%s %d", j > 0 ? L", " : L"", aJobs[j].first != NO_PROFESSION ? GC.getProfessionInfo(aJobs[j].first).getDescription() : L"none", aJobs[j].second);
		}
		logWTPCity(*pCity, "WTP status:     jobs in %S: %S", pCity->getName().GetCString(), szJobs.GetCString());
		logWTPCity(*pCity, "WTP status:   city %S pop %d food %+d (%d/%d) hammers %d lumber %d builds %S",
			pCity->getName().GetCString(), pCity->getPopulation(), pCity->foodDifference(), pCity->getFood(), pCity->growthThreshold(),
			pCity->getCurrentProductionDifference(false), pCity->getYieldStored(YIELD_LUMBER), pCity->getProductionName());
	}
}


void CvPlayerAI::AI_doTurnPost()
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_doTurnPost (native)" : "CvPlayerAI::AI_doTurnPost");
#endif
	PROFILE_FUNC();

	if (isHuman())
	{
		return;
	}

	AI_doTradeRoutes();

	AI_doDiplo();

}

// WTP, Schmiddie, AI Fleet Modernization START
void CvPlayerAI::AI_doFleetModernization()
{
	if (isHuman() || AI_isEarlyPhaseActive())
	{
		return;
	}

	if (!isPlayable())
	{
		return;
	}

	if (isNative())
	{
		return;
	}

	if (isEurope())
	{
		return;
	}

	if (GC.getGameINLINE().getGameTurnYear() < 1651)
	{
		return;
	}

	CvUnit* pBestOldUnit = NULL;
	UnitTypes eBestNewUnit = NO_UNIT;
	int iBestCost = -1;
	int iBestCount = 0;
	UnitAITypes eBestUnitAI = NO_UNITAI;

	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->isDead())
		{
			continue;
		}

		if (pLoopUnit->getUnitTravelState() != UNIT_TRAVEL_STATE_IN_EUROPE)
		{
			continue;
		}

		UnitTypes eOldUnit = pLoopUnit->getUnitType();
		const CvUnitInfo& kOldUnitInfo = GC.getUnitInfo(eOldUnit);

		if (kOldUnitInfo.getDomainType() != DOMAIN_SEA)
		{
			continue;
		}

		UnitClassTypes eOldUnitClass = (UnitClassTypes)kOldUnitInfo.getUnitClassType();
		UnitClassTypes eNewUnitClass = NO_UNITCLASS;

		if (eOldUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_WAR_GALLEON"))
		{
			eNewUnitClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_SHIP_OF_THE_LINE");
		}
		else if (eOldUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_GALLEON"))
		{
			eNewUnitClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_WEST_INDIAMAN");
		}
		else if (eOldUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_WAR_CARAVEL"))
		{
			eNewUnitClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_NAVAL_BRIG");
		}
		else if (eOldUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_NAO"))
		{
			eNewUnitClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FLUYT");
		}
		else if (eOldUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_CARAVELA_REDONDA"))
		{
			eNewUnitClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_SLOOP");
		}
		else if (eOldUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_CARRACK"))
		{
			eNewUnitClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_BRIGANTINE");
		}
		else if (eOldUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_CARAVEL"))
		{
			eNewUnitClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_SCHOONER");
		}

		if (eNewUnitClass == NO_UNITCLASS)
		{
			continue;
		}

		UnitTypes eNewUnit = (UnitTypes)GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eNewUnitClass);

		if (eNewUnit == NO_UNIT)
		{
			continue;
		}

		if (!isUnitWithinGameYearWindow(eNewUnit))
		{
			continue;
		}

		if (GC.getUnitInfo(eNewUnit).getDomainType() != DOMAIN_SEA)
		{
			continue;
		}

		int iNewPrice = getEuropeUnitBuyPrice(eNewUnit);

		if (iNewPrice <= 0)
		{
			continue;
		}

		int iOldPrice = std::abs(kOldUnitInfo.getEuropeCost());

		iOldPrice *= GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getTrainPercent();
		iOldPrice /= 100;

		iOldPrice *= GC.getEraInfo(GC.getGameINLINE().getStartEra()).getTrainPercent();
		iOldPrice /= 100;

		for (int iTrait = 0; iTrait < GC.getNumTraitInfos(); ++iTrait)
		{
			if (hasTrait((TraitTypes)iTrait))
			{
				iOldPrice *= std::max(0, (100 - GC.getTraitInfo((TraitTypes)iTrait).getRecruitPriceDiscount()));
				iOldPrice /= 100;
			}
		}

		iOldPrice *= GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAITrainPercent();
		iOldPrice /= 100;

		iOldPrice *= std::max(0, ((GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAIPerEraModifier() * getCurrentEra()) + 100));
		iOldPrice /= 100;

		int iCost = iNewPrice - iOldPrice;

		if (iCost < 0)
		{
			iCost = 0;
		}

		if (getGold() < iCost)
		{
			continue;
		}

		++iBestCount;

		if (GC.getGameINLINE().getSorenRandNum(iBestCount, "AI Fleet Modernization Unit Selection") == 0)
		{
			pBestOldUnit = pLoopUnit;
			eBestNewUnit = eNewUnit;
			iBestCost = iCost;
			eBestUnitAI = pLoopUnit->AI_getUnitAIType();
		}
	}

	if (pBestOldUnit == NULL)
	{
		return;
	}

	if (eBestNewUnit == NO_UNIT)
	{
		return;
	}

	if (iBestCost < 0)
	{
		return;
	}

	if (getGold() < iBestCost)
	{
		return;
	}

	const int iOldX = pBestOldUnit->getX_INLINE();
	const int iOldY = pBestOldUnit->getY_INLINE();

	pBestOldUnit->kill(false);

	changeGold(-iBestCost);

	CvUnit* pNewUnit = initUnit(eBestNewUnit, NO_PROFESSION, iOldX, iOldY, eBestUnitAI, NO_DIRECTION);

	if (pNewUnit != NULL)
	{
		pNewUnit->AI_setUnitAIType(eBestUnitAI);
		pNewUnit->setUnitTravelState(UNIT_TRAVEL_STATE_IN_EUROPE, false);
		pNewUnit->setUnitTravelTimer(0);
	}
}

void CvPlayerAI::AI_doTurnUnitsPre()
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_doTurnUnitsPre (native)" : "CvPlayerAI::AI_doTurnUnitsPre");
#endif
	PROFILE_FUNC();
	AI_updateBestYieldPlots();
	// WTP: rating every plot of the map is expensive; every 3 turns is enough (spread over the players).
	// Players without a colony keep rating every turn
	// Kings (they never found colonies) and the wild animals skip it, players without a colony rate every turn
	if (!isEurope() && !GET_TEAM(getTeam()).isBarbarian() && (getNumCities() == 0 || (GC.getGameINLINE().getGameTurn() + getID()) % 3 == 0))
	{
		AI_updateFoundValues();
	}
	AI_doEmotions();

	if (!isHuman())
	{
		if (getParent() != NO_PLAYER)
		{
			// WTP, Schmiddie, AI Fleet Modernization START
			AI_doFleetModernization();
			// WTP, Schmiddie, AI Fleet Modernization END

			AI_updateNativeTradeShip(); // WTP: one ship trades with the natives

			AI_doProfessions();
			AI_doEurope();
		}
	}

	if (GC.getGameINLINE().getSorenRandNum(8, "AI Update Area Targets") == 0) // XXX personality???
	{
		AI_updateAreaTargets();
	}

	if (!isHuman())
	{
		AI_doMilitaryStrategy();
		AI_doSuppressRevolution();
	}

	if (isHuman())
	{
		return;
	}
}
// WTP, Schmiddie, AI Fleet Modernization END

void CvPlayerAI::AI_doTurnUnitsPost()
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_doTurnUnitsPost (native)" : "CvPlayerAI::AI_doTurnUnitsPost");
#endif
	PROFILE_FUNC();

	CvUnit* pLoopUnit;
	int iLoop;
	if (!isHuman() || isOption(PLAYEROPTION_AUTO_PROMOTION))
	{
		for(pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
		{
			pLoopUnit->AI_promote();
		}
	}

	if (isHuman())
	{
		// WTP: a ship with the native trade automation does its business in Europe (sells, buys Trade Goods, sails back); the
		// units are collected first as buying goods creates units
		std::vector<CvUnitAI*> apTraders;
		std::vector<CvUnitAI*> apBack;
		for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
		{
			CvUnitAI* const pTrader = static_cast<CvUnitAI*>(pLoopUnit);
			if (pLoopUnit->getUnitTravelState() == UNIT_TRAVEL_STATE_IN_EUROPE && pTrader->AI_isHumanNativeTrade())
			{
				apTraders.push_back(pTrader);
			}
			// back from Europe (the automation of the group was lost on the way): automated again
			else if (pLoopUnit->isOnMap_() && pTrader->AI_hasNativeTradeAutomation() && pLoopUnit->getGroup() != NULL
				&& pLoopUnit->getGroup()->getNumUnits() == 1 && pLoopUnit->getGroup()->getAutomateType() == NO_AUTOMATE)
			{
				apBack.push_back(pTrader);
			}
		}
		for (uint i = 0; i < apTraders.size(); ++i)
		{
			apTraders[i]->AI_humanNativeTradeEurope();
		}
		for (uint i = 0; i < apBack.size(); ++i)
		{
			apBack[i]->getGroup()->setAutomateType(AUTOMATE_NATIVE_TRADE);
		}
		// WTP: units on their way to a port for a transport (transport to a city or plot, job automation): their ship is
		// chosen again now that the ships have their moves back, one unit after the other (each counts the tours before)
		std::vector<CvUnitAI*> apTravellers;
		for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
		{
			CvUnitAI* const pTraveller = static_cast<CvUnitAI*>(pLoopUnit);
			if ((pTraveller->AI_getWfTask() == CvUnitAI::WF_TRAVEL || pTraveller->AI_getWfTask() == CvUnitAI::WF_ASSIGN)
				&& pTraveller->AI_travelPickup() != NULL && pLoopUnit->isOnMap_() && !pLoopUnit->isCargo())
			{
				apTravellers.push_back(pTraveller);
			}
		}
		for (uint i = 0; i < apTravellers.size(); ++i)
		{
			apTravellers[i]->AI_travelChooseShip();
		}
		return;
	}

	// WTP, Schmiddie, Availability Change Project START
	const int iCurrentYear = GC.getGameINLINE().getGameTurnYear();

	if (iCurrentYear < 1610)
	{
		return;
	}

	int iSeaShipCount = 0;

	for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getUnitInfo().getDomainType() == DOMAIN_SEA)
		{
			iSeaShipCount++;
		}
	}

	if (iSeaShipCount <= 8)
	{
		return;
	}

	int iScrapUnitId = -1;
	int iScrapValue = 0;

	for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		const CvUnitInfo& kUnitInfo = pLoopUnit->getUnitInfo();

		if (kUnitInfo.getDomainType() != DOMAIN_SEA)
		{
			continue;
		}

		if (pLoopUnit->isCargo())
		{
			continue;
		}

		if (pLoopUnit->getTransportUnit() != NULL)
		{
			continue;
		}

		if (pLoopUnit->getCargo() > 0)
		{
			continue;
		}

		if (pLoopUnit->getUnitTravelState() != NO_UNIT_TRAVEL_STATE)
		{
			continue;
		}

		const int iObsoleteYear = kUnitInfo.getGameYearObsolete();

		if (iObsoleteYear <= 0)
		{
			continue;
		}

		if (iCurrentYear <= iObsoleteYear + 10)
		{
			continue;
		}

		iScrapUnitId = pLoopUnit->getID();
		iScrapValue = std::max(50, kUnitInfo.getEuropeCost() / 10);
		break;
	}

	if (iScrapUnitId != -1)
	{
		CvUnit* pScrapUnit = getUnit(iScrapUnitId);

		if (pScrapUnit != NULL)
		{
			changeGold(iScrapValue);
			pScrapUnit->scrap();
		}
	}
	// WTP, Schmiddie, Availability Change Project END
}


void CvPlayerAI::AI_doPeace()
{
	PROFILE_FUNC();

	CvDiploParameters* pDiplo;
	CvCity* pBestReceiveCity;
	CvCity* pBestGiveCity;
	CvCity* pLoopCity;
	CLinkList<TradeData> ourList;
	CLinkList<TradeData> theirList;
	bool abContacted[MAX_TEAMS];
	TradeData item;
	int iReceiveGold;
	int iGiveGold;
	int iGold;
	int iValue;
	int iBestValue;
	int iOurValue;
	int iTheirValue;
	int iLoop;
	int iI;
	FAssert(!isHuman());

	for (iI = 0; iI < MAX_TEAMS; iI++)
	{
		abContacted[iI] = false;
	}

	for (iI = 0; iI < MAX_PLAYERS; iI++)
	{
		if (GET_PLAYER((PlayerTypes)iI).isAlive())
		{
			if (iI != getID())
			{
				if (canContact((PlayerTypes)iI) && AI_isWillingToTalk((PlayerTypes)iI))
				{
					if (!(GET_TEAM(getTeam()).isHuman()) && (GET_PLAYER((PlayerTypes)iI).isHuman() || !(GET_TEAM(GET_PLAYER((PlayerTypes)iI).getTeam()).isHuman())))
					{
						if (GET_TEAM(getTeam()).isAtWar(GET_PLAYER((PlayerTypes)iI).getTeam()))
						{
							if (!(GET_PLAYER((PlayerTypes)iI).isHuman()) || (GET_TEAM(getTeam()).getLeaderID() == getID()))
							{
								FAssertMsg(iI != getID(), "iI is not expected to be equal with getID()");
								FAssert(GET_PLAYER((PlayerTypes)iI).getTeam() != getTeam());

								if (GET_TEAM(getTeam()).AI_getAtWarCounter(GET_PLAYER((PlayerTypes)iI).getTeam()) > 10)
								{
									if (AI_getContactTimer(((PlayerTypes)iI), CONTACT_PEACE_TREATY) == 0)
									{
										bool bOffered = false;
										if (!bOffered)
										{
											if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_PEACE_TREATY), "AI Diplo Peace Treaty") == 0)
											{
												setTradeItem(&item, TRADE_PEACE_TREATY, 0, NULL);

												if (canTradeItem(((PlayerTypes)iI), item, true) && GET_PLAYER((PlayerTypes)iI).canTradeItem(getID(), item, true))
												{
													iOurValue = GET_TEAM(getTeam()).AI_endWarVal(GET_PLAYER((PlayerTypes)iI).getTeam());
													iTheirValue = GET_TEAM(GET_PLAYER((PlayerTypes)iI).getTeam()).AI_endWarVal(getTeam());
													iReceiveGold = 0;
													iGiveGold = 0;

													pBestReceiveCity = NULL;
													pBestGiveCity = NULL;

													if (iTheirValue > iOurValue)
													{
														if (iTheirValue > iOurValue)
														{
															iBestValue = 0;
														}

														iGold = std::min((iTheirValue - iOurValue), GET_PLAYER((PlayerTypes)iI).AI_maxGoldTrade(getID()));

														if (iGold > 0)
														{
															setTradeItem(&item, TRADE_GOLD, iGold, NULL);

															if (GET_PLAYER((PlayerTypes)iI).canTradeItem(getID(), item, true))
															{
																iReceiveGold = iGold;
																iOurValue += iGold;
															}
														}

														if (iTheirValue > iOurValue)
														{
															iBestValue = 0;

															for (pLoopCity = GET_PLAYER((PlayerTypes)iI).firstCity(&iLoop); pLoopCity != NULL; pLoopCity = GET_PLAYER((PlayerTypes)iI).nextCity(&iLoop))
															{
																setTradeItem(&item, TRADE_CITIES, pLoopCity->getID(), NULL);

																if (GET_PLAYER((PlayerTypes)iI).canTradeItem(getID(), item, true))
																{
																	iValue = pLoopCity->plot()->calculateCulturePercent(getID());

																	if (iValue > iBestValue)
																	{
																		iBestValue = iValue;
																		pBestReceiveCity = pLoopCity;
																	}
																}
															}

															if (pBestReceiveCity != NULL)
															{
																iOurValue += AI_cityTradeVal(pBestReceiveCity);
															}
														}
													}
													else if (iOurValue > iTheirValue)
													{
														iBestValue = 0;
														iGold = std::min((iOurValue - iTheirValue), AI_maxGoldTrade((PlayerTypes)iI));

														if (iGold > 0)
														{
															setTradeItem(&item, TRADE_GOLD, iGold, NULL);

															if (canTradeItem(((PlayerTypes)iI), item, true))
															{
																iGiveGold = iGold;
																iTheirValue += iGold;
															}
														}

														iBestValue = 0;

														for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
														{
															setTradeItem(&item, TRADE_CITIES, pLoopCity->getID(), NULL);

															if (canTradeItem(((PlayerTypes)iI), item, true))
															{
																if (GET_PLAYER((PlayerTypes)iI).AI_cityTradeVal(pLoopCity) <= (iOurValue - iTheirValue))
																{
																	iValue = pLoopCity->plot()->calculateCulturePercent((PlayerTypes)iI);

																	if (iValue > iBestValue)
																	{
																		iBestValue = iValue;
																		pBestGiveCity = pLoopCity;
																	}
																}
															}
														}

														if (pBestGiveCity != NULL)
														{
															iTheirValue += GET_PLAYER((PlayerTypes)iI).AI_cityTradeVal(pBestGiveCity);
														}
													}

													if ((GET_PLAYER((PlayerTypes)iI).isHuman()) ? (iOurValue >= iTheirValue) : ((iOurValue > ((iTheirValue * 3) / 5)) && (iTheirValue > ((iOurValue * 3) / 5))))
													{
														ourList.clear();
														theirList.clear();

														setTradeItem(&item, TRADE_PEACE_TREATY, 0, NULL);

														ourList.insertAtEnd(item);
														theirList.insertAtEnd(item);
														if (iGiveGold != 0)
														{
															setTradeItem(&item, TRADE_GOLD, iGiveGold, NULL);
															ourList.insertAtEnd(item);
														}

														if (iReceiveGold != 0)
														{
															setTradeItem(&item, TRADE_GOLD, iReceiveGold, NULL);
															theirList.insertAtEnd(item);
														}

														if (pBestGiveCity != NULL)
														{
															setTradeItem(&item, TRADE_CITIES, pBestGiveCity->getID(), NULL);
															ourList.insertAtEnd(item);
														}

														if (pBestReceiveCity != NULL)
														{
															setTradeItem(&item, TRADE_CITIES, pBestReceiveCity->getID(), NULL);
															theirList.insertAtEnd(item);
														}

														if (GET_PLAYER((PlayerTypes)iI).isHuman())
														{
															if (!(abContacted[GET_PLAYER((PlayerTypes)iI).getTeam()]))
															{
																AI_changeContactTimer(((PlayerTypes)iI), CONTACT_PEACE_TREATY, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_PEACE_TREATY));
																pDiplo = new CvDiploParameters(getID());
																FAssertMsg(pDiplo != NULL, "pDiplo must be valid");
																pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_OFFER_PEACE"));
																pDiplo->setAIContact(true);
																pDiplo->setOurOfferList(theirList);
																pDiplo->setTheirOfferList(ourList);
																gDLL->beginDiplomacy(pDiplo, (PlayerTypes)iI);
																abContacted[GET_PLAYER((PlayerTypes)iI).getTeam()] = true;
															}
														}
														else
														{
															GC.getGameINLINE().implementDeal(getID(), ((PlayerTypes)iI), &ourList, &theirList);
														}
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
}


void CvPlayerAI::AI_updateFoundValues(bool bStartingLoc)
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_updateFoundValues (native)" : "CvPlayerAI::AI_updateFoundValues");
#endif
	PROFILE_FUNC();

	int iLoop;
	for(CvArea* pLoopArea = GC.getMap().firstArea(&iLoop); pLoopArea != NULL; pLoopArea = GC.getMap().nextArea(&iLoop))
	{
		pLoopArea->setBestFoundValue(getID(), 0);
	}

	if (bStartingLoc)
	{
		for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
		{
			GC.getMap().plotByIndexINLINE(iI)->setFoundValue(getID(), -1);
		}
	}
	else
	{
		for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
		{
			CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);

			int iValue = 0;
			if (pLoopPlot->isRevealed(getTeam(), false))
			{
				long lResult=-1;
				if(GC.getUSE_GET_CITY_FOUND_VALUE_CALLBACK())
				{
					CyArgsList argsList;
					argsList.add((int)getID());
					argsList.add(pLoopPlot->getX());
					argsList.add(pLoopPlot->getY());
					gDLL->getPythonIFace()->callFunction(PYGameModule, "getCityFoundValue", argsList.makeFunctionArgs(), &lResult);
				}

				if (lResult == -1)
				{
					iValue = AI_foundValue(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
				}
				else
				{
					iValue = lResult;
				}
			}

			pLoopPlot->setFoundValue(getID(), iValue);

			CvArea* pArea = pLoopPlot->area();
			if (iValue > pArea->getBestFoundValue(getID()))
			{
				pArea->setBestFoundValue(getID(), iValue);
			}
		}
	}
}


void CvPlayerAI::AI_updateAreaTargets()
{
	CvArea* pLoopArea;
	int iLoop;

	for(pLoopArea = GC.getMap().firstArea(&iLoop); pLoopArea != NULL; pLoopArea = GC.getMap().nextArea(&iLoop))
	{
		if (!(pLoopArea->isWater()))
		{
			if (GC.getGameINLINE().getSorenRandNum(3, "AI Target City") == 0)
			{
				pLoopArea->setTargetCity(getID(), NULL);
			}
			else
			{
				pLoopArea->setTargetCity(getID(), AI_findTargetCity(pLoopArea));
			}
		}
	}
}


// Returns priority for unit movement (lower values move first...)
int CvPlayerAI::AI_movementPriority(CvSelectionGroup* pGroup)
{
	CvUnit* pHeadUnit;
	int iCurrCombat;
	int iBestCombat;

	pHeadUnit = pGroup->getHeadUnit();

	if (pHeadUnit != NULL)
	{
		if (pHeadUnit->hasCargo())
		{
			if (pHeadUnit->specialCargo() == NO_SPECIALUNIT)
			{
				return 0;
			}
			else
			{
				return 1;
			}
		}

		if (pHeadUnit->AI_getUnitAIType() == UNITAI_SETTLER)
		{
			return 2;
		}

		if (pHeadUnit->AI_getUnitAIType() == UNITAI_WORKER)
		{
			return 3;
		}

		if (pHeadUnit->AI_getUnitAIType() == UNITAI_SCOUT)
		{
			return 4;
		}

		if (pHeadUnit->bombardRate() > 0)
		{
			return 5;
		}

		if (pHeadUnit->canFight())
		{
			if (pHeadUnit->withdrawalProbability() > 20)
			{
				return 7;
			}

			if (pHeadUnit->withdrawalProbability() > 0)
			{
				return 8;
			}

			iCurrCombat = pHeadUnit->currCombatStr(NULL, NULL);
			iBestCombat = (GC.getGameINLINE().getBestLandUnitCombat() * 100);

			if (pHeadUnit->noDefensiveBonus())
			{
				iCurrCombat *= 3;
				iCurrCombat /= 2;
			}

			if (pHeadUnit->AI_isCityAIType())
			{
				iCurrCombat /= 2;
			}

			if (iCurrCombat > iBestCombat)
			{
				return 9;
			}
			else if (iCurrCombat > ((iBestCombat * 4) / 5))
			{
				return 10;
			}
			else if (iCurrCombat > ((iBestCombat * 3) / 5))
			{
				return 11;
			}
			else if (iCurrCombat > ((iBestCombat * 2) / 5))
			{
				return 12;
			}
			else if (iCurrCombat > ((iBestCombat * 1) / 5))
			{
				return 13;
			}
			else
			{
				return 14;
			}
		}

		return 15;
	}

	return 16;
}

void CvPlayerAI::AI_unitUpdate()
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_unitUpdate (native)" : "CvPlayerAI::AI_unitUpdate");
#endif
	PROFILE_FUNC();

	CLLNode<int>* pCurrUnitNode;
	CvSelectionGroup* pLoopSelectionGroup;
	CLinkList<int> tempGroupCycle;
	CLinkList<int> finalGroupCycle;


	if (GC.getGameINLINE().getGameTurn() != m_iTurnLastManagedPop)
	{
		//This should only be done once a turn, but must be done right before
		//units are moved else it's unfair on the AI.
		if (!isHuman())
		{
			int iLoop;
			CvCity* pLoopCity;
			for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
			{
				if (isNative())
				{
					// WTP, Schmiddie, Native expansion START
					if (pLoopCity->getPopulation() >= 15)
					{
						const ProfessionTypes eProfession = AI_idealProfessionForUnitAIType(UNITAI_SETTLER, pLoopCity);

						if (eProfession != NO_PROFESSION)
						{
							static_cast<CvCityAI*>(pLoopCity)->AI_doSettlerProfessionCheat();

							int iBestValue = 0;
							CvUnit* pBestUnit = NULL;

							for (int i = 0; i < pLoopCity->getPopulation(); ++i)
							{
								CvUnit* pUnit = pLoopCity->getPopulationUnitByIndex(i);

								if (pUnit != NULL && pUnit->canHaveProfession(eProfession, false))
								{
									int iValue = AI_professionSuitability(pUnit, eProfession, pLoopCity->plot());

									if (pUnit->getProfession() == pUnit->AI_getIdealProfession())
									{
										iValue /= 4;
									}

									if (pUnit->AI_getIdealProfession() != NO_PROFESSION)
									{
										if ((pUnit->getProfession() != pUnit->AI_getIdealProfession()) && (GC.getProfessionInfo(pUnit->AI_getIdealProfession()).isWorkPlot()))
										{
											iValue *= 150;
											iValue /= 100;
										}
									}
									else
									{
										iValue *= 120;
										iValue /= 100;
									}

									if (iValue > iBestValue)
									{
										iBestValue = iValue;
										pBestUnit = pUnit;
									}
								}
							}

							if (pBestUnit != NULL)
							{
								if (pLoopCity->removePopulationUnit(CREATE_ASSERT_DATA, pBestUnit, false, eProfession))
								{
									pBestUnit->AI_setUnitAIType(UNITAI_SETTLER);
								}
							}
						}
					}
					// WTP, Schmiddie, Native expansion END

					pLoopCity->AI_doNative();
				}
				else if (pLoopCity->getPopulation() > 1)
				{
					int iValue = std::max(1, 11 / (1 + pLoopCity->getPopulation()));
					if (((GC.getGameINLINE().getGameTurn() + iLoop) % iValue) == 0)
					{
						bool bRemove = true;
						CvUnit* pRemoveUnit = pLoopCity->getPopulationUnitByIndex(0);

						if (pRemoveUnit->getProfession() != NO_PROFESSION)
						{
							CvProfessionInfo& kProfession = GC.getProfessionInfo(pRemoveUnit->getProfession());
							// R&R, ray , MYCP partially based on code of Aymerick - START
							YieldTypes eYieldProducedType = (YieldTypes)kProfession.getYieldsProduced(0);
							// R&R, ray , MYCP partially based on code of Aymerick - END

							if (eYieldProducedType == YIELD_EDUCATION)
							{
								bRemove = false;
							}

							if (bRemove && (pRemoveUnit->AI_getIdealProfession() != NO_PROFESSION) && (pRemoveUnit->getProfession() == pRemoveUnit->AI_getIdealProfession()))
							{
								bRemove = false;
								// R&R, ray , MYCP partially based on code of Aymerick - START
								YieldTypes eYieldProducedType = (YieldTypes)kProfession.getYieldsProduced(0);
								// R&R, ray , MYCP partially based on code of Aymerick - END
								if (kProfession.isWorkPlot())
								{
									CvPlot* pWorkedPlot = pLoopCity->getPlotWorkedByUnit(pRemoveUnit);
									if (pWorkedPlot == NULL)
									{
										bRemove = true;
									}
									else
									{
										if ((pWorkedPlot->getBonusType() == NO_BONUS) || (GC.getBonusInfo(pWorkedPlot->getBonusType()).getYieldChange(eYieldProducedType) <= 0))
										{
											bRemove = true;
										}
										else
										{
											CvPlot* pBestWorkedPlot = AI_getBestWorkedYieldPlot(eYieldProducedType);
											if ((pBestWorkedPlot == NULL) || (pWorkedPlot->calculateBestNatureYield(eYieldProducedType, getTeam()) < pBestWorkedPlot->calculateBestNatureYield(eYieldProducedType, getTeam())))
											{
												bRemove = true;
											}
										}
									}
								}
								else
								{
									if (pLoopCity->AI_getYieldAdvantage(eYieldProducedType) < 100)
									{
										bRemove = true;
									}
								}
							}
						}
						if (bRemove)
						{
							// make sure the unit can have the default profession
							// this fails even if it can if leaving is blocked by disorder
							//    Nightinggale
							ProfessionTypes eDefaultProfession = GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession();
							if (pRemoveUnit->canHaveProfession(eDefaultProfession, false))
							{
								pLoopCity->removePopulationUnit(CREATE_ASSERT_DATA, pRemoveUnit, false, eDefaultProfession);
							}
						}
					}
				}
			}

			for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
			{
				if (pLoopUnit->AI_getMovePriority() == 0)
				{
					pLoopUnit->AI_doInitialMovePriority();
				}
				else
				{
					FAssert(std::find(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), pLoopUnit->getID()) != m_unitPriorityHeap.end());
				}
			}
			m_iTurnLastManagedPop = GC.getGameINLINE().getGameTurn();
			m_iMoveQueuePasses = 0;
		}
	}

	if (!hasBusyUnit())
	{
		pCurrUnitNode = headGroupCycleNode();

		while (pCurrUnitNode != NULL)
		{
			pLoopSelectionGroup = getSelectionGroup(pCurrUnitNode->m_data);
			pCurrUnitNode = nextGroupCycleNode(pCurrUnitNode);

			if (pLoopSelectionGroup->AI_isForceSeparate())
			{
				// do not split groups that are in the midst of attacking
				if (pLoopSelectionGroup->isForceUpdate() || !pLoopSelectionGroup->AI_isGroupAttack())
				{
					pLoopSelectionGroup->AI_separate();	// pointers could become invalid...
				}
			}
		}

		if (isHuman())
		{
			pCurrUnitNode = headGroupCycleNode();

			while (pCurrUnitNode != NULL)
			{
				pLoopSelectionGroup = getSelectionGroup(pCurrUnitNode->m_data);
				pCurrUnitNode = nextGroupCycleNode(pCurrUnitNode);

				if (pLoopSelectionGroup == NULL || pLoopSelectionGroup->AI_update())
				{
					break; // pointers could become invalid...
				}
			}
		}
        else
		{
			int iLoop;

			//Continue existing missions.
			for(pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
			{
				pLoopSelectionGroup->autoMission();
			}

			while (!m_unitPriorityHeap.empty())
			{
				AI_verifyMoveQueue();
				CvUnit* pUnit = AI_getNextMoveUnit();

				// TAC - AI BTS Groups - koma13 - START
				/*
				if ((pUnit != NULL) && shouldUnitMove(pUnit))
				{
					int iOriginalPriority = pUnit->AI_getMovePriority();
					if (iOriginalPriority > 0)
					{
						if (!pUnit->getGroup()->isBusy() && !pUnit->getGroup()->isCargoBusy())
						{
							pUnit->AI_update();
						}
						else
						{
							m_iMoveQueuePasses++;
							if (m_iMoveQueuePasses > 100)
							{
								FAssertMsg(false, "Forcing AI to abort turn");
								return;
							}
							AI_addUnitToMoveQueue(pUnit);
							return;
						}
					}
				}
				*/

				if (pUnit->getGroup()->getNumUnits() <= 1)
				{
					if ((pUnit != NULL) && shouldUnitMove(pUnit))
					{
						int iOriginalPriority = pUnit->AI_getMovePriority();
						if (iOriginalPriority > 0)
						{
							if (!pUnit->getGroup()->isBusy() && !pUnit->getGroup()->isCargoBusy())
							{
								FAssert(!isTempUnit(pUnit));
								pUnit->AI_update();
							}
							else
							{
								m_iMoveQueuePasses++;
								if (m_iMoveQueuePasses > 100)
								{
									FAssertMsg(false, "Forcing AI to abort turn");
									return;
								}
								AI_addUnitToMoveQueue(pUnit);
								return;
							}
						}
					}
				}
				else
				{
					AI_removeUnitFromMoveQueue(pUnit);
				}
			}

			AI_groupUpdate();
			// TAC - AI BTS Groups - koma13 - END

			for (CvSelectionGroup*pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
			{
				if (pLoopSelectionGroup->readyToMove())
				{
					pLoopSelectionGroup->pushMission(MISSION_SKIP);
				}
			}
		}
	}
}

// TAC - AI BTS Groups - koma13 - START
void CvPlayerAI::AI_groupUpdate()
{
	CLLNode<int>* pCurrUnitNode;
	CvSelectionGroup* pLoopSelectionGroup;
	CLinkList<int> tempGroupCycle;
	CLinkList<int> finalGroupCycle;
	int iValue;

	tempGroupCycle.clear();
	finalGroupCycle.clear();

	pCurrUnitNode = headGroupCycleNode();

	while (pCurrUnitNode != NULL)
	{
		pLoopSelectionGroup = getSelectionGroup(pCurrUnitNode->m_data);
		if (pLoopSelectionGroup->getNumUnits() > 1)
		{
			tempGroupCycle.insertAtEnd(pCurrUnitNode->m_data);
		}
		pCurrUnitNode = nextGroupCycleNode(pCurrUnitNode);
	}

	iValue = 0;

	while (tempGroupCycle.getLength() > 0)
	{
		pCurrUnitNode = tempGroupCycle.head();

		while (pCurrUnitNode != NULL)
		{
			pLoopSelectionGroup = getSelectionGroup(pCurrUnitNode->m_data);
			FAssertMsg(pLoopSelectionGroup != NULL, "selection group node with NULL selection group");

			if (AI_movementPriority(pLoopSelectionGroup) <= iValue)
			{
				finalGroupCycle.insertAtEnd(pCurrUnitNode->m_data);
				pCurrUnitNode = tempGroupCycle.deleteNode(pCurrUnitNode);
			}
			else
			{
				pCurrUnitNode = tempGroupCycle.next(pCurrUnitNode);
			}
		}

		iValue++;
	}

	pCurrUnitNode = finalGroupCycle.head();

	while (pCurrUnitNode != NULL)
	{
		pLoopSelectionGroup = getSelectionGroup(pCurrUnitNode->m_data);

		if (NULL != pLoopSelectionGroup)  // group might have been killed by a previous group update
		{
			if (pLoopSelectionGroup->AI_update())
			{
				break; // pointers could become invalid...
			}
		}

		pCurrUnitNode = finalGroupCycle.next(pCurrUnitNode);
	}

}
// TAC - AI BTS Groups - koma13 - END

void CvPlayerAI::AI_makeAssignWorkDirty()
{
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		pLoopCity->AI_setAssignWorkDirty(true);
	}
}

struct ApplyAssignWorkingPlots
{
	ApplyAssignWorkingPlots(std::vector<CvCity*>& v_) : v(v_) {}

	void operator()(const tbb::blocked_range<size_t>& r) const
	{
		for (size_t i = r.begin(); i != r.end(); ++i)
		{
			v[i]->AI_assignWorkingPlots();
		}
	}

	std::vector<CvCity*>& v;
};

void CvPlayerAI::AI_assignWorkingPlots()
{
	AI_manageEconomy();

	// Collect cities
	std::vector<CvCity*> cities;
	cities.reserve(getNumCities());

	int iLoop;
	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		cities.push_back(pCity);
	}

	if (cities.empty())
		return;

	// Enable pop ejection deferral only when processing all cities concurrently
	FAssert(g_pDeferredPopEjectQ == NULL);

	DeferredPopEjectQueue ejectQ;
	g_pDeferredPopEjectQ = &ejectQ;

	ApplyAssignWorkingPlots body(cities);

	const int iGrainSize = 1;
	Threads::parallel_for(
		tbb::blocked_range<size_t>(0, cities.size(), iGrainSize),
		body,
		tbb::auto_partitioner()
	);

	g_pDeferredPopEjectQ = NULL;

	// Deterministic, serial apply
	applyDeferredPopEjects(ejectQ);
}


void CvPlayerAI::AI_updateAssignWork()
{
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		pLoopCity->AI_updateAssignWork();
	}
}


void CvPlayerAI::AI_makeProductionDirty()
{
	FAssertMsg(!isHuman(), "isHuman did not return false as expected");
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		pLoopCity->AI_setChooseProductionDirty(true);
	}
}


void CvPlayerAI::AI_conquerCity(CvCity* pCity)
{
	bool bRaze = false;
	if (canRaze(pCity))
	{
		bRaze = isNative();

		if (!bRaze)
		{
			CvCity* pNearestCity;
			int iRazeValue;

			iRazeValue = 0;
			if (GC.getGameINLINE().getElapsedGameTurns() > 20)
			{
				if (getNumCities() > 4)
				{
					pNearestCity = GC.getMap().findCity(pCity->getX_INLINE(), pCity->getY_INLINE(), NO_PLAYER, getTeam(), true, false, NO_TEAM, NO_DIRECTION, pCity);

					if (pNearestCity == NULL)
					{
						if (pCity->getPreviousOwner() != NO_PLAYER)
						{
							if (GET_TEAM(GET_PLAYER(pCity->getPreviousOwner()).getTeam()).countNumCitiesByArea(pCity->area()) > 3)
							{
								iRazeValue += 30;
							}
						}
					}
					else
					{
						int iDistance = plotDistance(pCity->getX_INLINE(), pCity->getY_INLINE(), pNearestCity->getX_INLINE(), pNearestCity->getY_INLINE());
						if ( iDistance > 12)
						{
							iRazeValue += iDistance * 2;
						}
					}

					int iCloseness = pCity->AI_playerCloseness(getID());
					if (iCloseness > 0)
					{
						iRazeValue -= 25;
						iRazeValue -= iCloseness * 2;
					}
					else
					{
						iRazeValue += 60;
					}

					if (pCity->area()->getCitiesPerPlayer(getID()) > 0)
					{
						iRazeValue += GC.getLeaderHeadInfo(getPersonalityType()).getRazeCityProb();
					}

					// TAC - AI Attack City - koma13 - START
					if (GET_PLAYER(pCity->getPreviousOwner()).isHuman())
					{
						int iChance = GC.getDefineINT("AI_CHANCE_FOR_RAZING_CITY");

						iRazeValue = std::max(0, iRazeValue);
						iRazeValue = std::min(iChance * 2, iRazeValue + iChance);
					}
					// TAC - AI Attack City - koma13 - END

					if (iRazeValue > 0)
					{
						if (GC.getGameINLINE().getSorenRandNum(100, "AI Raze City") < iRazeValue)
						{
							bRaze = true;
						}
					}
				}
			}
		}
	}

	if (bRaze)
	{
		pCity->doTask(TASK_RAZE);
	}
	else
	{
		gDLL->getEventReporterIFace()->cityAcquiredAndKept(GC.getGameINLINE().getActivePlayer(), pCity);
	}
}


bool CvPlayerAI::AI_acceptUnit(const CvUnit* pUnit)
{
	return true;
}


bool CvPlayerAI::AI_captureUnit(UnitTypes eUnit, CvPlot* pPlot)
{
	CvCity* pNearestCity;

	FAssert(!isHuman());

	if (pPlot->getTeam() == getTeam())
	{
		return true;
	}

	pNearestCity = GC.getMap().findCity(pPlot->getX_INLINE(), pPlot->getY_INLINE(), NO_PLAYER, getTeam());

	if (pNearestCity != NULL)
	{
		if (plotDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pNearestCity->getX_INLINE(), pNearestCity->getY_INLINE()) <= 4)
		{
			return true;
		}
	}

	return false;
}


DomainTypes CvPlayerAI::AI_unitAIDomainType(UnitAITypes eUnitAI) const
{
	switch (eUnitAI)
	{
	case UNITAI_UNKNOWN:
		return NO_DOMAIN;
		break;

	case UNITAI_COLONIST:
	case UNITAI_ANIMAL: // R&R, ray, Wild Animals
	case UNITAI_FLEEING: // R&R, ray, Fleeing Units
	case UNITAI_SETTLER:
	case UNITAI_WORKER:
	case UNITAI_MISSIONARY:
	case UNITAI_TRADER: // WTP, ray, Native Trade Posts - START
	case UNITAI_SCOUT:
	case UNITAI_WAGON:
	case UNITAI_TREASURE:
	case UNITAI_YIELD:
	case UNITAI_GENERAL:
	case UNITAI_DEFENSIVE:
	case UNITAI_OFFENSIVE:
	case UNITAI_COUNTER:
		return DOMAIN_LAND;
		break;

	case UNITAI_ANIMAL_SEA: // R&R, ray, Wild Animals
	case UNITAI_WORKER_SEA: //TAC Whaling, ray
	case UNITAI_TRANSPORT_SEA:
	case UNITAI_TRANSPORT_COAST:
	case UNITAI_ASSAULT_SEA:
	case UNITAI_COMBAT_SEA:
	case UNITAI_PIRATE_SEA:
	case UNITAI_ESCORT_SEA:		// TAC - AI Escort Sea - koma13
		return DOMAIN_SEA;
		break;

	default:
		FAssert(false);
		break;
	}

	return NO_DOMAIN;
}

bool CvPlayerAI::AI_unitAIIsCombat(UnitAITypes eUnitAI) const
{
	switch (eUnitAI)
	{
	case UNITAI_UNKNOWN:
		return false;
		break;

	case UNITAI_FLEEING: // R&R, ray, Fleeing Units
	case UNITAI_COLONIST:
	case UNITAI_SETTLER:
	case UNITAI_WORKER:
	case UNITAI_MISSIONARY:
	case UNITAI_TRADER: // WTP, ray, Native Trade Posts - START
	case UNITAI_SCOUT:
	case UNITAI_WAGON:
	case UNITAI_TREASURE:
	case UNITAI_YIELD:
	case UNITAI_GENERAL:
	case UNITAI_TRANSPORT_COAST:
		return false;
		break;

	case UNITAI_ANIMAL: // R&R, ray, Wild Animals
	case UNITAI_DEFENSIVE:
	case UNITAI_OFFENSIVE:
	case UNITAI_COUNTER:
		return true;
		break;

	//TAC Whaling, ray
	case UNITAI_WORKER_SEA:
		return false;
		break;

	case UNITAI_ANIMAL_SEA: // R&R, ray, Wild Animals
	case UNITAI_TRANSPORT_SEA:
	case UNITAI_ASSAULT_SEA:
	case UNITAI_COMBAT_SEA:
	case UNITAI_PIRATE_SEA:
	case UNITAI_ESCORT_SEA: // TAC - AI Escort Sea - koma13
		return true;
		break;

	default:
		FAssert(false);
		break;
	}
	return false;
}


int CvPlayerAI::AI_yieldWeight(YieldTypes eYield) const
{
	return GC.getYieldInfo(eYield).getAIWeightPercent();
}

#if 0
int CvPlayerAI::AI_estimatedColonistIncome(CvPlot* pPlot, CvUnit* pColonist)
{
	FAssert(pPlot != NULL);

	int iX = pPlot->getX_INLINE();
	int iY = pPlot->getY_INLINE();

	bool bFound = pColonist->canFound(pPlot);
	bool bJoin = pColonist->canJoinCity(pPlot);

	FAssert(!(bFound && bJoin));

	if (!(bFound || bJoin))
	{
		return -1;
	}

	if (!pPlot->isRevealed(getTeam(), false))
	{
		return -1;
	}

	//Calculate the income from the city tile plus the most profitable plot.
	int iTotal = 0;

	CvPlayer& kPlayerEurope = GET_PLAYER(getParent());

	if (bFound)
	{
		//cities get food and one other yield
		YieldTypes bestYield = NO_YIELD;
		int bestOutput = 0;
		for (int i = 0; i < NUM_YIELD_TYPES; i++)
		{
			//ignore food and yields not sold
			if ((i != YIELD_FOOD) && (i != YIELD_LUMBER) && (i != YIELD_STONE))
			{
				int natureYield = pPlot->calculateNatureYield((YieldTypes) i, getTeam(), false);
				if (natureYield > bestOutput)
				{
					bestYield = (YieldTypes) i;
					bestOutput = natureYield;
				}
			}
		}

		if (bestYield != NO_YIELD)
		{
			if (isYieldEuropeTradable(bestYield))
			{
				iTotal += kPlayerEurope.getYieldSellPrice(bestYield) * bestOutput;
			}
		}
	}

	int iBestValue = 0;
	for (int i = 0; i < NUM_CITY_PLOTS; i++)
	{
		CvPlot* pLoopPlot = plotCity(iX, iY, i);

		if (pLoopPlot != NULL)
		{
			if (!pLoopPlot->isBeingWorked())
			{
				YieldTypes bestYield = NO_YIELD;
				int bestOutput = 0;
				for (int j = 0; j < GC.getNumProfessionInfos(); j++)
				{
					ProfessionTypes loopProfession = (ProfessionTypes)j;
					if (GC.getCivilizationInfo(getCivilizationType()).isValidProfession(loopProfession))
					{
						CvProfessionInfo& kProfession = GC.getProfessionInfo(loopProfession);

						if (GC.getProfessionInfo(loopProfession).isWorkPlot())
						{
							CvProfessionInfo& kProfession = GC.getProfessionInfo(loopProfession);
							// R&R, ray , MYCP partially based on code of Aymerick - START
							YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
							// R&R, ray , MYCP partially based on code of Aymerick - END
							if ((eYield != NO_YIELD) && isYieldEuropeTradable(eYield))
							{
								int iValue = 0;
								int yield = pPlot->calculatePotentialProfessionYieldAmount(loopProfession, pColonist, false);
								if (eYield == YIELD_LUMBER || eYield == YIELD_STONE)
								{
									iValue += (yield * kPlayerEurope.getYieldSellPrice(eYield)) / 2;
								}
								else
								{
									iValue += yield * kPlayerEurope.getYieldBuyPrice(eYield);
								}

								iBestValue = std::max(iValue, iBestValue);
							}
						}
					}
				}
			}
		}
	}

	iTotal += iBestValue;

	if (bFound)
	{
		iTotal *= 7;
		iTotal /= 6 + getNumCities();
	}

	return iTotal;
}
#endif

int CvPlayerAI::AI_foundValue(int iX, int iY, int iMinRivalRange, bool bStartingLoc)
{
	PROFILE_FUNC();
	CvPlot* pPlot = GC.getMap().plotINLINE(iX, iY);

	if (!canFound(Coordinates(iX, iY)))
	{
		return 0;
	}

	if (!bStartingLoc)
	{
		if (!pPlot->isRevealed(getTeam(), false))
		{
			return 0;
		}
	}

	// TAC - AI City Sites - koma13 - START
	if (!isNative())
	{
		if (pPlot->calculateNatureYield(YIELD_FOOD, getTeam(), true) < GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION)
		{
				return 0;
		}
	}
	// TAC - AI City Sites - koma13 - END

	if (!isNative() && !bStartingLoc && getNumCities() == 0 && !AI_isFirstColonySiteAllowed(*pPlot))
	{
		return 0;
	}

	// WTP: later colonies need at least 5 land plots besides the city plot in the 5x5 around it,
	// unless pearls, gold or silver are there (no colonies on meaningless tiny islands)
	if (!isNative() && !bStartingLoc && getNumCities() > 0)
	{
		static const BonusTypes ePearls = (BonusTypes)GC.getInfoTypeForString("BONUS_PEARLS");
		static const BonusTypes eGold = (BonusTypes)GC.getInfoTypeForString("BONUS_GOLD");
		static const BonusTypes eSilver = (BonusTypes)GC.getInfoTypeForString("BONUS_SILVER");
		int iLandPlots = 0;
		bool bPreciousBonus = false;
		for (int iDX = -2; iDX <= 2; ++iDX)
		{
			for (int iDY = -2; iDY <= 2; ++iDY)
			{
				const CvPlot* const pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);
				if (pLoopPlot == NULL)
				{
					continue;
				}
				const BonusTypes eBonus = pLoopPlot->getBonusType();
				if (eBonus != NO_BONUS && (eBonus == ePearls || eBonus == eGold || eBonus == eSilver))
				{
					bPreciousBonus = true;
				}
				if ((iDX != 0 || iDY != 0) && !pLoopPlot->isWater() && !pLoopPlot->isImpassable())
				{
					++iLandPlots;
				}
			}
		}
		if (iLandPlots < 5 && !bPreciousBonus)
		{
			return 0;
		}
	}

	bool bNeedMoreExploring = false;
	if (getNumCities() == 0 && (pPlot->area()->getNumRevealedTiles(getTeam()) < 10))
	{
		bNeedMoreExploring = true;
	}

	if (isNative() && getNumCities() > 0)
	{
		int iRange = CITY_PLOTS_RADIUS * 2 - 1;

		int iCityDistance = AI_cityDistance(pPlot);
		if (iCityDistance == -1)
		{
			return 0;
		}
		if (iCityDistance > (iRange * 3))
		{
			return 0;
		}

		// Keep a minimum distance of 4 from all Native villages.
		for (int iDX = -3; iDX <= 3; ++iDX)
		{
			for (int iDY = -3; iDY <= 3; ++iDY)
			{
				CvPlot* pLoopPlot = plotXY(iX, iY, iDX, iDY);

				if (pLoopPlot != NULL && pLoopPlot->isCity())
				{
					if (GET_PLAYER(pLoopPlot->getOwnerINLINE()).isNative())
					{
						return 0;
					}
				}
			}
		}

		// Do not found on developed land.
		if (pPlot->getImprovementType() != NO_IMPROVEMENT)
		{
			return 0;
		}
	}

	bool bIsCoastal = pPlot->isCoastalLand(GC.getMIN_WATER_SIZE_FOR_OCEAN());
	CvArea* pArea = pPlot->area();
	int iNumAreaCities = pArea->getCitiesPerPlayer(getID());

	bool bAdvancedStart = (getAdvancedStartPoints() >= 0);

	if (!bStartingLoc && !bAdvancedStart)
	{
		if (iNumAreaCities == 0)
		{
			if (getParent() != NO_PLAYER)
			{
				if (pPlot->getNearestEurope() == NO_EUROPE)
				{
					return 0;
				}
			}
		}
	}

	if (!bStartingLoc)
	{
		if (getNumCities() == 0)
		{
			if (pArea->getNumTiles() < (NUM_CITY_PLOTS * 3))
			{
				return 0;
			}
		}
	}

	if (bAdvancedStart)
	{
		//FAssert(!bStartingLoc);
		FAssert(GC.getGameINLINE().isOption(GAMEOPTION_ADVANCED_START) || GC.getCivilizationInfo(getCivilizationType()).getAdvancedStartPoints() > 0);
		if (bStartingLoc)
		{
			bAdvancedStart = false;
		}
	}

	if (iMinRivalRange != -1)
	{
		for (int iDX = -(iMinRivalRange); iDX <= iMinRivalRange; iDX++)
		{
			for (int iDY = -(iMinRivalRange); iDY <= iMinRivalRange; iDY++)
			{
				CvPlot* pLoopPlot = plotXY(iX, iY, iDX, iDY);

				if (pLoopPlot != NULL)
				{
					if (pLoopPlot->plotCheck(PUF_isOtherTeam, getID()) != NULL)
					{
						return 0;
					}
				}
			}
		}
	}

	int iOwnedTiles = 0;

	for (int iI = 0; iI < NUM_CITY_PLOTS; iI++)
	{
		CvPlot* pLoopPlot = plotCity(iX, iY, iI);

		if (pLoopPlot == NULL)
		{
			iOwnedTiles++;
		}
		else if (pLoopPlot->isOwned() && !GET_PLAYER(pLoopPlot->getOwnerINLINE()).isNative())
        {
            if (pLoopPlot->getTeam() != getTeam())
            {
                iOwnedTiles++;
            }
        }
	}

	if (iOwnedTiles > (NUM_CITY_PLOTS / 3))
	{
		return 0;
	}

	int iBadTile = 0;
	int iNativeTile = 0;
	int iFriendlyTile = 0;
	int iColonialTile = 0;

	std::vector<int> aiFood(NUM_CITY_PLOTS, 0);

	for (int iI = 0; iI < NUM_CITY_PLOTS; iI++)
	{
		CvPlot* pLoopPlot = plotCity(iX, iY, iI);

		if (pLoopPlot != NULL)
		{
			if (iI != CITY_HOME_PLOT)
			{
				if (pLoopPlot->isImpassable())
				{
					if (pLoopPlot->getBonusType() == NO_BONUS)
					{
						// Only count impassable plots that do not have any bonus resource as bad
						iBadTile += 2;
					}
				}
				else if (!pLoopPlot->isOwned() || GET_PLAYER(pLoopPlot->getOwnerINLINE()).isNative())
				{
					if (!pLoopPlot->isHills() && !pLoopPlot->isWater())
					{
						// R&R, ray, adjustment because of MYCP
						if (((pLoopPlot->calculateBestNatureYield(YIELD_FOOD, getTeam()) == 0) && (pLoopPlot->calculateBestNatureYield(YIELD_FUR, getTeam()) < 2) && (pLoopPlot->calculateBestNatureYield(YIELD_WILD_FEATHERS, getTeam()) < 2) && (pLoopPlot->calculateBestNatureYield(YIELD_FRUITS, getTeam()) < 2)) || (pLoopPlot->calculateTotalBestNatureYield(getTeam()) <= 2))
						{
							iBadTile++;
						}
						else if (pLoopPlot->isWater() && !bIsCoastal && (pLoopPlot->calculateBestNatureYield(YIELD_FOOD, getTeam()) <= 1))
						{
							iBadTile++;
						}
					}
				}
			}
			if (pLoopPlot->isOwned())
			{
				if (GET_PLAYER(pLoopPlot->getOwnerINLINE()).isNative())
				{
					iNativeTile++;
				}
				else if (GET_PLAYER(pLoopPlot->getOwnerINLINE()).getParent() != NO_PLAYER)
				{
					iColonialTile += pLoopPlot->getCityRadiusCount();
					if (pLoopPlot->getTeam() == getTeam())
					{
						iFriendlyTile++;
					}
				}
			}
		}
		else
		{
			iBadTile++;
		}

	}

	if(!bStartingLoc)
	{
		if (bNeedMoreExploring)
		{
			if (iBadTile >= (NUM_CITY_PLOTS / 2))
			{
				return 0;
			}
		}
	}


	int aiBestWorkedYield[NUM_YIELD_TYPES];
	int aiBestUnworkedYield[NUM_YIELD_TYPES];

	for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
	{
		CvPlot* pWorkedPlot = AI_getBestWorkedYieldPlot((YieldTypes)iYield);
		if (pWorkedPlot == NULL)
		{
			aiBestWorkedYield[iYield] = 0;
		}
		else
		{
			aiBestWorkedYield[iYield] = pWorkedPlot->calculateBestNatureYield((YieldTypes)iYield, getTeam());
		}

		CvPlot* pUnworkedPlot = AI_getBestUnworkedYieldPlot((YieldTypes)iYield);
		if (pUnworkedPlot == NULL)
		{
			aiBestUnworkedYield[iYield] = 0;
		}
		else
		{
			aiBestUnworkedYield[iYield] = pUnworkedPlot->calculateBestNatureYield((YieldTypes)iYield, getTeam());
		}
	}

	int iTakenTiles = 0;
	int iTeammateTakenTiles = 0;
	int iValue = 1000;

	int iBestPlotValue = 0;
	for (int iI = 0; iI < NUM_CITY_PLOTS; iI++)
	{
		CvPlot* pLoopPlot = plotCity(iX, iY, iI);

		if (pLoopPlot == NULL)
		{
			iTakenTiles++;
		}
		else
		{
			// WTP: hostile camps nearby do not matter for founding (ray's penalty removed)

			if (pLoopPlot->isCityRadius())
			{
				iTakenTiles++;

				if (pLoopPlot->getTeam() == getTeam() && pLoopPlot->getOwnerINLINE() != getID())
				{
					iTeammateTakenTiles++;
				}
			}

			if (!pLoopPlot->isCityRadius() || (pLoopPlot->isOwned() && GET_PLAYER(pLoopPlot->getOwnerINLINE()).isNative()))
			{
				int iBestBonusAmount = 0;
				YieldTypes eBestBonusYield = NO_YIELD;

				int aiYield[NUM_YIELD_TYPES];

				for (int iYieldType = 0; iYieldType < NUM_YIELD_TYPES; ++iYieldType)
				{
					YieldTypes eYield = (YieldTypes)iYieldType;
					int iYield = pLoopPlot->calculateBestNatureYield(eYield, getTeam());

					if (iI == CITY_HOME_PLOT)
					{
						iYield += GC.getYieldInfo(eYield).getCityChange();
						//XXX make sure this reflects reality of Col
						iYield = std::max(iYield, GC.getYieldInfo(eYield).getMinCity());
					}

					aiYield[eYield] = iYield;
					if (eYield == YIELD_FOOD)
					{
						aiFood[iI] = iYield;
					}
				}

				if (iI == CITY_HOME_PLOT)
				{
					// WTP: food weighs less for later colonies, so cash crops count for more
					iValue += 2 * aiYield[YIELD_FOOD] * AI_yieldValue(YIELD_FOOD) * ((getNumCities() > 0) ? 75 : 100) / 100;

					YieldTypes bestYield = NO_YIELD;
					int bestOutput = 0;
					for (int i = 0; i < NUM_YIELD_TYPES; i++)
					{
						//ignore food because checked and lumber and stone because City Center Plot
						if ((i != YIELD_FOOD) && (i != YIELD_LUMBER) && (i != YIELD_STONE))
						{
							int natureYield = pPlot->calculateNatureYield((YieldTypes) i, getTeam(), false);
							if (natureYield > bestOutput)
							{
								bestYield = (YieldTypes) i;
								bestOutput = natureYield;
							}
						}
					}
					if (bestYield != NO_YIELD)
					{
						iValue += 2 * bestOutput * AI_yieldValue(bestYield);
					}
				}
				else
				{
					YieldTypes eBestYield = NO_YIELD;
					int iBestValue = 0;
					for (int iYieldType = 0; iYieldType < NUM_YIELD_TYPES; ++iYieldType)
					{
						YieldTypes eYield = (YieldTypes)iYieldType;

						if (aiYield[eYield] > 0)
						{
							int iYieldValue = aiYield[eYield] * AI_yieldValue(eYield);

							if (pLoopPlot->isWater())
							{
								iYieldValue /= 2;
							}

							// WTP: food weighs less for later colonies, so cash crops count for more
							if (eYield == YIELD_FOOD && getNumCities() > 0)
							{
								iYieldValue = iYieldValue * 75 / 100;
							}

							if (getNumCities() > 0)
							{
								if (aiYield[eYield] > aiBestUnworkedYield[eYield])
								{
									iYieldValue *= (4 + aiYield[eYield]);
									iYieldValue /= (2 + aiBestUnworkedYield[eYield]);

									if (aiBestWorkedYield[eYield] == 0)
									{
										iYieldValue *= 2;
										if (aiBestUnworkedYield[eYield] == 0)
										{
											if (eYield == YIELD_LUMBER)
											{
												iYieldValue *= 4;
											}
											if ((eYield == YIELD_STONE) && getNumCities() > 4)
											{
												iYieldValue *= 2;
											}
										}
									}
								}
							}
							else
							{
								if (eYield == YIELD_LUMBER)
								{
									iYieldValue *= 2;
								}
							}

							iValue += iYieldValue / ((getNumCities() == 0) ? 3 : 8);
							if (iYieldValue > iBestValue)
							{
								iBestValue = iYieldValue;
								eBestYield = eYield;
							}
						}
					}

					if (getNumCities() == 0)
					{
						if (eBestYield == YIELD_FOOD)
						{
							iBestValue *= 150;
							iBestValue /= 100;
						}
					}

					iValue += iBestValue;

					iBestPlotValue = std::max(iBestPlotValue, iBestValue);

					if (pLoopPlot->getBonusType() != NO_BONUS && eBestYield != NO_YIELD)
					{
						iValue += iBestValue;
						if (GC.getBonusInfo(pLoopPlot->getBonusType()).getYieldChange(eBestYield) > 0)
						{
							iValue += iBestValue;
						}
					}
				}
			}
		}
	}

	iValue += iBestPlotValue;

	if (iTeammateTakenTiles > 1)
	{
		return 0;
	}

	// WTP: a site with fewer than 3 usable land plots next to the city center is worth 25% less
	{
		int iUsableLand = 0;
		for (int iI = 0; iI < NUM_DIRECTION_TYPES; ++iI)
		{
			const CvPlot* const pLoopPlot = plotDirection(iX, iY, (DirectionTypes)iI);
			if (pLoopPlot != NULL && !pLoopPlot->isWater() && !pLoopPlot->isImpassable() && !pLoopPlot->isCityRadius()
				&& (!pLoopPlot->isOwned() || pLoopPlot->getOwnerINLINE() == getID() || GET_PLAYER(pLoopPlot->getOwnerINLINE()).isNative()))
			{
				++iUsableLand;
			}
		}
		if (iUsableLand < 3)
		{
			iValue = iValue * 75 / 100;
		}
	}

	if (pPlot->isCoastalLand(GC.getMIN_WATER_SIZE_FOR_OCEAN()))
	{
		//WTP, ray, Large Rivers - START
		// to ensure that first city is not found at Large River without other Water access
		if (bStartingLoc)
		{
			if (pPlot->hasDeepWaterCoast())
			{
				iValue *= 200; // found value of starting city on deep water coast dramatically increased
				iValue /= 100;
			}
			else
			{
				iValue *= 80; // found value decreased for stuff like shallow coast or lakes decreased, so it prefers above 
				iValue /= 100;
			}
		}

		// old normal logic - not considering special cases for Large Rivers without direct Ocean Plots - for all Cities after first city
		else
		{
			iValue *= 150; // increased preference for coastal cities generally
			iValue /= 100;
		}
		//WTP, ray, Large Rivers - END
	}

	if (bStartingLoc)
	{
		int iRange = GREATER_FOUND_RANGE;
		int iGreaterBadTile = 0;

		for (int iDX = -(iRange); iDX <= iRange; iDX++)
		{
			for (int iDY = -(iRange); iDY <= iRange; iDY++)
			{
				CvPlot* pLoopPlot = plotXY(iX, iY, iDX, iDY);

				if (pLoopPlot != NULL)
				{
					if (pLoopPlot->isWater() || (pLoopPlot->area() == pArea))
					{
						if (plotDistance(iX, iY, pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE()) <= iRange)
						{

							// WTP: hostile camps nearby do not matter for founding (ray's penalty removed)

						    int iTempValue = 0;
							iTempValue += (pLoopPlot->calculatePotentialYield(YIELD_FOOD, NULL, false) * 15);
							iTempValue += (pLoopPlot->calculatePotentialYield(YIELD_FUR, NULL, false) * 4); // R&R, ray, adjustment because of MYCP
							iTempValue += (pLoopPlot->calculatePotentialYield(YIELD_WILD_FEATHERS, NULL, false) * 4); // WTP, ray, adjustment because of MYCP
							iTempValue += (pLoopPlot->calculatePotentialYield(YIELD_FRUITS, NULL, false) * 4); // WTP, ray, adjustment because of MYCP
							iValue += iTempValue;
							if (iTempValue < 21)
							{
								iGreaterBadTile += 2;
								if (pLoopPlot->getFeatureType() != NO_FEATURE)
								{
									// R&R, ray, adjustment because of MYCP
									//if (pLoopPlot->calculateBestNatureYield(YIELD_FOOD, getTeam()) > 1)
							    	if (pLoopPlot->calculateBestNatureYield(YIELD_FOOD, getTeam()) > 1 || pLoopPlot->calculateBestNatureYield(YIELD_FUR, getTeam()) > 2 || pLoopPlot->calculateBestNatureYield(YIELD_WILD_FEATHERS, getTeam()) > 2 || pLoopPlot->calculateBestNatureYield(YIELD_FRUITS, getTeam()) > 2)
							    	{
										iGreaterBadTile--;
							    	}
								}
							}
						}
					}
				}
			}
		}

		if (!pPlot->isStartingPlot())
		{
			iGreaterBadTile /= 2;
			if (iGreaterBadTile > 12)
			{
				iValue *= 11;
				iValue /= iGreaterBadTile;
			}
		}

		int iWaterCount = 0;

		for (int iI = 0; iI < NUM_CITY_PLOTS; iI++)
		{
		    CvPlot* pLoopPlot = plotCity(iX, iY, iI);

            if (pLoopPlot != NULL)
		    {
		        if (pLoopPlot->isWater())
		        {
		            iWaterCount ++;
		            if (pLoopPlot->calculatePotentialYield(YIELD_FOOD, NULL, false) <= 1)
		            {
		                iWaterCount++;
					}
				}
			}
		}
		iWaterCount /= 2;

		int iLandCount = (NUM_CITY_PLOTS - iWaterCount);

		if (iLandCount < (NUM_CITY_PLOTS / 2))
		{
		    //discourage very water-heavy starts.
		    iValue *= 1 + iLandCount;
		    iValue /= (1 + (NUM_CITY_PLOTS / 2));
		}
	}

	if (bStartingLoc)
	{
		if (pPlot->getMinOriginalStartDist() == -1)
		{
			iValue += (GC.getMap().maxStepDistance() * 100);
		}
		else
		{
			iValue *= (1 + 4 * pPlot->getMinOriginalStartDist());
			iValue /= (1 + 2 * GC.getMap().maxStepDistance());
		}

		//nice hacky way to avoid this messing with normalizer, use elsewhere?
		if (!pPlot->isStartingPlot())
		{
			int iMinDistanceFactor = MAX_INT;
			int iMinRange = startingPlotRange();

			iValue *= 100;
			for (int iJ = 0; iJ < MAX_PLAYERS; iJ++)
			{
				if (GET_PLAYER((PlayerTypes)iJ).isAlive())
				{
					if (iJ != getID())
					{
						int iClosenessFactor = GET_PLAYER((PlayerTypes)iJ).startingPlotDistanceFactor(pPlot, getID(), iMinRange);
						iMinDistanceFactor = std::min(iClosenessFactor, iMinDistanceFactor);

						if (iClosenessFactor < 1000)
						{
							iValue *= 2000 + iClosenessFactor;
							iValue /= 3000;
						}
					}
				}
			}

			if (iMinDistanceFactor > 1000)
			{
				if (isNative())
				{
					iValue *= 500 + iMinDistanceFactor;
					iValue /= 1500;
				}
				else
				{
					//give a maximum boost of 25% for somewhat distant locations, don't go overboard.
					iMinDistanceFactor = std::min(1500, iMinDistanceFactor);
					iValue *= (1000 + iMinDistanceFactor);
					iValue /= 2000;
				}
			}
			else if (iMinDistanceFactor < 1000)
			{
				//this is too close so penalize again.
				iValue *= iMinDistanceFactor;
				iValue /= 1000;
				iValue *= iMinDistanceFactor;
				iValue /= 1000;
			}

			iValue /= 10;
		}
	}

	if (getNumCities() > 0)
	{
		//Friendly City Distance Modifier
		if (isNative())
		{
			int iCityDistance = AI_cityDistance(pPlot);

			iCityDistance = std::min(iCityDistance, 15);

			iValue *= 10;
			iValue /= 6 + iCityDistance;

			if (iCityDistance < 4)
			{
				iValue /= 4 - iCityDistance;
			}
		}
		else
		{
			int iCityDistance = AI_cityDistance(pPlot);

			iCityDistance = std::min(iCityDistance, 10);

			int iMinDistance = GC.getMIN_CITY_RANGE() + 1;
			int iMaxDistance = iMinDistance + 2;

			if (iCityDistance < iMinDistance)
			{
				int iFactor = AI_isStrategy(STRATEGY_DENSE_CITY_SPACING) ? 4 : 1;
				iValue *= iFactor + iCityDistance;
				iValue /= iFactor + iMinDistance;
			}

			// WTP: farther than iMaxDistance from the nearest own colony the value falls steadily, on any landmass. It
			// used to be capped at 10 plots, and the distance to the primary colony did not count on another landmass,
			// so an empty landmass 35 plots away was as good as a site next to the colonies.
			int iNearest = MAX_INT;
			int iLoop;
			for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
			{
				iNearest = std::min(iNearest, plotDistance(iX, iY, pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE()));
			}
			if (iNearest > iMaxDistance)
			{
				// factor = iMaxDistance / d, beyond iMaxDistance + 3 also * 3 / (d - iMaxDistance); the malus (1 - factor)
				// is reduced by 15 %
				int iNum = iMaxDistance;
				int iDen = iNearest;
				if (iNearest > iMaxDistance + 3)
				{
					iNum *= 3;
					iDen *= iNearest - iMaxDistance;
				}
				iValue = (int)(((__int64)iValue * (15 * iDen + 85 * iNum)) / (100 * iDen));
			}
		}
	}

	// TAC - AI City Sites - koma13 - START
	/*
	else if (!isNative())
	{// Ocean Distance Modifier
		iValue *= 8;
		iValue /= std::max(1, pPlot->getDistanceToOcean());
	}
	*/
	else if (!isNative())
	{
		iValue *= 2;
	}
	// TAC - AI City Sites - koma13 - END

	if (iValue <= 0)
	{
		return 1;
	}

	if (bNeedMoreExploring)
	{
		int iBonusCount = 0;
		int iLandCount = 0;
		for (int iI = 0; iI < NUM_CITY_PLOTS; iI++)
		{
			CvPlot* pLoopPlot = plotCity(iX, iY, iI);

			if (iI != CITY_HOME_PLOT)
			{
				if ((pLoopPlot == NULL) || pLoopPlot->isImpassable())
				{
					return 1;
				}
				else
				{
					if (pLoopPlot->getBonusType() != NO_BONUS)
					{
						iBonusCount++;
					}
				}
				if (!pLoopPlot->isWater())
				{
					iLandCount++;
				}
			}

		}

		if (iBonusCount == 0)
		{
			return 1;
		}
		else if (iBonusCount == 1)
		{
			if (pPlot->getYield(YIELD_FOOD) < GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION)
			{
				// R&R, ray, adjustment because of MYCP
				// WTP, ray, adjustment because of MYCP
				if (pPlot->getYield(YIELD_FUR) < GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION && pPlot->getYield(YIELD_WILD_FEATHERS) < GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION && pPlot->getYield(YIELD_FRUITS) < GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION && pPlot->getYield(YIELD_RICE) < GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION && pPlot->getYield(YIELD_CASSAVA) < GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION)
				{
					return 1;
				}
			}
			if (iLandCount < (NUM_CITY_PLOTS / 2))
			{
				return 1;
			}
			iValue /= 4;
		}
	}

	//Modify value according to easily attainable food.
	//Making this more important for early cities.
	{
		int iFood = aiFood[CITY_HOME_PLOT];

		std::sort(aiFood.begin(), aiFood.end(), std::greater<int>());

		iFood += aiFood[0];

		int iConsumption = 4 * GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION;

		if (iFood < iConsumption)
		{
			if (getNumCities() == 0)
			{
				return 1;
			}
			iValue *= 100 - 4 * (100 - 100 * iFood / iConsumption) / (4 + getNumCities());
			iValue /= 100;
		}
		else if (iFood > iConsumption)
		{
			iValue *= 100 + ((100 * iFood / iConsumption) - 100) / (1 + getNumCities());
			iValue /= 100;
		}

		if (isNative())
		{
			iValue *= iFood;
			iValue /= iConsumption;
		}
	}

	if (pPlot->getBonusType() != NO_BONUS)
	{
		iValue *= 2;
		iValue /= 3;
	}

	//Modify values according to other player culture
	if (!isNative())
	{
		iValue *= (NUM_CITY_PLOTS - iNativeTile);
		iValue /= NUM_CITY_PLOTS;

		if (iColonialTile > 0)
		{
			iValue *= std::max(1, (NUM_CITY_PLOTS - iColonialTile));
			iValue /= NUM_CITY_PLOTS;

			if (iFriendlyTile == 0)
			{
				iValue /= 2;
			}
		}
	}

	return std::max(1, iValue);
}

int CvPlayerAI::AI_foundValueNative(int iX, int iY, bool allowSettleOnBonus) const
{
	CvPlot* pPlot = plotXY(iX, iY, 0, 0);
	FAssert(pPlot != NULL);

	if (!allowSettleOnBonus && pPlot->getBonusType() != NO_BONUS)
	{
		return 0;
	}

	if (pPlot->isWater())
	{
		return 0;
	}

	if (pPlot->getOwnerINLINE() != getID())
	{
		return 0;
	}

	if (!canFound(Coordinates(iX, iY), false))
	{
		return 0;
	}

	int iYields = 0;
	for (int i = 0; i < NUM_YIELD_TYPES; i++)
	{
		if (i != YIELD_FOOD)
		{
			iYields += pPlot->getYield((YieldTypes)i);
		}
	}
	if (iYields == 0)
	{
		return 0;
	}

	int iWaterCount = 0;
	for (int iDirection = 0; iDirection < NUM_DIRECTION_TYPES; iDirection++)
	{
		CvPlot* pLoopPlot = plotDirection(iX, iY, (DirectionTypes)iDirection);
		if (pLoopPlot != NULL)
		{
			if (pLoopPlot->isWater() && !pLoopPlot->isLake())
			{
				iWaterCount++;
			}
		}
	}
	if (iWaterCount > 4)
	{
		return 0;
	}

	// R&R, ray, adjustment for Mapsize Gigantic
	bool giganticMap = (strcmp(getTypeStr(GC.getMap().getWorldSize()), "WORLDSIZE_GIGANTIC") == 0);

	int iBadTileCount = 0;
	for (int iI = 0; iI < NUM_CITY_PLOTS; ++iI)
	{
		CvPlot* pLoopPlot = plotCity(iX, iY, iI);
		if (pLoopPlot != NULL)
		{
			if (pLoopPlot->isImpassable())
			{
				iBadTileCount++;
			}

			// R&R, ray, adjustment for Mapsize Gigantic - START
			else
			{
				if (giganticMap)
				{
					// R&R, ray, adjustment because of MYCP
					// WTP, ray, adjustment because of MYCP
					if (pLoopPlot->getYield(YIELD_FOOD) < 2 && pLoopPlot->getYield(YIELD_FUR) < 2 && pLoopPlot->getYield(YIELD_WILD_FEATHERS) < 2 && pLoopPlot->getYield(YIELD_FRUITS) < 2 && pLoopPlot->getYield(YIELD_RICE) < 2 && pLoopPlot->getYield(YIELD_CASSAVA) < 2)
					{
						iBadTileCount++;
					}
				}
				else
				{
					// R&R, ray, adjustment because of MYCP
					// WTP, ray, adjustment because of MYCP
					if (pLoopPlot->getYield(YIELD_FOOD) < 1 && pLoopPlot->getYield(YIELD_FUR) < 2 && pLoopPlot->getYield(YIELD_WILD_FEATHERS) < 2 && pLoopPlot->getYield(YIELD_FRUITS) < 2 && pLoopPlot->getYield(YIELD_RICE) < 2 && pLoopPlot->getYield(YIELD_CASSAVA) < 2)
					{
						iBadTileCount++;
					}
				}
			}
			// R&R, ray, adjustment for Mapsize Gigantic - END
		}
	}
	if (iBadTileCount > (NUM_CITY_PLOTS * 2) / 3)
	{
		return 0;
	}

	int iValue = 0;
	int iCityCount = 0;
	int iRange = 3;

	for (int iDX = -iRange; iDX <= iRange; iDX++)
	{
		for (int iDY = -iRange; iDY <= iRange; iDY++)
		{
			int iDistance = plotDistance(iDX, iDY, 0, 0);
			if (iDistance <= iRange)
			{
				CvPlot* pLoopPlot = plotXY(iX, iY, iDX, iDY);
				if (pLoopPlot != NULL)
				{
					if (pLoopPlot->isCity())
					{
						iCityCount++;
						// R&R, ray, general improvement
						// if (iDistance == 1)
						if (iDistance <= 2)
						{
							iCityCount++;
						}
					}
					else if (!pLoopPlot->isCityRadius())
					{
						if (pLoopPlot->isWater())
						{
							iValue ++;
						}
						else if (pLoopPlot->getOwnerINLINE() == getID())
						{
							iValue++;
						}
					}
				}
			}
		}
	}

	if (iCityCount > 1)
	{
		return 0;
	}

	// R&R, ray, adjustment for Mapsize Gigantic - START
	if (giganticMap)
	{
		if (iValue < (28 / (std::max(1, 3 - getNumCities()))))
		{
			return 0;
		}
	}

	else
	{
		// TAC - AI More native settlements - START
		if (iValue < (26 / (std::max(1, 3 - getNumCities()))))
		{
			return 0;
		}
		// TAC - AI More native settlements - END
	}
	// R&R, ray, adjustment for Mapsize Gigantic - END

	iValue *= 100;
	iValue += GC.getGame().getSorenRandNum(300, "AI native city found value");
	return iValue;
}


bool CvPlayerAI::AI_isAreaAlone(CvArea* pArea)
{
	return ((pArea->getNumCities()) == GET_TEAM(getTeam()).countNumCitiesByArea(pArea));
}


bool CvPlayerAI::AI_isCapitalAreaAlone()
{
	CvCity* pCapitalCity;

	pCapitalCity = getPrimaryCity();

	if (pCapitalCity != NULL)
	{
		return AI_isAreaAlone(pCapitalCity->area());
	}

	return false;
}


bool CvPlayerAI::AI_isPrimaryArea(CvArea* pArea)
{
	CvCity* pCapitalCity;

	if (pArea->isWater())
	{
		return false;
	}

	if (pArea->getCitiesPerPlayer(getID()) > 2)
	{
		return true;
	}

	pCapitalCity = getPrimaryCity();

	if (pCapitalCity != NULL)
	{
		if (pCapitalCity->area() == pArea)
		{
			return true;
		}
	}

	return false;
}


int CvPlayerAI::AI_militaryWeight(CvArea* pArea)
{
	return (pArea->getPopulationPerPlayer(getID()) + pArea->getCitiesPerPlayer(getID()) + 1);
}


int CvPlayerAI::AI_targetCityValue(CvCity* pCity, bool bRandomize, bool bIgnoreAttackers)
{
	PROFILE_FUNC();

	CvCity* pNearestCity;
	CvPlot* pLoopPlot;
	int iValue;
	int iI;

	FAssertMsg(pCity != NULL, "City is not assigned a valid value");

	iValue = 1;

	iValue += ((pCity->getPopulation() * (50 + pCity->calculateCulturePercent(getID()))) / 100);

	if (pCity->getDefenseDamage() > 0)
	{
		iValue += ((pCity->getDefenseDamage() / 30) + 1);
	}

	if (pCity->isCoastal(GC.getMIN_WATER_SIZE_FOR_OCEAN()))
	{
		iValue++;
	}

	if (pCity->isEverOwned(getID()))
	{
		iValue += 3;
	}
	if (!bIgnoreAttackers)
	{
	iValue += AI_adjacentPotentialAttackers(pCity->plot());
	}

	for (iI = 0; iI < NUM_CITY_PLOTS; iI++)
	{
		pLoopPlot = plotCity(pCity->getX_INLINE(), pCity->getY_INLINE(), iI);

		if (pLoopPlot != NULL)
		{
			if (pLoopPlot->getBonusType() != NO_BONUS)
			{
				iValue++;
			}

			if (pLoopPlot->getOwnerINLINE() == getID())
			{
				iValue++;
			}

			if (pLoopPlot->isAdjacentPlayer(getID(), true))
			{
				iValue++;
			}
		}
	}

	pNearestCity = GC.getMap().findCity(pCity->getX_INLINE(), pCity->getY_INLINE(), getID());

	if (pNearestCity != NULL)
	{
		iValue += std::max(1, ((GC.getMap().maxStepDistance() * 2) - GC.getMap().calculatePathDistance(pNearestCity->plot(), pCity->plot())));
	}

	if (bRandomize)
	{
		iValue += GC.getGameINLINE().getSorenRandNum(((pCity->getPopulation() / 2) + 1), "AI Target City Value");
	}

	return iValue;
}


CvCity* CvPlayerAI::AI_findTargetCity(CvArea* pArea)
{
	CvCity* pLoopCity;
	CvCity* pBestCity;
	int iValue;
	int iBestValue;
	int iLoop;
	int iI;

	iBestValue = 0;
	pBestCity = NULL;

	for (iI = 0; iI < MAX_PLAYERS; iI++)
	{
		if (GET_PLAYER((PlayerTypes)iI).isAlive())
		{
			// R&R, ray, Natives raiding party - START
			if ((isPotentialEnemy(getTeam(), GET_PLAYER((PlayerTypes)iI).getTeam())) || (AI_isPotentialRaidTarget((PlayerTypes)iI)))
			{
				for (pLoopCity = GET_PLAYER((PlayerTypes)iI).firstCity(&iLoop); pLoopCity != NULL; pLoopCity = GET_PLAYER((PlayerTypes)iI).nextCity(&iLoop))
				{
					if (pLoopCity->area() == pArea)
					{
						iValue = AI_targetCityValue(pLoopCity, true);

						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							pBestCity = pLoopCity;
						}
					}
				}
			}
			// R&R, ray, Natives raiding party - END
		}
	}

	return pBestCity;
}


int CvPlayerAI::AI_getPlotDanger(CvPlot* pPlot, int iRange, bool bTestMoves, bool bOffensive) const
{
	PROFILE_FUNC();

	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	CvPlot* pLoopPlot;
	int iCount;
	int iDistance;
	int iBorderDanger;
	int iDX, iDY;
	CvArea *pPlotArea = pPlot->area();

	iCount = 0;
	iBorderDanger = 0;

	if (iRange == -1)
	{
		iRange = DANGER_RANGE;
	}

	for (iDX = -(iRange); iDX <= iRange; iDX++)
	{
		for (iDY = -(iRange); iDY <= iRange; iDY++)
		{
			pLoopPlot	= plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);

			if (pLoopPlot != NULL)
			{
				if (pLoopPlot->area() == pPlotArea)
				{
					iDistance = stepDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
					if (atWar(pLoopPlot->getTeam(), getTeam()))
					{
						if (iDistance == 1)
						{
							iBorderDanger++;
						}
						else if ((iDistance == 2) && (pLoopPlot->isRoute()))
						{
							iBorderDanger++;
						}
					}


					pUnitNode = pLoopPlot->headUnitNode();

					while (pUnitNode != NULL)
					{
						pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

						if (pLoopUnit == NULL)
						{
							continue;
						}

						// Ignore animals
						if (pLoopUnit->getUnitInfo().isAnimal())
						{
							continue;
						}

						if (pLoopUnit->isEnemy(getTeam()))
						{
							if (bOffensive || pLoopUnit->canAttack())
							{
								if (!(pLoopUnit->isInvisible(getTeam(), false)))
								{
									if (bOffensive || pLoopUnit->canMoveOrAttackInto(pPlot))
									{
										if (!bTestMoves)
										{
											iCount++;
										}
										else
										{
											int iDangerRange = pLoopUnit->baseMoves();
											iDangerRange += ((pLoopPlot->isValidRoute(pLoopUnit)) ? 1 : 0);
											if (iDangerRange >= iDistance)
											{
												iCount++;
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (iBorderDanger > 0)
	{
		if (!isHuman() && (!pPlot->isCity() || bOffensive))
		{
			iCount += iBorderDanger;
		}
	}

	return iCount;
}

int CvPlayerAI::AI_getUnitDanger(CvUnit* pUnit, int iRange, bool bTestMoves, bool bAnyDanger) const
{
	PROFILE_FUNC();

	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	CvPlot* pLoopPlot;
	int iCount;
	int iDistance;
	int iBorderDanger;
	int iDX, iDY;

	CvPlot* pPlot = pUnit->plot();
	iCount = 0;
	iBorderDanger = 0;

	// use a target area instead of the current one
	// This makes ships use the sea area from a plot next to it when they are in a land plot (city)
	// Vanilla used the land plot in that case and that made the AI hide ships in native settlements to get away from dangerous land units
	//    Nightinggale
	CvArea* pTargetArea = pUnit->area();

	if (iRange == -1)
	{
		iRange = DANGER_RANGE;
	}

	for (iDX = -(iRange); iDX <= iRange; iDX++)
	{
		for (iDY = -(iRange); iDY <= iRange; iDY++)
		{
			pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);

			if (pLoopPlot != NULL)
			{
				if (pLoopPlot->area() == pTargetArea)
				{
					iDistance = stepDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
					if (atWar(pLoopPlot->getTeam(), getTeam()))
					{
						if (iDistance == 1)
						{
							iBorderDanger++;
						}
						else if ((iDistance == 2) && (pLoopPlot->isRoute()))
						{
							iBorderDanger++;
						}
					}


					pUnitNode = pLoopPlot->headUnitNode();

					while (pUnitNode != NULL)
					{
						pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

						if (pLoopUnit != NULL && atWar(pLoopUnit->getTeam(), getTeam()))
						{
							if (pLoopUnit->canAttack())
							{
								// Ignore animals
								if (pLoopUnit->getUnitInfo().isAnimal())
									continue;

								if (!(pLoopUnit->isInvisible(getTeam(), false)))
								{
									if (pLoopUnit->canMoveOrAttackInto(pPlot))
									{
										if (!bTestMoves)
										{
											iCount++;
										}
										else
										{
											int iDangerRange = pLoopUnit->baseMoves();
											iDangerRange += ((pLoopPlot->isValidRoute(pLoopUnit)) ? 1 : 0);
											if (iDangerRange >= iDistance)
											{
												iCount++;
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (iBorderDanger > 0)
	{
	    if (!isHuman() || pUnit->isAutomated())
	    {
            iCount += iBorderDanger;
	    }
	}

	return iCount;
}

// TAC - AI Improved Navel AI - koma13 - START
//int CvPlayerAI::AI_getWaterDanger(CvPlot* pPlot, int iRange, bool bTestMoves)
// Returns a count of hostile units (animals will be ignored since they do not represent a real threat) within iRange of the plot
// if bDangerMap is true the count will include a (decaying) history of hostiles that we've seen.
int CvPlayerAI::AI_getWaterDanger(CvPlot* pPlot, int iRange, bool bTestMoves, bool bDangerMap, bool bVisibleOnly) const
// TAC - AI Improved Navel AI - koma13 - END
{
	PROFILE_FUNC();

	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	CvPlot* pLoopPlot;
	int iCount;

	iCount = 0;

	if (iRange == -1)
	{
		iRange = DANGER_RANGE;
	}

	//CvArea* pWaterArea = pPlot->waterArea();

	for (int iDX = -(iRange); iDX <= iRange; iDX++)
	{
		for (int iDY = -(iRange); iDY <= iRange; iDY++)
		{
			pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);
			if (pLoopPlot != NULL)
			{
				if (pLoopPlot->isWater())
				{
					if (pPlot->isAdjacentToArea(pLoopPlot->getArea()))
					{
						if (!bVisibleOnly || pLoopPlot->isVisible(getTeam(), false))	// TAC - AI Improved Navel AI - koma13
						{
							pUnitNode = pLoopPlot->headUnitNode();
							while (pUnitNode != NULL)
							{
								pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

								if (pLoopUnit == NULL)
								{
									continue;
								}

								if (!pLoopUnit->isEnemy(getTeam()))
									continue;

								if (!pLoopUnit->canAttack())
									continue;

								if ((pLoopUnit->isInvisible(getTeam(), false)))
									continue;

								// Ignore animals
								if (pLoopUnit->getUnitInfo().isAnimal())
									continue;

								iCount++;
							}
						}

						// TAC - AI Improved Navel AI - koma13 - START
						if (bDangerMap)
						{
							iCount += (pLoopPlot->getDangerMap(getID()) > 0) ? 1 : 0;
						}
						// TAC - AI Improved Navel AI - koma13 - END
					}
				}
			}
		}
	}

	return iCount;
}

int CvPlayerAI::AI_goldTarget()
{
	int iGold = 0;

	if (GC.getGameINLINE().getElapsedGameTurns() >= 40)
	{
		int iMultiplier = 0;
		iMultiplier += GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getFatherPercent();
		iMultiplier += GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getTrainPercent();
		iMultiplier += GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getConstructPercent();
		iMultiplier /= 3;

		iGold += ((getNumCities() * 3) + (getTotalPopulation() / 3));

		iGold += (GC.getGameINLINE().getElapsedGameTurns() / 2);

		iGold *= iMultiplier;
		iGold /= 100;

		bool bAnyWar = GET_TEAM(getTeam()).getAnyWarPlanCount() > 0;
		if (bAnyWar)
		{
			iGold *= 3;
			iGold /= 2;
		}
		iGold += (AI_goldToUpgradeAllUnits() / (bAnyWar ? 1 : 2));
	}

	return iGold + AI_getExtraGoldTarget();
}

DiploCommentTypes CvPlayerAI::AI_getGreeting(PlayerTypes ePlayer)
{
	TeamTypes eWorstEnemy;

	if (GET_PLAYER(ePlayer).getTeam() != getTeam())
	{
		eWorstEnemy = GET_TEAM(getTeam()).AI_getWorstEnemy();
		if ((eWorstEnemy != NO_TEAM) && (eWorstEnemy != GET_PLAYER(ePlayer).getTeam()) && GET_TEAM(GET_PLAYER(ePlayer).getTeam()).isHasMet(eWorstEnemy) && (GC.getASyncRand().get(4) == 0))
		{
			if (GET_PLAYER(ePlayer).AI_hasTradedWithTeam(eWorstEnemy) && !atWar(GET_PLAYER(ePlayer).getTeam(), eWorstEnemy))
			{
				return (DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_WORST_ENEMY_TRADING");
			}
			else
			{
				return (DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_WORST_ENEMY");
			}
		}
	}

	return (DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_GREETINGS");
}


bool CvPlayerAI::AI_isWillingToTalk(PlayerTypes ePlayer)
{
	FAssertMsg(getPersonalityType() != NO_LEADER, "getPersonalityType() is not expected to be equal with NO_LEADER");
	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");

	if (GET_PLAYER(ePlayer).getTeam() == getTeam())
	{
		return true;
	}

	if (GET_TEAM(getTeam()).isHuman())
	{
		return false;
	}

	if (atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
	{
		if (GET_TEAM(getTeam()).isParentOf(GET_PLAYER(ePlayer).getTeam()))
		{
			return false;
		}

		int iRefuseDuration = (GC.getLeaderHeadInfo(getPersonalityType()).getRefuseToTalkWarThreshold());

		if  (GET_TEAM(getTeam()).AI_isChosenWar(GET_PLAYER(ePlayer).getTeam()))
		{
			if (!isNative())
			{
				iRefuseDuration *= 2;
			}
		}
		else
		{
			if (isNative())
			{
				iRefuseDuration *= 2;
			}
		}

		int iOurSuccess = 1 + GET_TEAM(getTeam()).AI_getWarSuccess(GET_PLAYER(ePlayer).getTeam());
		int iTheirSuccess = 1 + GET_TEAM(GET_PLAYER(ePlayer).getTeam()).AI_getWarSuccess(getTeam());
		if (iTheirSuccess > iOurSuccess * 2)
		{
			iRefuseDuration *= 50 + ((50 * iOurSuccess * 2) / iTheirSuccess);
			iRefuseDuration /= 100;
		}

		if (isNative())
		{
			iRefuseDuration *= 2;
			int iGameTurns = GC.getGameINLINE().getEstimateEndTurn();
			int iCurrentTurn = GC.getGameINLINE().getGameTurn();

			if (!GET_TEAM(getTeam()).AI_isChosenWar((GET_PLAYER(ePlayer).getTeam())))
			{
				iCurrentTurn += iGameTurns / 2;
			}

			iRefuseDuration *= std::max(0, iCurrentTurn - iGameTurns / 12);
			iRefuseDuration /= iGameTurns;
		}

		if (GET_TEAM(getTeam()).AI_getAtWarCounter(GET_PLAYER(ePlayer).getTeam()) < iRefuseDuration)
		{
			return false;
		}
	}
	else
	{
		if (AI_getMemoryCount(ePlayer, MEMORY_STOPPED_TRADING_RECENT) > 0)
		{
			return false;
		}
	}

	return true;
}


// XXX what if already at war???
// Returns true if the AI wants to sneak attack...
bool CvPlayerAI::AI_demandRebukedSneak(PlayerTypes ePlayer)
{
	FAssertMsg(!isHuman(), "isHuman did not return false as expected");
	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");

	FAssert(!(GET_TEAM(getTeam()).isHuman()));

	if (GC.getGameINLINE().getSorenRandNum(100, "AI Demand Rebuked") < GC.getLeaderHeadInfo(getPersonalityType()).getDemandRebukedSneakProb())
	{
		if (GET_TEAM(getTeam()).getPower() > GET_TEAM(GET_PLAYER(ePlayer).getTeam()).getDefensivePower())
		{
			return true;
		}
	}

	return false;
}


// XXX what if already at war???
// Returns true if the AI wants to declare war...
bool CvPlayerAI::AI_demandRebukedWar(PlayerTypes ePlayer)
{
	FAssertMsg(!isHuman(), "isHuman did not return false as expected");
	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");

	FAssert(!(GET_TEAM(getTeam()).isHuman()));

	// needs to be async because it only happens on the computer of the player who is in diplomacy...
	if (GC.getASyncRand().get(100, "AI Demand Rebuked ASYNC") < GC.getLeaderHeadInfo(getPersonalityType()).getDemandRebukedWarProb())
	{
		if (GET_TEAM(getTeam()).getPower() > GET_TEAM(GET_PLAYER(ePlayer).getTeam()).getDefensivePower())
		{
			if (GET_TEAM(getTeam()).AI_isAllyLandTarget(GET_PLAYER(ePlayer).getTeam()))
			{
				return true;
			}
		}
	}

	return false;
}


// XXX maybe make this a little looser (by time...)
bool CvPlayerAI::AI_hasTradedWithTeam(TeamTypes eTeam)
{
	int iI;

	for (iI = 0; iI < MAX_PLAYERS; iI++)
	{
		if (GET_PLAYER((PlayerTypes)iI).isAlive())
		{
			if (GET_PLAYER((PlayerTypes)iI).getTeam() == eTeam)
			{
				if ((AI_getPeacetimeGrantValue((PlayerTypes)iI) + AI_getPeacetimeTradeValue((PlayerTypes)iI)) > 0)
				{
					return true;
				}
			}
		}
	}

	return false;
}

// static
AttitudeTypes CvPlayerAI::AI_getAttitude(int iAttitudeVal)
{
	if (iAttitudeVal >= 10)
	{
		return ATTITUDE_FRIENDLY;
	}
	else if (iAttitudeVal >= 3)
	{
		return ATTITUDE_PLEASED;
	}
	else if (iAttitudeVal <= -10)
	{
		return ATTITUDE_FURIOUS;
	}
	else if (iAttitudeVal <= -3)
	{
		return ATTITUDE_ANNOYED;
	}
	else
	{
		return ATTITUDE_CAUTIOUS;
	}
}

AttitudeTypes CvPlayerAI::AI_getAttitude(PlayerTypes ePlayer, bool bForced)
{
	PROFILE_FUNC();

	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");

	return (AI_getAttitude(AI_getAttitudeVal(ePlayer, bForced)));
}


int CvPlayerAI::AI_getAttitudeVal(PlayerTypes ePlayer, bool bForced)
{
	PROFILE_FUNC();

	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");

	if (bForced)
	{
		if (getTeam() == GET_PLAYER(ePlayer).getTeam())
		{
			return 100;
		}
	}

	int iAttitude = GC.getLeaderHeadInfo(getPersonalityType()).getBaseAttitude();

	if (GET_PLAYER(ePlayer).isNative())
	{
		iAttitude += GC.getLeaderHeadInfo(getPersonalityType()).getNativeAttitude();
	}

	iAttitude += GC.getHandicapInfo(GET_PLAYER(ePlayer).getHandicapType()).getAttitudeChange();

	iAttitude -= std::max(0, (GET_TEAM(GET_PLAYER(ePlayer).getTeam()).getNumMembers() - GET_TEAM(getTeam()).getNumMembers()));

	if (GET_TEAM(GET_PLAYER(ePlayer).getTeam()).AI_getWarSuccess(getTeam()) > GET_TEAM(getTeam()).AI_getWarSuccess(GET_PLAYER(ePlayer).getTeam()))
	{
		iAttitude += GC.getLeaderHeadInfo(getPersonalityType()).getLostWarAttitudeChange();
	}

	iAttitude += AI_getCloseBordersAttitude(ePlayer);
	iAttitude += AI_getStolenPlotsAttitude(ePlayer);
	iAttitude += AI_getAlarmAttitude(ePlayer);
	iAttitude += AI_getRebelAttitude(ePlayer);
	iAttitude += AI_getWarAttitude(ePlayer);
	iAttitude += AI_getPeaceAttitude(ePlayer);
	iAttitude += AI_getOpenBordersAttitude(ePlayer);
	iAttitude += AI_getDefensivePactAttitude(ePlayer);
	iAttitude += AI_getRivalDefensivePactAttitude(ePlayer);
	iAttitude += AI_getShareWarAttitude(ePlayer);
	iAttitude += AI_getTradeAttitude(ePlayer);
	iAttitude += AI_getRivalTradeAttitude(ePlayer);

	for (int iI = 0; iI < NUM_MEMORY_TYPES; iI++)
	{
		iAttitude += AI_getMemoryAttitude(ePlayer, ((MemoryTypes)iI));
	}

	iAttitude += AI_getAttitudeExtra(ePlayer);

	return range(iAttitude, -100, 100);
}


int CvPlayerAI::AI_calculateStolenCityRadiusPlots(PlayerTypes ePlayer)
{
	PROFILE_FUNC();

	CvPlot* pLoopPlot;
	int iCount;
	int iI;

	FAssert(ePlayer != getID());

	iCount = 0;

	for (iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
	{
		pLoopPlot = GC.getMap().plotByIndexINLINE(iI);

		if (pLoopPlot->getOwnerINLINE() == ePlayer)
		{
			if (pLoopPlot->isPlayerCityRadius(getID()))
			{
				iCount++;
			}
		}
	}

	return iCount;
}


int CvPlayerAI::AI_getCloseBordersAttitude(PlayerTypes ePlayer)
{
	if (m_em_iCloseBordersAttitudeCache.get(ePlayer) == MAX_INT)
	{
		if (isNative())
		{
			return 0;
		}

		if (getTeam() == GET_PLAYER(ePlayer).getTeam())
		{
			return 0;
		}

		int iPercent = std::min(60, (AI_calculateStolenCityRadiusPlots(ePlayer) * 3));

		if (GET_TEAM(getTeam()).AI_isLandTarget(GET_PLAYER(ePlayer).getTeam()))
		{
			iPercent += 40;
		}

		m_em_iCloseBordersAttitudeCache.set(ePlayer, ((GC.getLeaderHeadInfo(getPersonalityType()).getCloseBordersAttitudeChange() * iPercent) / 100));
	}

	return m_em_iCloseBordersAttitudeCache.get(ePlayer);
}


int CvPlayerAI::AI_getStolenPlotsAttitude(PlayerTypes ePlayer)
{
	if (m_em_iStolenPlotsAttitudeCache.get(ePlayer) == MAX_INT)
	{
		if (getTeam() == GET_PLAYER(ePlayer).getTeam())
		{
			return 0;
		}

		if (!isNative())
		{
			return 0;
		}

		int iStolenPlots = 0;
		for(int i=0;i<GC.getMap().numPlotsINLINE();i++)
		{
			CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(i);
			if(pLoopPlot->getOwnerINLINE() == ePlayer && pLoopPlot->getCulture(getID()) > pLoopPlot->getCulture(ePlayer))
			{
				if (!pLoopPlot->isNotCulture(ePlayer))
				{
					++iStolenPlots;
				}
			}
		}

		// change attitude by stolen plots per city
		m_em_iStolenPlotsAttitudeCache.set(ePlayer, GC.getLeaderHeadInfo(getPersonalityType()).getCloseBordersAttitudeChange() * iStolenPlots / std::max(getNumCities(), 1));
	}

	return m_em_iStolenPlotsAttitudeCache.get(ePlayer);
}


int CvPlayerAI::AI_getAlarmAttitude(PlayerTypes ePlayer)
{
	if (getTeam() == GET_PLAYER(ePlayer).getTeam())
	{
		return 0;
	}

	AlarmTypes eAlarm = (AlarmTypes) GC.getLeaderHeadInfo(getLeaderType()).getAlarmType();
	if (eAlarm == NO_ALARM)
	{
		return 0;
	}

	int iAlarm = GET_PLAYER(ePlayer).getNumCities() * GC.getAlarmInfo(eAlarm).getNumColonies();
	iAlarm *= std::max(0, 100 + GET_PLAYER(ePlayer).getNativeAngerModifier());
	iAlarm /= 100;

	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		iAlarm += pLoopCity->AI_calculateAlarm(ePlayer);
	}

	iAlarm *= GC.getLeaderHeadInfo(getPersonalityType()).getAlarmAttitudeChange();
	iAlarm /= std::max(1, GC.getAlarmInfo(eAlarm).getAttitudeDivisor());

	return std::min(iAlarm, 0);
}

int CvPlayerAI::AI_getRebelAttitude(PlayerTypes ePlayer)
{
	if (GET_PLAYER(ePlayer).getParent() != getID())
	{
		return 0;
	}

	if (GC.getLeaderHeadInfo(getPersonalityType()).getRebelAttitudeDivisor() == 0)
	{
		return 0;
	}

	int iBells = GET_PLAYER(ePlayer).getBellsStored();

	iBells *= 100;
	iBells /= std::max(1, GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getGrowthPercent());
	iBells /= GC.getLeaderHeadInfo(getPersonalityType()).getRebelAttitudeDivisor();

	// Erik: Negative attitude from bells now scale with the rebel sentiment.
	// This effectively disables the exploit that allows players to request discounted
	// units from their king as long as the king is at least cautious
	const int iRebelPercent = GET_TEAM(GET_PLAYER(ePlayer).getTeam()).getRebelPercent();
	// TODO: The maximum should be a XML parameter
	const int iCappedBells = static_cast<int>(std::max(-20.0, iBells * ((100 - iRebelPercent) / 100.0)));

	return iCappedBells;
}

int CvPlayerAI::AI_getWarAttitude(PlayerTypes ePlayer)
{
	int iAttitude = 0;

	if (atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
	{
		iAttitude -= 3;
	}

	if (GC.getLeaderHeadInfo(getPersonalityType()).getAtWarAttitudeDivisor() != 0)
	{
		int iAttitudeChange = (GET_TEAM(getTeam()).AI_getAtWarCounter(GET_PLAYER(ePlayer).getTeam()) / GC.getLeaderHeadInfo(getPersonalityType()).getAtWarAttitudeDivisor());
		iAttitude += range(iAttitudeChange, -(abs(GC.getLeaderHeadInfo(getPersonalityType()).getAtWarAttitudeChangeLimit())), abs(GC.getLeaderHeadInfo(getPersonalityType()).getAtWarAttitudeChangeLimit()));
	}

	return iAttitude;
}


int CvPlayerAI::AI_getPeaceAttitude(PlayerTypes ePlayer)
{
	if (GC.getLeaderHeadInfo(getPersonalityType()).getAtPeaceAttitudeDivisor() != 0)
	{
		int iAttitudeChange = (GET_TEAM(getTeam()).AI_getAtPeaceCounter(GET_PLAYER(ePlayer).getTeam()) / GC.getLeaderHeadInfo(getPersonalityType()).getAtPeaceAttitudeDivisor());
		return range(iAttitudeChange, -(abs(GC.getLeaderHeadInfo(getPersonalityType()).getAtPeaceAttitudeChangeLimit())), abs(GC.getLeaderHeadInfo(getPersonalityType()).getAtPeaceAttitudeChangeLimit()));
	}

	return 0;
}


int CvPlayerAI::AI_getOpenBordersAttitude(PlayerTypes ePlayer)
{
	if (!atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
	{
		if (GC.getLeaderHeadInfo(getPersonalityType()).getOpenBordersAttitudeDivisor() != 0)
		{
			int iAttitudeChange = (GET_TEAM(getTeam()).AI_getOpenBordersCounter(GET_PLAYER(ePlayer).getTeam()) / GC.getLeaderHeadInfo(getPersonalityType()).getOpenBordersAttitudeDivisor());
			return range(iAttitudeChange, -(abs(GC.getLeaderHeadInfo(getPersonalityType()).getOpenBordersAttitudeChangeLimit())), abs(GC.getLeaderHeadInfo(getPersonalityType()).getOpenBordersAttitudeChangeLimit()));
		}
	}

	return 0;
}


int CvPlayerAI::AI_getDefensivePactAttitude(PlayerTypes ePlayer)
{
	if (!atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
	{
		if (GC.getLeaderHeadInfo(getPersonalityType()).getDefensivePactAttitudeDivisor() != 0)
		{
			int iAttitudeChange = (GET_TEAM(getTeam()).AI_getDefensivePactCounter(GET_PLAYER(ePlayer).getTeam()) / GC.getLeaderHeadInfo(getPersonalityType()).getDefensivePactAttitudeDivisor());
			return range(iAttitudeChange, -(abs(GC.getLeaderHeadInfo(getPersonalityType()).getDefensivePactAttitudeChangeLimit())), abs(GC.getLeaderHeadInfo(getPersonalityType()).getDefensivePactAttitudeChangeLimit()));
		}
	}

	return 0;
}


int CvPlayerAI::AI_getRivalDefensivePactAttitude(PlayerTypes ePlayer)
{
	int iAttitude = 0;

	if (getTeam() == GET_PLAYER(ePlayer).getTeam())
	{
		return iAttitude;
	}

	if (!(GET_TEAM(getTeam()).isDefensivePact(GET_PLAYER(ePlayer).getTeam())))
	{
		iAttitude -= ((4 * GET_TEAM(GET_PLAYER(ePlayer).getTeam()).getDefensivePactCount(GET_PLAYER(ePlayer).getTeam())) / std::max(1, (GC.getGameINLINE().countCivTeamsAlive() - 2)));
	}

	return iAttitude;
}


int CvPlayerAI::AI_getShareWarAttitude(PlayerTypes ePlayer)
{
	int iAttitudeChange;
	int iAttitude;

	iAttitude = 0;

	if (!atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
	{
		if (GET_TEAM(getTeam()).AI_shareWar(GET_PLAYER(ePlayer).getTeam()))
		{
			iAttitude += GC.getLeaderHeadInfo(getPersonalityType()).getShareWarAttitudeChange();
		}

		if (GC.getLeaderHeadInfo(getPersonalityType()).getShareWarAttitudeDivisor() != 0)
		{
			iAttitudeChange = (GET_TEAM(getTeam()).AI_getShareWarCounter(GET_PLAYER(ePlayer).getTeam()) / GC.getLeaderHeadInfo(getPersonalityType()).getShareWarAttitudeDivisor());
			iAttitude += range(iAttitudeChange, -(abs(GC.getLeaderHeadInfo(getPersonalityType()).getShareWarAttitudeChangeLimit())), abs(GC.getLeaderHeadInfo(getPersonalityType()).getShareWarAttitudeChangeLimit()));
		}
	}

	return iAttitude;
}

// WTP, Schmiddie, Native Economic Diplomacy - START
int CvPlayerAI::AI_getNativeTradeTrust(PlayerTypes eIndex) const
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");

	return range(m_em_iNativeTradeTrust.get(eIndex), 0, 40);
}

void CvPlayerAI::AI_recordNativeTrade(PlayerTypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");

	if (!isNative() || GET_PLAYER(eIndex).isNative())
	{
		return;
	}

	const int iCurrentTurn = GC.getGameINLINE().getGameTurn();

	if (m_em_iNativeTradeLastTurn.get(eIndex) != iCurrentTurn)
	{
		m_em_iNativeTradeLastTurn.set(eIndex, iCurrentTurn);
		m_em_iNativeTradeCountThisTurn.set(eIndex, 0);
	}

	if (m_em_iNativeTradeCountThisTurn.get(eIndex) >= 2)
	{
		return;
	}

	m_em_iNativeTradeTrust.set(
		eIndex,
		std::min(40, m_em_iNativeTradeTrust.get(eIndex) + 1)
	);

	m_em_iNativeTradeCountThisTurn.set(
		eIndex,
		m_em_iNativeTradeCountThisTurn.get(eIndex) + 1
	);

	m_em_iNativeEconomicContactLastTurn.set(eIndex, iCurrentTurn);
}
// WTP, Schmiddie, Native Trade Trust - END

void CvPlayerAI::AI_recordNativeGift(PlayerTypes eIndex, int iGiftValue)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");

	if (!isNative() || GET_PLAYER(eIndex).isNative() || iGiftValue <= 0)
	{
		return;
	}

	const int iDivisor =
		(GET_TEAM(getTeam()).AI_getHasMetCounter(GET_PLAYER(eIndex).getTeam()) + 1) * 3;

	const int iGiftAttitude =
		range(iGiftValue / std::max(1, iDivisor), 0, 3);

	if (iGiftAttitude > 0)
	{
		m_em_iNativeGiftAttitude.set(
			eIndex,
			std::min(6, m_em_iNativeGiftAttitude.get(eIndex) + iGiftAttitude)
		);
	}

	m_em_iNativeEconomicContactLastTurn.set(
		eIndex,
		GC.getGameINLINE().getGameTurn()
	);
}
// WTP, Schmiddie, Native Economic Diplomacy - END

int CvPlayerAI::AI_getTradeAttitude(PlayerTypes ePlayer)
{
	// XXX human only?

	// WTP, Schmiddie, Native Economic Diplomacy - START
	if (isNative())
	{
		const int iGrantAttitude =
			range(m_em_iNativeGiftAttitude.get(ePlayer), 0, 6);

		const int iTradeAttitude =
			AI_getNativeTradeTrust(ePlayer) / 10;

		int iEconomicAttitude =
			range(iGrantAttitude + iTradeAttitude, 0, 10);

		const int iLastEconomicContactTurn =
			m_em_iNativeEconomicContactLastTurn.get(ePlayer);

		if (iLastEconomicContactTurn >= 0)
		{
			const int iTurnsSinceEconomicContact =
				GC.getGameINLINE().getGameTurn() - iLastEconomicContactTurn;

			if (iTurnsSinceEconomicContact >= 60)
			{
				const int iDecay =
					(iTurnsSinceEconomicContact - 50) / 10;

				iEconomicAttitude =
					std::max(0, iEconomicAttitude - iDecay);
			}
		}

		return iEconomicAttitude;
	}
	// WTP, Schmiddie, Native Economic Diplomacy - END

	return range(((AI_getPeacetimeGrantValue(ePlayer) + std::max(0, (AI_getPeacetimeTradeValue(ePlayer) - GET_PLAYER(ePlayer).AI_getPeacetimeTradeValue(getID())))) / ((GET_TEAM(getTeam()).AI_getHasMetCounter(GET_PLAYER(ePlayer).getTeam()) + 1) * 5)), 0, 4);
}

int CvPlayerAI::AI_getRivalTradeAttitude(PlayerTypes ePlayer)
{
	// XXX human only?
	return -(range(((GET_TEAM(getTeam()).AI_getEnemyPeacetimeGrantValue(GET_PLAYER(ePlayer).getTeam()) + (GET_TEAM(getTeam()).AI_getEnemyPeacetimeTradeValue(GET_PLAYER(ePlayer).getTeam()) / 3)) / ((GET_TEAM(getTeam()).AI_getHasMetCounter(GET_PLAYER(ePlayer).getTeam()) + 1) * 10)), 0, 4));
}


int CvPlayerAI::AI_getMemoryAttitude(PlayerTypes ePlayer, MemoryTypes eMemory)
{
	return ((AI_getMemoryCount(ePlayer, eMemory) * GC.getLeaderHeadInfo(getPersonalityType()).getMemoryAttitudePercent(eMemory)) / 100);
}

int CvPlayerAI::AI_dealVal(PlayerTypes ePlayer, const CLinkList<TradeData>* pList, bool bIgnoreAnnual, int iChange)
{
	CxDesyncMonitor StartMonitoring(CxDesyncMonitor::TYPE_BOTH);

	int iValue = 0;

	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");

	if (atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
	{
		iValue += GET_TEAM(getTeam()).AI_endWarVal(GET_PLAYER(ePlayer).getTeam());
	}

	for (CLLNode<TradeData>* pNode = pList->head(); pNode; pNode = pList->next(pNode))
	{
		FAssertMsg(!(pNode->m_data.m_bHidden), "(pNode->m_data.m_bHidden) did not return false as expected");

		switch (pNode->m_data.m_eItemType)
		{
		case TRADE_CITIES:
			iValue += AI_cityTradeVal(GET_PLAYER(ePlayer).getCity(pNode->m_data.m_iData1));
			break;
		case TRADE_GOLD:
			iValue += (pNode->m_data.m_iData1 * AI_goldTradeValuePercent()) / 100;
			break;
		case TRADE_YIELD:
			iValue += AI_yieldTradeVal((YieldTypes) pNode->m_data.m_iData1, pNode->m_data.m_kTransport, ePlayer);
			break;
		case TRADE_MAPS:
			iValue += GET_TEAM(getTeam()).AI_mapTradeVal(GET_PLAYER(ePlayer).getTeam());
			break;
		case TRADE_OPEN_BORDERS:
			iValue += GET_TEAM(getTeam()).AI_openBordersTradeVal(GET_PLAYER(ePlayer).getTeam());
			break;
		case TRADE_DEFENSIVE_PACT:
			iValue += GET_TEAM(getTeam()).AI_defensivePactTradeVal(GET_PLAYER(ePlayer).getTeam());
			break;
		case TRADE_PEACE:
			iValue += GET_TEAM(getTeam()).AI_makePeaceTradeVal(((TeamTypes)(pNode->m_data.m_iData1)), GET_PLAYER(ePlayer).getTeam());
			break;
		case TRADE_WAR:
			iValue += GET_TEAM(getTeam()).AI_declareWarTradeVal(((TeamTypes)(pNode->m_data.m_iData1)), GET_PLAYER(ePlayer).getTeam());
			break;
		case TRADE_EMBARGO:
			iValue += AI_stopTradingTradeVal(((TeamTypes)(pNode->m_data.m_iData1)), ePlayer);
			break;
		}
	}

	return iValue;
}


bool CvPlayerAI::AI_goldDeal(const CLinkList<TradeData>* pList)
{
	CLLNode<TradeData>* pNode;

	for (pNode = pList->head(); pNode; pNode = pList->next(pNode))
	{
		FAssert(!(pNode->m_data.m_bHidden));

		switch (pNode->m_data.m_eItemType)
		{
		case TRADE_GOLD:
			return true;
			break;
		}
	}

	return false;
}


bool CvPlayerAI::AI_considerOffer(PlayerTypes ePlayer, const CLinkList<TradeData>* pTheirList, const CLinkList<TradeData>* pOurList, int iChange)
{
	CxDesyncMonitor StartMonitoring(CxDesyncMonitor::TYPE_BOTH);

	CLLNode<TradeData>* pNode;
	int iThreshold;

	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");

	if (AI_goldDeal(pTheirList) && AI_goldDeal(pOurList))
	{
		return false;
	}

	if (iChange > -1)
	{
		for (pNode = pOurList->head(); pNode; pNode = pOurList->next(pNode))
		{
			if (getTradeDenial(ePlayer, pNode->m_data) != NO_DENIAL)
			{
				return false;
			}
		}
	}

	if (GET_PLAYER(ePlayer).getTeam() == getTeam())
	{
		return true;
	}

	if ((pOurList->getLength() == 0) && (pTheirList->getLength() > 0))
	{
		return true;
	}

	int iOurValue = GET_PLAYER(ePlayer).AI_dealVal(getID(), pOurList, false, iChange);
	int iTheirValue = AI_dealVal(ePlayer, pTheirList, false, iChange);

	if (iOurValue > 0 && 0 == pTheirList->getLength() && 0 == iTheirValue)
	{
		if (AI_getAttitude(ePlayer) < ATTITUDE_PLEASED)
		{
			if (GET_TEAM(getTeam()).getPower() > ((GET_TEAM(GET_PLAYER(ePlayer).getTeam()).getPower() * 4) / 3))
			{
				return false;
			}
		}

		if (AI_getMemoryCount(ePlayer, MEMORY_MADE_DEMAND_RECENT) > 0)
		{
			return false;
		}

		iThreshold = (GET_TEAM(getTeam()).AI_getHasMetCounter(GET_PLAYER(ePlayer).getTeam()) + 50);

		iThreshold *= 2;

		if (GET_TEAM(GET_PLAYER(ePlayer).getTeam()).AI_isLandTarget(getTeam()))
		{
			iThreshold *= 3;
		}

		iThreshold *= (GET_TEAM(GET_PLAYER(ePlayer).getTeam()).getPower() + 100);
		iThreshold /= (GET_TEAM(getTeam()).getPower() + 100);

		iThreshold -= GET_PLAYER(ePlayer).AI_getPeacetimeGrantValue(getID());

		return (iOurValue < iThreshold);
	}

	if (iChange < 0)
	{
		return (iTheirValue * 110 >= iOurValue * 100);
	}

	return (iTheirValue >= iOurValue);
}

int CvPlayerAI::AI_militaryHelp(PlayerTypes ePlayer, int& iNumUnits, UnitTypes& eUnit, ProfessionTypes& eProfession)
{
	FAssert(GET_PLAYER(ePlayer).getParent() == getID());

	iNumUnits = 0;
	eUnit = NO_UNIT;
	eProfession = NO_PROFESSION;

	if (AI_getAttitude(ePlayer) < ATTITUDE_CAUTIOUS)
	{
		return -1;
	}

	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	int iBestValue = MAX_INT;
	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		UnitTypes eLoopUnit = GC.getCivilizationInfo(kPlayer.getCivilizationType()).getCivilizationUnits(eUnitClass);
		if (eLoopUnit != NO_UNIT)
		{

			//WTP, Ray, fixing Unit Variation for Buying Units from King - START
			CvUnitInfo& kUnit = GC.getUnitInfo(eLoopUnit);

			if (kUnit.getDomainType() == DOMAIN_LAND && kPlayer.getEuropeUnitBuyPrice(eLoopUnit) > 0)
			{
				// check Combat Value of Unit
				bool isValidbyUnitCombat = (kUnit.getCombat() > 5);

				// check Combat Change From Profession
				ProfessionTypes checkProfession = GC.getUnitInfo(eLoopUnit).getDefaultProfession();
				bool isValidbyDefaultProfessionCombatChange = false;
				if (checkProfession != NO_PROFESSION)
				{
					isValidbyDefaultProfessionCombatChange = (GC.getProfessionInfo(checkProfession).getCombatChange() > 3);
				}

				bool bValid = (isValidbyUnitCombat || isValidbyDefaultProfessionCombatChange);
				//WTP, Ray, fixing Unit Variation for Buying Units from King - END

				//WTP, Ray, replaced because otherwise King gives Medics, which also have Promotions
				/*
				for (int j = 0; j < GC.getNumPromotionInfos() && !bValid; ++j)
				{
					if (kUnit.getFreePromotions(j))
					{
						bValid = true;
					}
				}
				*/

				if (bValid)
				{
					int iValue = kPlayer.getUnitClassCount(eUnitClass);
					if (iValue < iBestValue)
					{
						iBestValue = iValue;
						eUnit = eLoopUnit;
						//WTP, Ray, fixing Unit Variation for Buying Units from King - END
						eProfession = GC.getUnitInfo(eLoopUnit).getDefaultProfession();
					}
				}
			}
		}
	}

	if (eUnit == NO_UNIT)
	{
		return -1;
	}

	iNumUnits = 1;
	//WTP, Ray, fixing Unit Variation for Buying Units from King - END
	//eProfession = (ProfessionTypes) GC.getUnitInfo(eUnit).getDefaultProfession();

	return kPlayer.getEuropeUnitBuyPrice(eUnit) * GC.getDefineINT("KING_BUY_UNIT_PRICE_MODIFIER") / 100;
}

bool CvPlayerAI::AI_counterPropose(PlayerTypes ePlayer, const CLinkList<TradeData>* pTheirList, const CLinkList<TradeData>* pOurList, CLinkList<TradeData>* pTheirInventory, CLinkList<TradeData>* pOurInventory, CLinkList<TradeData>* pTheirCounter, CLinkList<TradeData>* pOurCounter, const IDInfo& kTransport)
{
	FAssert(CxDesyncMonitor::isNeverSync());
	CxDesyncMonitor StartMonitoring; // called in async from python. EXE calls this function through the virtual function pointer

	CLLNode<TradeData>* pNode;
	CLLNode<TradeData>* pBestNode;
	CLLNode<TradeData>* pGoldNode;
	CvCity* pCity;
	bool bTheirGoldDeal;
	bool bOurGoldDeal;
	int iHumanDealWeight;
	int iAIDealWeight;
	int iGoldData;
	int iGoldWeight;
	int iWeight;
	int iBestWeight;
	int iBestValue;

	bTheirGoldDeal = AI_goldDeal(pTheirList);
	bOurGoldDeal = AI_goldDeal(pOurList);

	CvPlayerAI& kTradingPartner = GET_PLAYER(ePlayer);


	// R&R, ray, Bargaining - START
	bool acceptsBargaining = this->isWillingToBargain(kTradingPartner.getID());

	if(acceptsBargaining)
	{
		if (bOurGoldDeal)
		{
			int priceIncreaseMax = GLOBAL_DEFINE_PRICE_INCREASE_BARGAIN_SELL;
			pNode = pOurInventory->head();
			if (pNode->m_data.m_eItemType == TRADE_GOLD)
			{
				int oldPrice = pNode->m_data.m_iData1;
				int randomPriceChange = GC.getGameINLINE().getAsyncRandom(priceIncreaseMax);
				if (randomPriceChange < priceIncreaseMax / 3)
				{
					randomPriceChange = priceIncreaseMax / 3;
				}
				int newPrice = oldPrice * (1000 + randomPriceChange) / 1000;

				// R&R, ray, change for Trait Trader - START
				// WTP, ray, also consider European Trait
				int iTotalNativeTradeModifier = GET_PLAYER(getID()).getNativeTradeModifier() + kTradingPartner.getNativeTradeModifier();
				// safety check to ensure that no numbers do not get too extreme
				if (iTotalNativeTradeModifier > GLOBAL_DEFINE_MAX_NATIVE_BARGAIN_MODIFIER)
				{
					iTotalNativeTradeModifier = GLOBAL_DEFINE_MAX_NATIVE_BARGAIN_MODIFIER;
				}
				// here we calculate the price
				newPrice = newPrice * (100 + iTotalNativeTradeModifier) / 100;
				// R&R, ray, change for Trait Trader - END

				// R&R, ray, small correction to stop at max gold of player
				int iGoldAvailable = this->AI_maxGoldTrade(ePlayer);
				if (newPrice >= iGoldAvailable)
				{
					newPrice = iGoldAvailable;
				}

				//setting value to new price
				pOurCounter->clear();
				CLLNode<TradeData>* newNode = pNode;
				newNode->m_data.m_iData1 = newPrice;
				pOurCounter->insertAtEnd(newNode->m_data);
			}
		}

		else if (bTheirGoldDeal)
		{
			int priceDecreaseMax = GLOBAL_DEFINE_PRICE_DECREASE_BARGAIN_BUY;
			pNode = pTheirInventory->head();
			if (pNode->m_data.m_eItemType == TRADE_GOLD)
			{
				int oldPrice = pNode->m_data.m_iData1;
				int randomPriceChange = GC.getGameINLINE().getAsyncRandom(priceDecreaseMax);
				if (randomPriceChange < priceDecreaseMax / 3)
				{
					randomPriceChange = priceDecreaseMax / 3;
				}
				int newPrice = oldPrice * (1000 - randomPriceChange) / 1000;

				// R&R, ray, change for Trait Trader - START
				// WTP, ray, also consider European Trait
				int iTotalNativeTradeModifier = this->getNativeTradeModifier() + kTradingPartner.getNativeTradeModifier();
				// safety check to ensure that no negative numbers occurs
				if (iTotalNativeTradeModifier > GLOBAL_DEFINE_MAX_NATIVE_BARGAIN_MODIFIER)
				{
					iTotalNativeTradeModifier = GLOBAL_DEFINE_MAX_NATIVE_BARGAIN_MODIFIER;
				}
				// here we calculate the price
				newPrice = newPrice * (100 - iTotalNativeTradeModifier) / 100;
				// R&R, ray, change for Trait Trader - END
				
				//setting value to new price
				pTheirCounter->clear();
				CLLNode<TradeData>* newNode = pNode;
				newNode->m_data.m_iData1 = newPrice;
				pTheirCounter->insertAtEnd(newNode->m_data);
			}
		}

		//at the end reset value for Bargaining
		this->applyBargainOutcome(kTradingPartner.getID(), 0, false);
		return true;
	}
	// R&R, ray, Bargaining - END

	if (bOurGoldDeal && bTheirGoldDeal)
	{
		return false;
	}

	pGoldNode = NULL;

	iHumanDealWeight = AI_dealVal(ePlayer, pTheirList);
	iAIDealWeight = GET_PLAYER(ePlayer).AI_dealVal(getID(), pOurList);

	int iGoldValuePercent = AI_goldTradeValuePercent();

	pTheirCounter->clear();
	pOurCounter->clear();

	if (iAIDealWeight > iHumanDealWeight)
	{
		if (atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
		{
			iBestValue = 0;
			iBestWeight = 0;
			pBestNode = NULL;

			for (pNode = pTheirInventory->head(); pNode && iAIDealWeight > iHumanDealWeight; pNode = pTheirInventory->next(pNode))
			{
				if (!pNode->m_data.m_bOffering && !pNode->m_data.m_bHidden)
				{
					if (pNode->m_data.m_eItemType == TRADE_CITIES)
					{
						FAssert(GET_PLAYER(ePlayer).canTradeItem(getID(), pNode->m_data));

						if (GET_PLAYER(ePlayer).getTradeDenial(getID(), pNode->m_data) == NO_DENIAL)
						{
							pCity = GET_PLAYER(ePlayer).getCity(pNode->m_data.m_iData1);

							if (pCity != NULL)
							{
								iWeight = AI_cityTradeVal(pCity);

								if (iWeight > 0)
								{
									const int iValue = AI_targetCityValue(pCity, false);

									if (iValue > iBestValue)
									{
										iBestValue = iValue;
										iBestWeight = iWeight;
										pBestNode = pNode;
									}
								}
							}
						}
					}
				}
			}

			if (pBestNode != NULL)
			{
				iHumanDealWeight += iBestWeight;
				pTheirCounter->insertAtEnd(pBestNode->m_data);
			}
		}

		for (pNode = pTheirInventory->head(); pNode && iAIDealWeight > iHumanDealWeight; pNode = pTheirInventory->next(pNode))
		{
			if (!pNode->m_data.m_bOffering && !pNode->m_data.m_bHidden)
			{
				FAssert(GET_PLAYER(ePlayer).canTradeItem(getID(), pNode->m_data));

				if (GET_PLAYER(ePlayer).getTradeDenial(getID(), pNode->m_data) == NO_DENIAL)
				{
					switch (pNode->m_data.m_eItemType)
					{
					case TRADE_GOLD:
						if (!bOurGoldDeal)
						{
							pGoldNode = pNode;
						}
						break;
					}
				}
			}
		}

		int iGoldWeight = iAIDealWeight - iHumanDealWeight;

		if (iGoldWeight > 0)
		{
			if (pGoldNode)
			{
				iGoldData = iGoldWeight * 100;
				iGoldData /= iGoldValuePercent;
				if ((iGoldData * iGoldValuePercent) < iGoldWeight)
				{
					iGoldData++;
				}
				if (GET_PLAYER(ePlayer).getMaxGoldTrade(getID(), kTransport) >= iGoldData)
				{
					pGoldNode->m_data.m_iData1 = iGoldData;
					iHumanDealWeight += (iGoldData * iGoldValuePercent) / 100;
					pTheirCounter->insertAtEnd(pGoldNode->m_data);
					pGoldNode = NULL;
				}
			}
		}

		for (pNode = pTheirInventory->head(); pNode && iAIDealWeight > iHumanDealWeight; pNode = pTheirInventory->next(pNode))
		{
			if (!pNode->m_data.m_bOffering && !pNode->m_data.m_bHidden)
			{
				if (pNode->m_data.m_eItemType == TRADE_MAPS)
				{
					FAssert(GET_PLAYER(ePlayer).canTradeItem(getID(), pNode->m_data));

					if (GET_PLAYER(ePlayer).getTradeDenial(getID(), pNode->m_data) == NO_DENIAL)
					{
						iWeight = GET_TEAM(getTeam()).AI_mapTradeVal(GET_PLAYER(ePlayer).getTeam());

						if (iWeight > 0)
						{
							iHumanDealWeight += iWeight;
							pTheirCounter->insertAtEnd(pNode->m_data);
						}
					}
				}
			}
		}

		iGoldWeight = iAIDealWeight - iHumanDealWeight;

		if (iGoldWeight > 0)
		{
			if (pGoldNode)
			{
				iGoldData = iGoldWeight * 100;
				iGoldData /= iGoldValuePercent;

				if ((iGoldWeight * 100) > (iGoldData * iGoldValuePercent))
				{
					iGoldData++;
				}

				iGoldData = std::min(iGoldData, GET_PLAYER(ePlayer).getMaxGoldTrade(getID(), kTransport));

				if (iGoldData > 0)
				{
					pGoldNode->m_data.m_iData1 = iGoldData;
					iHumanDealWeight += (iGoldData * iGoldValuePercent) / 100;
					pTheirCounter->insertAtEnd(pGoldNode->m_data);
					pGoldNode = NULL;
				}
			}
		}
	}
	else if (iHumanDealWeight > iAIDealWeight)
	{
		if (atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
		{
				for (pNode = pOurInventory->head(); pNode; pNode = pOurInventory->next(pNode))
				{
					if (!pNode->m_data.m_bOffering && !pNode->m_data.m_bHidden)
					{
						if (pNode->m_data.m_eItemType == TRADE_PEACE_TREATY)
						{
							pOurCounter->insertAtEnd(pNode->m_data);
							break;
						}
					}
				}

			iBestValue = 0;
			iBestWeight = 0;
			pBestNode = NULL;

			for (pNode = pOurInventory->head(); pNode && iHumanDealWeight > iAIDealWeight; pNode = pOurInventory->next(pNode))
			{
				if (!pNode->m_data.m_bOffering && !pNode->m_data.m_bHidden)
				{
					if (pNode->m_data.m_eItemType == TRADE_CITIES)
					{
						FAssert(canTradeItem(ePlayer, pNode->m_data));

						if (getTradeDenial(ePlayer, pNode->m_data) == NO_DENIAL)
						{
							pCity = getCity(pNode->m_data.m_iData1);

							if (pCity != NULL)
							{
								iWeight = GET_PLAYER(ePlayer).AI_cityTradeVal(pCity);

								if (iWeight > 0)
								{
									const int iValue = GET_PLAYER(ePlayer).AI_targetCityValue(pCity, false);

									if (iValue > iBestValue)
									{
										if (iHumanDealWeight >= (iAIDealWeight + iWeight))
										{
											iBestValue = iValue;
											iBestWeight = iWeight;
											pBestNode = pNode;
										}
									}
								}
							}
						}
					}
				}
			}

			if (pBestNode != NULL)
			{
				iAIDealWeight += iBestWeight;
				pOurCounter->insertAtEnd(pBestNode->m_data);
			}
		}

		for (pNode = pOurInventory->head(); pNode && iHumanDealWeight > iAIDealWeight; pNode = pOurInventory->next(pNode))
		{
			if (!pNode->m_data.m_bOffering && !pNode->m_data.m_bHidden)
			{
				FAssert(canTradeItem(ePlayer, pNode->m_data));

				if (getTradeDenial(ePlayer, pNode->m_data) == NO_DENIAL)
				{
					switch (pNode->m_data.m_eItemType)
					{
					case TRADE_GOLD:
						if (!bTheirGoldDeal)
						{
							pGoldNode = pNode;
						}
						break;
					}
				}
			}
		}

		iGoldWeight = iHumanDealWeight - iAIDealWeight;

		if (iGoldWeight > 0)
		{
			if (pGoldNode)
			{
				int iGoldData = iGoldWeight * 100;
				iGoldData /= iGoldValuePercent;

				if (getMaxGoldTrade(ePlayer, kTransport) >= iGoldData)
				{
					pGoldNode->m_data.m_iData1 = iGoldData;
					iAIDealWeight += ((iGoldData * iGoldValuePercent) / 100);
					pOurCounter->insertAtEnd(pGoldNode->m_data);
					pGoldNode = NULL;
				}
			}
		}

		for (pNode = pOurInventory->head(); pNode && iHumanDealWeight > iAIDealWeight; pNode = pOurInventory->next(pNode))
		{
			if (!pNode->m_data.m_bOffering && !pNode->m_data.m_bHidden)
			{
				if (pNode->m_data.m_eItemType == TRADE_MAPS)
				{
					FAssert(canTradeItem(ePlayer, pNode->m_data));

					if (getTradeDenial(ePlayer, pNode->m_data) == NO_DENIAL)
					{
						iWeight = GET_TEAM(GET_PLAYER(ePlayer).getTeam()).AI_mapTradeVal(getTeam());

						if (iWeight > 0)
						{
							if (iHumanDealWeight >= (iAIDealWeight + iWeight))
							{
								iAIDealWeight += iWeight;
								pOurCounter->insertAtEnd(pNode->m_data);
							}
						}
					}
				}
			}
		}

		iGoldWeight = iHumanDealWeight - iAIDealWeight;
		if (iGoldWeight > 0)
		{
			if (pGoldNode)
			{
				iGoldData = iGoldWeight * 100;
				iGoldData /= AI_goldTradeValuePercent();

				iGoldData = std::min(iGoldData, getMaxGoldTrade(ePlayer, kTransport));

				if (iGoldData > 0)
				{
					pGoldNode->m_data.m_iData1 = iGoldData;
					iAIDealWeight += (iGoldData * AI_goldTradeValuePercent()) / 100;
					pOurCounter->insertAtEnd(pGoldNode->m_data);
					pGoldNode = NULL;
				}
			}
		}
	}

	return ((iAIDealWeight <= iHumanDealWeight) && ((pOurList->getLength() > 0) || (pOurCounter->getLength() > 0) || (pTheirCounter->getLength() > 0)));
}


int CvPlayerAI::AI_maxGoldTrade(PlayerTypes ePlayer) const
{
	int iMaxGold;

	FAssert(ePlayer != getID());

	if (isHuman() || (GET_PLAYER(ePlayer).getTeam() == getTeam()))
	{
		iMaxGold = getGold();
	}
	else
	{
		iMaxGold = getTotalPopulation() * 10;

		iMaxGold *= (GET_TEAM(getTeam()).AI_getHasMetCounter(GET_PLAYER(ePlayer).getTeam()) + 10);

		iMaxGold *= GC.getLeaderHeadInfo(getPersonalityType()).getMaxGoldTradePercent();
		iMaxGold /= 100;

		iMaxGold -= AI_getGoldTradedTo(ePlayer);
		iMaxGold += GET_PLAYER(ePlayer).AI_getGoldTradedTo(getID());

		iMaxGold = std::min(iMaxGold, getGold());

		iMaxGold -= (iMaxGold % GC.getDefineINT("DIPLOMACY_VALUE_REMAINDER"));
	}

	return std::max(0, iMaxGold);
}

int CvPlayerAI::AI_cityTradeVal(CvCity* pCity, PlayerTypes eOwner)
{
	if (pCity == NULL)
	{
		return 0;
	}

	if (eOwner == NO_PLAYER)
	{
		eOwner = pCity->getOwnerINLINE();
	}
	FAssert(eOwner != getID());

	int iValue = 300;

	iValue += (pCity->getPopulation() * 50);
	iValue += (pCity->getCultureLevel() * 200);
	iValue += (((((pCity->getPopulation() * 50) + GC.getGameINLINE().getElapsedGameTurns() + 100) * 4) * pCity->plot()->calculateCulturePercent(eOwner)) / 100);

	if (!(pCity->isEverOwned(getID())))
	{
		iValue *= 3;
		iValue /= 2;
	}

	iValue -= (iValue % GC.getDefineINT("DIPLOMACY_VALUE_REMAINDER"));

	if (isHuman())
	{
		return std::max(iValue, GC.getDefineINT("DIPLOMACY_VALUE_REMAINDER"));
	}
	else
	{
		return iValue;
	}
}


DenialTypes CvPlayerAI::AI_cityTrade(CvCity* pCity, PlayerTypes ePlayer) const
{
	FAssert(pCity->getOwnerINLINE() == getID());

	if (pCity->getLiberationPlayer(false) == ePlayer)
	{
		return NO_DENIAL;
	}

	if (!(GET_PLAYER(ePlayer).isHuman()))
	{
		if (GET_PLAYER(ePlayer).getTeam() != getTeam())
		{
			if ((pCity->plot()->calculateCulturePercent(ePlayer) == 0) && !(pCity->isEverOwned(ePlayer)) && (GET_PLAYER(ePlayer).getNumCities() > 3))
			{
				CvCity* pNearestCity = GC.getMap().findCity(pCity->getX_INLINE(), pCity->getY_INLINE(), ePlayer, NO_TEAM, true, false, NO_TEAM, NO_DIRECTION, pCity);
				if ((pNearestCity == NULL) || (plotDistance(pCity->getX_INLINE(), pCity->getY_INLINE(), pNearestCity->getX_INLINE(), pNearestCity->getY_INLINE()) > 9))
				{
					return DENIAL_UNKNOWN;
				}
			}
		}
	}

	if (isHuman())
	{
		return NO_DENIAL;
	}

	if (atWar(getTeam(), GET_PLAYER(ePlayer).getTeam()))
	{
		return NO_DENIAL;
	}

	if (isNative() && !GET_PLAYER(ePlayer).isNative())
	{
		return NO_DENIAL;
	}

	if (GET_PLAYER(ePlayer).getTeam() != getTeam())
	{
		return DENIAL_NEVER;
	}

	if (pCity->calculateCulturePercent(getID()) > 50)
	{
		return DENIAL_TOO_MUCH;
	}

	return NO_DENIAL;
}


int CvPlayerAI::AI_stopTradingTradeVal(TeamTypes eTradeTeam, PlayerTypes ePlayer)
{
	CvDeal* pLoopDeal;
	int iModifier;
	int iValue;
	int iLoop;

	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");
	FAssertMsg(GET_PLAYER(ePlayer).getTeam() != getTeam(), "shouldn't call this function on ourselves");
	FAssertMsg(eTradeTeam != getTeam(), "shouldn't call this function on ourselves");
	FAssertMsg(GET_TEAM(eTradeTeam).isAlive(), "GET_TEAM(eWarTeam).isAlive is expected to be true");
	FAssertMsg(!atWar(eTradeTeam, GET_PLAYER(ePlayer).getTeam()), "eTeam should be at peace with eWarTeam");

	iValue = (50 + (GC.getGameINLINE().getGameTurn() / 2));
	iValue += (GET_TEAM(eTradeTeam).getNumCities() * 5);

	iModifier = 0;

	switch (GET_TEAM(GET_PLAYER(ePlayer).getTeam()).AI_getAttitude(eTradeTeam))
	{
	case ATTITUDE_FURIOUS:
		break;

	case ATTITUDE_ANNOYED:
		iModifier += 25;
		break;

	case ATTITUDE_CAUTIOUS:
		iModifier += 50;
		break;

	case ATTITUDE_PLEASED:
		iModifier += 100;
		break;

	case ATTITUDE_FRIENDLY:
		iModifier += 200;
		break;

	default:
		FAssert(false);
		break;
	}

	iValue *= std::max(0, (iModifier + 100));
	iValue /= 100;

	if (GET_TEAM(GET_PLAYER(ePlayer).getTeam()).isOpenBorders(eTradeTeam))
	{
		iValue *= 2;
	}

	if (GET_TEAM(GET_PLAYER(ePlayer).getTeam()).isDefensivePact(eTradeTeam))
	{
		iValue *= 3;
	}

	for(pLoopDeal = GC.getGameINLINE().firstDeal(&iLoop); pLoopDeal != NULL; pLoopDeal = GC.getGameINLINE().nextDeal(&iLoop))
	{
		if (pLoopDeal->isCancelable(getID()) && !(pLoopDeal->isPeaceDeal()))
		{
			if (GET_PLAYER(pLoopDeal->getFirstPlayer()).getTeam() == GET_PLAYER(ePlayer).getTeam())
			{
				if (pLoopDeal->getLengthSecondTrades() > 0)
				{
					iValue += (GET_PLAYER(pLoopDeal->getFirstPlayer()).AI_dealVal(pLoopDeal->getSecondPlayer(), pLoopDeal->getSecondTrades()) * ((pLoopDeal->getLengthFirstTrades() == 0) ? 2 : 1));
				}
			}

			if (GET_PLAYER(pLoopDeal->getSecondPlayer()).getTeam() == GET_PLAYER(ePlayer).getTeam())
			{
				if (pLoopDeal->getLengthFirstTrades() > 0)
				{
					iValue += (GET_PLAYER(pLoopDeal->getSecondPlayer()).AI_dealVal(pLoopDeal->getFirstPlayer(), pLoopDeal->getFirstTrades()) * ((pLoopDeal->getLengthSecondTrades() == 0) ? 2 : 1));
				}
			}
		}
	}

	iValue -= (iValue % GC.getDefineINT("DIPLOMACY_VALUE_REMAINDER"));

	if (isHuman())
	{
		return std::max(iValue, GC.getDefineINT("DIPLOMACY_VALUE_REMAINDER"));
	}
	else
	{
		return iValue;
	}
}


DenialTypes CvPlayerAI::AI_stopTradingTrade(TeamTypes eTradeTeam, PlayerTypes ePlayer) const
{
	AttitudeTypes eAttitude;
	AttitudeTypes eAttitudeThem;
	int iI;

	FAssertMsg(ePlayer != getID(), "shouldn't call this function on ourselves");
	FAssertMsg(GET_PLAYER(ePlayer).getTeam() != getTeam(), "shouldn't call this function on ourselves");
	FAssertMsg(eTradeTeam != getTeam(), "shouldn't call this function on ourselves");
	FAssertMsg(GET_TEAM(eTradeTeam).isAlive(), "GET_TEAM(eTradeTeam).isAlive is expected to be true");
	FAssertMsg(!atWar(getTeam(), eTradeTeam), "should be at peace with eTradeTeam");

	if (isHuman())
	{
		return NO_DENIAL;
	}

	eAttitude = GET_TEAM(getTeam()).AI_getAttitude(GET_PLAYER(ePlayer).getTeam());

	for (iI = 0; iI < MAX_PLAYERS; iI++)
	{
		if (GET_PLAYER((PlayerTypes)iI).isAlive())
		{
			if (GET_PLAYER((PlayerTypes)iI).getTeam() == getTeam())
			{
				if (eAttitude <= GC.getLeaderHeadInfo(GET_PLAYER((PlayerTypes)iI).getPersonalityType()).getStopTradingRefuseAttitudeThreshold())
				{
					return DENIAL_ATTITUDE;
				}
			}
		}
	}

	eAttitudeThem = GET_TEAM(getTeam()).AI_getAttitude(eTradeTeam);

	for (iI = 0; iI < MAX_PLAYERS; iI++)
	{
		if (GET_PLAYER((PlayerTypes)iI).isAlive())
		{
			if (GET_PLAYER((PlayerTypes)iI).getTeam() == getTeam())
			{
				if (eAttitudeThem > GC.getLeaderHeadInfo(GET_PLAYER((PlayerTypes)iI).getPersonalityType()).getStopTradingThemRefuseAttitudeThreshold())
				{
					return DENIAL_ATTITUDE_THEM;
				}
			}
		}
	}

	return NO_DENIAL;
}

int CvPlayerAI::AI_yieldTradeVal(YieldTypes eYield, const IDInfo& kTransport, PlayerTypes ePlayer)
{
	int iValue = 0;
	CvPlayerAI& kTradePlayer = GET_PLAYER(ePlayer);
	CvUnit* pTransport = ::getUnit(kTransport);
	if (pTransport != NULL)
	{
		int iAmount = kTradePlayer.getTradeYieldAmount(eYield, pTransport);
		if (isNative())
		{
			int iTotalStored = countTotalYieldStored(eYield);
			int iMaxStored;
			if (eYield == YIELD_FOOD)
			{
				iMaxStored = getNumCities() * getGrowthThreshold(1);
			}
			else
			{
				iMaxStored = getNumCities() * GC.getGameINLINE().getCargoYieldCapacity();
			}

			int iNativeConsumptionPercent = GC.getYieldInfo(eYield).getNativeConsumptionPercent();
			if (iNativeConsumptionPercent > 0)
			{
				iMaxStored *= iNativeConsumptionPercent;
				iMaxStored /= 80;
			}
			else
			{
				iMaxStored *= 2;
			}
			int iBuyPrice = GC.getYieldInfo(eYield).getNativeBuyPrice();
			CvCity* pCity = pTransport->plot()->getPlotCity();
			if (pCity != NULL)
			{
				if (eYield == pCity->AI_getDesiredYield() || eYield == YIELD_MUSKETS || eYield == YIELD_HORSES)
				{
					iBuyPrice *= 125;
					iBuyPrice /= 100;
				}
			}

			int iHighPricePercent = std::max(25, 100 - ((100 * iTotalStored) / iMaxStored));
			int iLowPricePercent = std::max(25, 100 - ((100 * std::min(iMaxStored, iTotalStored + iAmount)) / iMaxStored));

			iValue += (iAmount * iBuyPrice * (iHighPricePercent + iLowPricePercent)) / 200;
		}
		else if (kTradePlayer.isNative())
		{
			int iTotalStored = kTradePlayer.countTotalYieldStored(eYield);
			int iMaxStored;
			if (eYield == YIELD_FOOD)
			{
				iMaxStored = kTradePlayer.getNumCities() * getGrowthThreshold(1);
			}
			else
			{
				iMaxStored = kTradePlayer.getNumCities() * GC.getGameINLINE().getCargoYieldCapacity();
			}

			int iNativeConsumptionPercent = GC.getYieldInfo(eYield).getNativeConsumptionPercent();
			if (iNativeConsumptionPercent > 0)
			{
				iMaxStored *= iNativeConsumptionPercent;
				iMaxStored /= 80;
			}
			else
			{
				iMaxStored *= 2;
			}
			int iSellPrice = GC.getYieldInfo(eYield).getNativeSellPrice();

			int iHighPricePercent = std::max(25, 100 - ((100 * iTotalStored) / std::max(1, iMaxStored)));
			int iLowPricePercent = std::max(25, 100 - ((100 * std::max(0, iTotalStored - iAmount)) / std::max(1, iMaxStored)));

			iValue += (iAmount * iSellPrice * (iHighPricePercent + iLowPricePercent)) / 200;
		}
		else
		{
			CvCity* pCity = pTransport->plot()->getPlotCity();
			if (pCity != NULL)
			{
				/// Ist die Stadt vom Verk�ufer?
				if (ePlayer == pCity->getOwnerINLINE())
				{
					/// Wird das angefragt Handelsgut (von der KI) zur Weiterverarbeitung gebraucht?
					if (kTradePlayer.AI_isYieldFinalProduct(eYield))
					{
						iValue += kTradePlayer.getSellToEuropeProfit(eYield, iAmount) * 95 / 100;
					}
					else
					{
						/// Das Gut wird zur Weiterverarbeitung gebraucht, daher �berlegt sich die
						/// KI einen angemessenen Preis
						iValue += kTradePlayer.AI_yieldValue(eYield, true, iAmount);
					}
				}
				else
				{
					/// Verk�ufer ist nicht Besitzer der Stadt (Ich verkaufe der KI)
					iValue += kTradePlayer.AI_yieldValue(eYield, false, iAmount);

					if ( AI_isYieldFinalProduct(eYield) )
					{
						iValue = std::min(iValue, getSellToEuropeProfit(eYield, iAmount) * 90 / 100);
					}
					else
					{
						/// Wenn ein kein Endprodukt ist bezahlt der K�ufer h�chsten 90% von dem f�r den Preis er es selbst verkaufen w�rde
						iValue = std::min(iValue, AI_yieldValue(eYield, true, iAmount) * 90 / 100);
					}
				}
			}
		}
	}
	// TAC - Endlos-Handelsbug Fix - Netbandit - DIV-1.06 - ENDE

	return iValue;
}

DenialTypes CvPlayerAI::AI_yieldTrade(YieldTypes eYield, const IDInfo& kTransport, PlayerTypes ePlayer) const
{
	CvUnit* pTransport = ::getUnit(kTransport);
	if (pTransport == NULL)
	{
		FAssert(pTransport != NULL);
		return NO_DENIAL;
	}
	CvCity* pCity = pTransport->plot()->getPlotCity();
	if (pCity != NULL)
	{
		CvPlayer& kPlayer = GET_PLAYER(pCity->getOwnerINLINE());
		if (kPlayer.isNative())
		{
			if (getID() == pCity->getOwnerINLINE())
			{
				if (GC.getYieldInfo(eYield).getNativeSellPrice() == -1)
				{
					return DENIAL_NEVER;
				}
			}
			else
			{
				if (pCity->AI_getDesiredYield() == eYield)
				{
					return NO_DENIAL;
				}
				if (GC.getYieldInfo(eYield).getNativeBuyPrice() == -1)
				{
					return DENIAL_NOT_INTERESTED;
				}
			}

			bool bCanProduce = pCity->canProduceYield(eYield);
			if (eYield == YIELD_HORSES)
			{
				bCanProduce = false;
			}

			if (getID() == pCity->getOwnerINLINE())
			{
				if (!bCanProduce)
				{
					return DENIAL_UNKNOWN;
				}
			}
			else
			{
				if (bCanProduce)
				{
					return DENIAL_NO_GAIN;
				}
			}
		}
		else
		{
			if (getID() == pCity->getOwnerINLINE())
			{
				if (eYield == YIELD_BLADES || eYield == YIELD_MUSKETS || eYield == YIELD_CANNONS || eYield == YIELD_BLACK_POWDER || eYield == YIELD_HORSES)
				{
					return DENIAL_NEVER;
				}
				if (AI_shouldBuyFromEurope(eYield))
				{
					return DENIAL_NO_GAIN;
				}
				if (pCity->calculateActualYieldConsumed(eYield) > 0)
				{
					return DENIAL_NO_GAIN;
				}
			}
		}
	}

	return NO_DENIAL;
}

// WTP: gold for iAmount of a yield in a trade with this native tribe, computed without a ship on the spot.
// bNativeBuys: the natives pay for it (we sell), otherwise they ask for it (we buy). Same formula as AI_yieldTradeVal:
// the price falls with the stock of the whole tribe, down to 25%. Returns -1 if the natives do not trade the yield that way.
int CvPlayerAI::AI_nativeYieldTradeValue(YieldTypes eYield, int iAmount, bool bNativeBuys, const CvCity* pCity) const
{
	FAssert(isNative());
	if (!isNative() || iAmount <= 0)
	{
		return -1;
	}

	const CvYieldInfo& kYield = GC.getYieldInfo(eYield);
	int iPrice = bNativeBuys ? kYield.getNativeBuyPrice() : kYield.getNativeSellPrice();
	if (iPrice <= 0)
	{
		return -1;
	}
	if (bNativeBuys && pCity != NULL && (eYield == pCity->AI_getDesiredYield() || eYield == YIELD_MUSKETS || eYield == YIELD_HORSES))
	{
		iPrice = iPrice * 125 / 100;
	}

	const int iTotalStored = countTotalYieldStored(eYield);
	int iMaxStored = getNumCities() * ((eYield == YIELD_FOOD) ? getGrowthThreshold(1) : GC.getGameINLINE().getCargoYieldCapacity());
	if (kYield.getNativeConsumptionPercent() > 0)
	{
		iMaxStored = iMaxStored * kYield.getNativeConsumptionPercent() / 80;
	}
	else
	{
		iMaxStored *= 2;
	}
	iMaxStored = std::max(1, iMaxStored);

	// the stock grows by the amount if the natives buy, and shrinks by it if they sell
	const int iStockAfter = bNativeBuys ? std::min(iMaxStored, iTotalStored + iAmount) : std::max(0, iTotalStored - iAmount);
	const int iHighPricePercent = std::max(25, 100 - (100 * iTotalStored) / iMaxStored);
	const int iLowPricePercent = std::max(25, 100 - (100 * iStockAfter) / iMaxStored);

	return iAmount * iPrice * (iHighPricePercent + iLowPricePercent) / 200;
}

// WTP: does this native village buy the yield from us? The denial rules of AI_yieldTrade (implementDeal does not check them)
bool CvPlayerAI::AI_nativeCityBuys(YieldTypes eYield, const CvCity& kCity) const
{
	if (kCity.AI_getDesiredYield() == eYield)
	{
		return true;
	}
	if (GC.getYieldInfo(eYield).getNativeBuyPrice() <= 0)
	{
		return false;
	}
	// a yield the village produces itself is of no use to it (horses are always wanted)
	return eYield == YIELD_HORSES || !kCity.canProduceYield(eYield);
}

// WTP: native villages are told apart by the coordinates of their plot
namespace
{
	int nativeVillageKey(const CvCity& kCity)
	{
		return kCity.getX_INLINE() + kCity.getY_INLINE() * 10000;
	}
}

// WTP: turn the native trade ship last visited a village, -1 if never
int CvPlayerAI::AI_getNativeVillageVisitTurn(const CvCity& kCity) const
{
	const std::map<int, int>::const_iterator it = m_mapNativeVillageVisit.find(nativeVillageKey(kCity));
	return (it == m_mapNativeVillageVisit.end()) ? -1 : it->second;
}

void CvPlayerAI::AI_setNativeVillageVisited(const CvCity& kCity)
{
	m_mapNativeVillageVisit[nativeVillageKey(kCity)] = GC.getGameINLINE().getGameTurn();
}

bool CvPlayerAI::AI_hasVisitedNativeVillage() const
{
	return !m_mapNativeVillageVisit.empty();
}

// WTP: plots from the village to our nearest colony (a large number without colonies)
int CvPlayerAI::AI_nativeVillageDistanceToColonies(const CvCity& kCity) const
{
	int iBest = 1000;
	int iLoop;
	for (const CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		iBest = std::min(iBest, plotDistance(kCity.getX_INLINE(), kCity.getY_INLINE(), pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE()));
	}
	return iBest;
}

// WTP: the colonial player (not ourselves, humans included) the natives like least, judged by the sum of the attitudes of
// all native tribes towards it. NO_PLAYER if there is none.
PlayerTypes CvPlayerAI::AI_leastLikedColonialPlayer() const
{
	PlayerTypes eWorst = NO_PLAYER;
	int iWorstAttitude = MAX_INT;
	for (PlayerTypes eColonial = FIRST_PLAYER; eColonial < NUM_PLAYER_TYPES; ++eColonial)
	{
		const CvPlayer& kColonial = GET_PLAYER(eColonial);
		if (eColonial == getID() || !kColonial.isAlive() || !kColonial.is(CIV_CATEGORY_COLONIAL) || kColonial.getNumCities() == 0)
		{
			continue;
		}
		int iAttitude = 0;
		for (PlayerTypes eNative = FIRST_PLAYER; eNative < NUM_PLAYER_TYPES; ++eNative)
		{
			if (GET_PLAYER(eNative).isAlive() && GET_PLAYER(eNative).isNative())
			{
				iAttitude += GET_PLAYER(eNative).AI_getAttitudeVal(eColonial, false);
			}
		}
		if (iAttitude < iWorstAttitude)
		{
			iWorstAttitude = iAttitude;
			eWorst = eColonial;
		}
	}
	return eWorst;
}

// WTP: the smallest cargo space a ship needs to carry a treasure (galleons); such ships are kept for the treasures
int CvPlayerAI::AI_minTreasureTransportSize() const
{
	int iMin = MAX_INT;
	for (int iUnit = 0; iUnit < GC.getNumUnitInfos(); ++iUnit)
	{
		const CvUnitInfo& kUnit = GC.getUnitInfo((UnitTypes)iUnit);
		if (kUnit.getDefaultUnitAIType() == UNITAI_TREASURE)
		{
			iMin = std::min(iMin, kUnit.getRequiredTransportSize());
		}
	}
	return iMin;
}

// WTP: every colonial AI keeps one native trade ship once it has a colony. It is a transport that cannot carry
// treasures and has no passengers; the one with the largest cargo space is taken (also as the replacement of a lost one).
void CvPlayerAI::AI_updateNativeTradeShip()
{
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || getNumCities() == 0)
	{
		return;
	}

	// one ship (the second one of the early strategy Ship Trader gets its role when it is bought)
	int iTradeShips = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		const UnitAIStates eState = pLoopUnit->AI_getUnitAIState();
		if (pLoopUnit->getDomainType() == DOMAIN_SEA && (eState == UNITAI_STATE_NATIVE_TRADE_BUY || eState == UNITAI_STATE_NATIVE_TRADE_EUROPE || eState == UNITAI_STATE_NATIVE_TRADE_SELL))
		{
			++iTradeShips;
		}
	}
	if (iTradeShips >= 1)
	{
		return;
	}

	const int iTreasureSize = AI_minTreasureTransportSize();
	CvUnit* pBest = NULL;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getDomainType() == DOMAIN_SEA && pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA && pLoopUnit->AI_getUnitAIState() != UNITAI_STATE_EXPLORER_SHIP
			&& pLoopUnit->cargoSpace() < iTreasureSize && !pLoopUnit->hasAnyUnitInCargo() && (pBest == NULL || pLoopUnit->cargoSpace() > pBest->cargoSpace()))
		{
			pBest = pLoopUnit;
		}
	}
	if (pBest != NULL)
	{
		pBest->AI_setUnitAIState(UNITAI_STATE_NATIVE_TRADE_BUY);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_TRADE, getID(), "WTP native trade: Player %d (%S) turn %d makes %S (cargo %d, treasures need %d) its native trade ship",
			getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), pBest->getName().GetCString(), pBest->cargoSpace(), iTreasureSize);
	}
}

// WTP: profit of buying iAmount of a yield from a native tribe and selling it in Europe (after tax).
// False if the natives do not sell it or Europe does not buy it.
bool CvPlayerAI::AI_nativeBuyProfit(YieldTypes eYield, int iAmount, PlayerTypes eNative, int& iProfit) const
{
	iProfit = 0;
	if (eNative == NO_PLAYER || !GET_PLAYER(eNative).isNative() || getParent() == NO_PLAYER || !isYieldEuropeTradable(eYield))
	{
		return false;
	}
	const int iCost = GET_PLAYER(eNative).AI_nativeYieldTradeValue(eYield, iAmount, false, NULL);
	if (iCost < 0)
	{
		return false;
	}
	iProfit = getSellToEuropeProfit(eYield, iAmount) - iCost;
	return true;
}

// WTP: profit of buying iAmount of a yield in Europe and selling it to a native village. The payment is limited by
// the gold the tribe is willing to spend on us. False if the natives do not buy it or Europe does not sell it.
bool CvPlayerAI::AI_nativeSellProfit(YieldTypes eYield, int iAmount, const CvCity& kNativeCity, int& iProfit) const
{
	iProfit = 0;
	const PlayerTypes eNative = kNativeCity.getOwnerINLINE();
	if (!GET_PLAYER(eNative).isNative() || getParent() == NO_PLAYER || !GET_PLAYER(getParent()).isEurope() || !isYieldEuropeTradable(eYield))
	{
		return false;
	}
	int iRevenue = GET_PLAYER(eNative).AI_nativeYieldTradeValue(eYield, iAmount, true, &kNativeCity);
	if (iRevenue < 0)
	{
		return false;
	}
	iRevenue = std::min(iRevenue, GET_PLAYER(eNative).AI_maxGoldTrade(getID()));
	iProfit = iRevenue - GET_PLAYER(getParent()).getYieldSellPrice(eYield) * iAmount;
	return true;
}

// WTP: the AI bargains once for a better price when it sells to natives (the bargaining of the diplomacy screen is
// Python driven, the AI has no access to it). Same rules: the natives may refuse (they do not trade with us for some
// turns and like us less), otherwise the price rises by 7 to 21% plus the trader bonus, never above what the tribe may pay.
// Returns true if the price was raised. iPrice is the price in gold the natives pay for the whole deal.
bool CvPlayerAI::AI_bargainWithNatives(PlayerTypes eNative, int& iPrice)
{
	CvPlayerAI& kNative = GET_PLAYER(eNative);
	if (!kNative.isNative() || iPrice <= 0)
	{
		return false;
	}

	const int iAttitudeLevel = (int)kNative.AI_getAttitude(getID(), false);
	const int iRandomBase = std::max(1, 1000 + GLOBAL_DEFINE_CHANCE_IMPROVEMENT_ATTITUDE * iAttitudeLevel);
	if (GC.getGameINLINE().getSorenRandNum(iRandomBase, "AI bargain sell fails") < GLOBAL_DEFINE_BASE_CHANCE_BARGAIN_SELL_FAIL)
	{
		const int iMaxTurns = GLOBAL_DEFINE_ROUNDS_NO_TALK_BECAUSE_BAD_BARGAINING * GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getTrainPercent() / 100;
		const int iTurns = std::max(1, (iMaxTurns > 0) ? GC.getGameINLINE().getSorenRandNum(iMaxTurns, "AI bargain anger turns") : 0);
		kNative.applyBargainOutcome(getID(), iTurns, false);
		return false;
	}

	const int iMaxIncrease = GLOBAL_DEFINE_PRICE_INCREASE_BARGAIN_SELL;
	const int iIncrease = std::max(iMaxIncrease / 3, GC.getGameINLINE().getSorenRandNum(std::max(1, iMaxIncrease), "AI bargain sell price"));
	const int iModifier = std::min(getNativeTradeModifier() + kNative.getNativeTradeModifier(), GLOBAL_DEFINE_MAX_NATIVE_BARGAIN_MODIFIER);
	int iNewPrice = iPrice * (1000 + iIncrease) / 1000;
	iNewPrice = iNewPrice * (100 + iModifier) / 100;
	iNewPrice = std::min(iNewPrice, kNative.AI_maxGoldTrade(getID()));
	if (iNewPrice <= iPrice)
	{
		return false;
	}
	iPrice = iNewPrice;
	return true;
}

int CvPlayerAI::AI_calculateDamages(TeamTypes eTeam)
{
	int iValue = 0;

	int iStolenPlotCost = 0;
	int iPopulationCost = 0;
	for (int iI = 0; iI < MAX_PLAYERS; ++iI)
	{
		PlayerTypes ePlayer = (PlayerTypes)iI;
		CvPlayer& kPlayer = GET_PLAYER(ePlayer);
		if (kPlayer.isAlive())
		{
			if (kPlayer.getTeam() == eTeam)
			{
				int iStolenPlots = 0;
				for(int i=0;i<GC.getMap().numPlotsINLINE();i++)
				{
					CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(i);
					if(pLoopPlot->getOwnerINLINE() == ePlayer && pLoopPlot->getCulture(getID()) > pLoopPlot->getCulture(ePlayer))
					{
						iStolenPlotCost += pLoopPlot->getBuyPrice(ePlayer);
					}
				}

				iPopulationCost += kPlayer.getTotalPopulation() * 100;
			}
		}
	}

	iPopulationCost += 250;

	iValue += iPopulationCost;

	iValue += iStolenPlotCost / 4;

	return iValue;
}

// Encodes binary movement abilities (i.e. passability)  into a bitset. The bit is set when the unit CANNOT enter
// the corresponding feature/terrain/plot/rival
UnitImpassables CvPlayerAI::AI_unitImpassables(const CvUnit& kUnit) const
{
	const unsigned int numTerrains = (unsigned int)GC.getNumTerrainInfos();
	const unsigned int numFeatures = (unsigned int)GC.getNumFeatureInfos();
	const unsigned int PEAK_BIT = numTerrains + numFeatures;
	const unsigned int RIVAL_TERRITORY_BIT = PEAK_BIT + 1;
	const unsigned int totalBits = RIVAL_TERRITORY_BIT + 1;

	FAssert(totalBits <= UNIT_IMPASSABLES_BITS);

	UnitImpassables kResult;
	kResult.iFlags.reset();
	kResult.iCount = 0;

	const CvUnitInfo& kUnitInfo = kUnit.getUnitInfo();
	const ProfessionTypes eProfession = kUnit.getProfession();

	// Base unit abilities
	bool bCanMoveIntoPeak = kUnitInfo.allowsMoveIntoPeak();
	bool bCanCrossLargeRivers = !kUnitInfo.getTerrainImpassable(TERRAIN_LARGE_RIVERS);

	// Profession overrides the base unit abilities for these cases
	if (eProfession != NO_PROFESSION)
	{
		const CvProfessionInfo& kProfessionInfo = GC.getProfessionInfo(eProfession);

		bCanMoveIntoPeak = kProfessionInfo.allowsMoveIntoPeak();
		bCanCrossLargeRivers = kProfessionInfo.isCanCrossLargeRivers();
	}

	// Terrain impassables
	for (int i = 0; i < (int)numTerrains; ++i)
	{
		const TerrainTypes eTerrain = (TerrainTypes)i;
		bool bImpassable = false;

		if (eTerrain == TERRAIN_LARGE_RIVERS)
		{
			// Large rivers crossing ability that is enabled by the profession is coalesced into the terrain bitset
			bImpassable = !bCanCrossLargeRivers;
		}
		else
		{
			bImpassable = kUnitInfo.getTerrainImpassable(eTerrain);
		}

		if (bImpassable)
		{
			kResult.iFlags.set((unsigned int)i);
			kResult.iCount++;
		}
	}

	// Feature impassables
	for (int i = 0; i < (int)numFeatures; ++i)
	{
		if (kUnitInfo.getFeatureImpassable((FeatureTypes)i))
		{
			kResult.iFlags.set(numTerrains + (unsigned int)i);
			kResult.iCount++;
		}
	}

	// Peaks need their own bit because they are plot type, not terrain/feature.
	if (!bCanMoveIntoPeak)
	{
		// Note that this ability is only on the profession level
		kResult.iFlags.set(PEAK_BIT);
		kResult.iCount++;
	}

	// Rival territory is a separate binary enterability rule.
	if (!kUnit.isRivalTerritory())
	{
		kResult.iFlags.set(RIVAL_TERRITORY_BIT);
		kResult.iCount++;
	}

	return kResult;
}


//Calculates the value of the unit as "Profit generated in 20 turns" in pCity,
//or if pCity is NULL it assumes that it will either found somewhere
//or has some other role.
int CvPlayerAI::AI_unitEconomicValue(UnitTypes eUnit, UnitAITypes* peUnitAI, CvCity* pCity)
{
	UnitAITypes eBestUnitAI = NO_UNITAI;

	if (getNumCities() == 0)
	{
		return 1;
	}

	CvUnitInfo& kUnitInfo = GC.getUnitInfo(eUnit);
	int iBestValue = 0;
	if (kUnitInfo.getUnitAIType(UNITAI_COLONIST))
	{
		if (pCity == NULL)
		{
			//Do we have an ideal profession?
			for (int iI = 0; iI < NUM_YIELD_TYPES; iI++)
			{
				YieldTypes eYield = (YieldTypes)iI;

				if (kUnitInfo.getYieldModifier(eYield) > 0)
				{
					for(ProfessionTypes eProfession = FIRST_PROFESSION; eProfession < NUM_PROFESSION_TYPES; ++eProfession)
					{
						if (GC.getCivilizationInfo(getCivilizationType()).isValidProfession(eProfession))
						{
							const CvProfessionInfo& kProfessionInfo = GC.getProfessionInfo(eProfession);
							// R&R, ray , MYCP partially based on code of Aymerick - START
							if (kProfessionInfo.getNumYieldsProduced() >0 && kProfessionInfo.getYieldsProduced(0) == eYield)
							{
								if (kProfessionInfo.getNumYieldsConsumed() == 0)
								{
									// R&R, ray , MYCP partially based on code of Aymerick - END
									if (kProfessionInfo.isWorkPlot())
									{
										CvPlot* pCenter = AI_getTerritoryCenter();
										int iRadius = AI_getTerritoryRadius();
										for (int iX = -iRadius; iX <= iRadius; iX++)
										{
											for (int iY = -iRadius; iY <= iRadius; iY++)
											{
												CvPlot* pLoopPlot = plotXY(pCenter->getX_INLINE(), pCenter->getY_INLINE(), iX, iY);
												if ((pLoopPlot != NULL) && (pLoopPlot->getOwnerINLINE() == getID()) && (!pLoopPlot->isCity()))
												{
													int iAmount = pLoopPlot->calculatePotentialYield(eYield, NULL, false);

													iAmount *= 100 + kUnitInfo.getYieldModifier(eYield);
													iAmount /= 100;

													int iValue = 20 * AI_yieldValue(eYield) * iAmount;
													bool bValid = true;
													if (pLoopPlot->isBeingWorked())
													{
														iValue *= 75;
														iValue /= 100;

														CvCity* pCity = pLoopPlot->getWorkingCity();
														FAssert(pCity!= NULL);

														CvUnit* pWorkingUnit = pCity->getUnitWorkingPlot(pLoopPlot);
														FAssert(pWorkingUnit != NULL);

														if (pWorkingUnit != NULL)
														{
															FAssert(pWorkingUnit->getProfession() != NO_PROFESSION);
															// R&R, ray , MYCP partially based on code of Aymerick - START
															YieldTypes eTempYield = (YieldTypes)GC.getProfessionInfo(pWorkingUnit->getProfession()).getYieldsProduced(0);
															// R&R, ray , MYCP partially based on code of Aymerick - END

															int iTempAmount = pLoopPlot->getYield(eYield);
															int iTempValue = 20 * AI_yieldValue(eTempYield) * iTempAmount;

															if (iValue > (iTempValue * 4) / 3)
															{
																bValid = false;
															}
														}
													}

													if (bValid)
													{
														if (iValue > iBestValue)
														{
															iBestValue = iValue;
															eBestUnitAI = UNITAI_COLONIST;
														}
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (kUnitInfo.getUnitAIType(UNITAI_WAGON))
	{
		int iValue = 0;
		if (pCity != NULL)
		{
			int iCityCount = pCity->area()->getCitiesPerPlayer(getID());
			int iWagonCount = AI_totalAreaUnitAIs(pCity->area(), UNITAI_WAGON);

			int iNeededWagons = iCityCount;
			if (iNeededWagons < iWagonCount)
			{
				iValue += 100 * kUnitInfo.getCargoSpace();
			}
		}

		if (iValue > iBestValue)
		{
			iBestValue = iValue;
			eBestUnitAI = UNITAI_WAGON;
		}
	}

	if (kUnitInfo.getUnitAIType(UNITAI_TRANSPORT_COAST))
	{
		int iValue = 0;

		if (pCity != NULL)
		{
			CvArea* const pWaterArea = pCity->waterArea();

			if (pWaterArea != NULL)
			{
				const int iCoastalCityCount = countNumCoastalCities();
				const int iTransportCount = AI_totalWaterAreaUnitAIs(pWaterArea, UNITAI_TRANSPORT_COAST);

				int iNeededTransports = iCoastalCityCount / 2;
				if (iNeededTransports < iTransportCount)
				{
					iValue += 50 * kUnitInfo.getCargoSpace();
				}
			}
		}

		if (iValue > iBestValue)
		{
			iBestValue = iValue;
			eBestUnitAI = UNITAI_TRANSPORT_COAST;
		}
	}

	if (peUnitAI != NULL)
	{
		*peUnitAI = eBestUnitAI;
	}

	return iBestValue;
}

int CvPlayerAI::AI_unitValue(UnitTypes eUnit, UnitAITypes eUnitAI, CvArea* pArea)
{
	bool bValid;
	int iCombatValue;
	int iValue;

	FAssertMsg(eUnit != NO_UNIT, "Unit is not assigned a valid value");
	FAssertMsg(eUnitAI != NO_UNITAI, "UnitAI is not assigned a valid value");
	CvUnitInfo& kUnitInfo = GC.getUnitInfo(eUnit);

	if (kUnitInfo.getDomainType() != AI_unitAIDomainType(eUnitAI))
	{
		return 0;
	}

	if (kUnitInfo.getNotUnitAIType(eUnitAI))
	{
		return 0;
	}

	bValid = kUnitInfo.getUnitAIType(eUnitAI);

	if (!bValid)
	{
		switch (eUnitAI)
		{
		case UNITAI_UNKNOWN:
			break;
		case UNITAI_COLONIST:
			break;
		case UNITAI_ANIMAL: // R&R, ray, Wild Animals
			break;
		case UNITAI_ANIMAL_SEA: // R&R, ray, Wild Animals
			break;
		case UNITAI_FLEEING: // R&R, ray, Fleeing Units
			break;
		case UNITAI_SETTLER:
			break;
		case UNITAI_WORKER:
			break;
		case UNITAI_MISSIONARY:
			break;
		case UNITAI_TRADER: // WTP, ray, Native Trade Posts - START
			break;
		case UNITAI_SCOUT:
			break;
		case UNITAI_WAGON:
			break;
		case UNITAI_TREASURE:
			break;
		case UNITAI_YIELD:
			break;
		case UNITAI_GENERAL:
			break;
		case UNITAI_DEFENSIVE:
			break;
		case UNITAI_OFFENSIVE:
			break;
		case UNITAI_COUNTER:
			break;

		//TAC Whaling, ray
		case UNITAI_WORKER_SEA:
			if (kUnitInfo.isGatherBoat() && kUnitInfo.getMoves() > 0)
			{
				bValid = true;
			}
			break;

		case UNITAI_TRANSPORT_SEA:
		case UNITAI_TRANSPORT_COAST:
			break;
		case UNITAI_ASSAULT_SEA:
			// TAC - AI Assault Sea - koma13 - START
			//if ((kUnitInfo.getCargoSpace() > 0) && kUnitInfo.getMoves() > 0)
			if ((kUnitInfo.getCargoSpace() > 0) && (kUnitInfo.getMoves() > 0) && (kUnitInfo.getSpecialCargo() == NO_SPECIALUNIT))
			// TAC - AI Assault Sea - koma13 - END
			{
				bValid = true;
			}
			break;
		case UNITAI_COMBAT_SEA:
			break;
		case UNITAI_PIRATE_SEA:
			if (kUnitInfo.isHiddenNationality())
			{
				if ((kUnitInfo.getCombat() > 0) && kUnitInfo.getMoves() > 0)
				{
					bValid = true;
				}
			}
			break;

		// TAC - AI Escort Sea - koma13 - START
		case UNITAI_ESCORT_SEA:
			if (kUnitInfo.getCombat() > 0)
			{
				if (kUnitInfo.getCargoSpace() == 0)
				{
					bValid = true;
				}
			}
			break;
		// TAC - AI Escort Sea - koma13 - END

		default:
			FAssert(false);
			break;
		}
	}

	if (!bValid)
	{
		return 0;
	}

	iCombatValue = GC.getGameINLINE().AI_combatValue(eUnit);

	iValue = 100;

	iValue += kUnitInfo.getAIWeight();

	int iEuropeCost = getEuropeUnitBuyPrice(eUnit);
	if (iEuropeCost > 0)
	{
		iValue += iEuropeCost;
	}

	return std::max(0, iValue);
}


//This function attempts to return how much gold this unit is worth.
int CvPlayerAI::AI_unitGoldValue(UnitTypes eUnit, UnitAITypes eUnitAI, CvArea* pArea) const
{
	bool bValid = false;
	int iValue = 0;

	FAssertMsg(eUnit != NO_UNIT, "Unit is not assigned a valid value");
	FAssertMsg(eUnitAI != NO_UNITAI, "UnitAI is not assigned a valid value");
	CvUnitInfo& kUnitInfo = GC.getUnitInfo(eUnit);

	if (kUnitInfo.getDomainType() != AI_unitAIDomainType(eUnitAI))
	{
		return 0;
	}

	if (kUnitInfo.getNotUnitAIType(eUnitAI))
	{
		return 0;
	}

	bValid = kUnitInfo.getUnitAIType(eUnitAI);

	if (!bValid)
	{
		switch (eUnitAI)
		{
		case UNITAI_UNKNOWN:
		case UNITAI_ANIMAL: // R&R, ray, Wild Animals
		case UNITAI_ANIMAL_SEA: // R&R, ray, Wild Animals
		case UNITAI_FLEEING: // R&R, ray, Fleeing Units
			break;

		case UNITAI_SETTLER:
			{
				// This fixes the bug that caused townguard units to be assigned
				// the settler ai. Note that we cannot check the unit by itself since most units
				// have bFound set to true, thus we need to check the default profession as well
				const ProfessionTypes eProfession = (ProfessionTypes)kUnitInfo.getDefaultProfession();
				if (eProfession != NO_PROFESSION)
				{
					const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);

					if (kUnitInfo.isFound() && kProfession.canFound())
					{
						bValid = true;
					}
				}
			}
			break;

		case UNITAI_COLONIST:
		case UNITAI_WORKER:
		case UNITAI_MISSIONARY:
		case UNITAI_TRADER: // WTP, ray, Native Trade Posts - START
		case UNITAI_SCOUT:
			if (kUnitInfo.getDefaultProfession() != NO_PROFESSION)
			{
				bValid = true;
			}
			break;

		case UNITAI_WAGON:
			if (kUnitInfo.getCargoSpace() > 0)
			{
				bValid = true;
			}
			break;

		case UNITAI_TREASURE:
			break;

		case UNITAI_YIELD:
			break;

		case UNITAI_GENERAL:
			break;

		case UNITAI_DEFENSIVE:
			if (kUnitInfo.getDefaultProfession() != NO_PROFESSION)
			{
				bValid = true;
			}
			break;

		case UNITAI_OFFENSIVE:
			if (kUnitInfo.getBombardRate() > 0)
			{
				bValid = true;
			}
			break;

		case UNITAI_COUNTER:
			if (kUnitInfo.getDefaultProfession() != NO_PROFESSION)
			{
				bValid = true;
			}
			break;

		//TAC Whaling, ray
		case UNITAI_WORKER_SEA:
			if (kUnitInfo.isGatherBoat())
			{
				bValid = true;
			}
			break;

		case UNITAI_TRANSPORT_SEA:
		case UNITAI_TRANSPORT_COAST:
			if (kUnitInfo.getCargoSpace() > 0)
			{
				bValid = true;
			}
			break;

		case UNITAI_ASSAULT_SEA:
			// TAC - AI Assault Sea - koma13 - START
			//if ((kUnitInfo.getCargoSpace() > 0) && kUnitInfo.getMoves() > 0)
			if ((kUnitInfo.getCargoSpace() > 0) && (kUnitInfo.getMoves() > 0) && (kUnitInfo.getSpecialCargo() == NO_SPECIALUNIT))
			// TAC - AI Assault Sea - koma13 - END
			{
				bValid = true;
			}
			break;

		case UNITAI_COMBAT_SEA:
			// TAC - AI purchases military units - koma13 - START
			//if (!kUnitInfo.isOnlyDefensive() && (kUnitInfo.getCombat() > 0) && kUnitInfo.getMoves() > 0)
			if (!kUnitInfo.isOnlyDefensive() && (kUnitInfo.getCombat() > 0) && kUnitInfo.getMoves() > 0 && kUnitInfo.getDefaultUnitAIType() != UNITAI_TRANSPORT_SEA && !kUnitInfo.isHiddenNationality())
			// TAC - AI purchases military units - koma13 - END
			{
				bValid = true;
			}
			break;
		case UNITAI_PIRATE_SEA:
			if (kUnitInfo.isHiddenNationality())
			{
				if ((kUnitInfo.getCombat() > 0) && kUnitInfo.getMoves() > 0)
				{
					bValid = true;
				}
			}
			break;

		// TAC - AI Escort Sea - koma13 - START
		case UNITAI_ESCORT_SEA:
			if (!kUnitInfo.isOnlyDefensive() && (kUnitInfo.getCombat() > 0) && kUnitInfo.getMoves() > 0)
			{
				if (kUnitInfo.getCargoSpace() == 0)
				{
					bValid = true;
				}
			}
			break;
		// TAC - AI Escort Sea - koma13 - END

		default:
			FAssert(false);
			break;
		}
	}

	if (!bValid)
	{
		return 0;
	}
	//This function specifically tries to estimate the gold value of a unit.

	int iOffenseCombatValue = kUnitInfo.getCombat() * 100;
	int iDefenseCombatValue = kUnitInfo.getCombat() * 100;

	if (kUnitInfo.isOnlyDefensive())
	{
		iOffenseCombatValue /= 4;
	}

	int iCargoValue = kUnitInfo.getCargoSpace() * 250;

	if (kUnitInfo.getDefaultProfession() != NO_PROFESSION)
	{
		iValue += std::max(0, kUnitInfo.getEuropeCost());
	}

	int iTempValue;

	switch (eUnitAI)
	{
	case UNITAI_UNKNOWN:
	case UNITAI_ANIMAL: // R&R, ray, Wild Animals
	case UNITAI_ANIMAL_SEA: // R&R, ray, Wild Animals
	case UNITAI_FLEEING: // R&R, ray, Fleeing Units
		break;

	case UNITAI_COLONIST:
	case UNITAI_SETTLER:
	case UNITAI_WORKER:
	case UNITAI_MISSIONARY:
	case UNITAI_TRADER: // WTP, ray, Native Trade Posts - START
	case UNITAI_SCOUT:
		break;

	case UNITAI_WAGON:
	case UNITAI_TRANSPORT_COAST:
		iTempValue = iCargoValue + iDefenseCombatValue / 2;
		iTempValue *= 1 + kUnitInfo.getMoves();
		iTempValue /= 2;
		iValue += iTempValue;
		break;


	case UNITAI_TREASURE:
		break;

	case UNITAI_YIELD:
		break;

	case UNITAI_GENERAL:
		break;

	case UNITAI_DEFENSIVE:
		iTempValue = iOffenseCombatValue / 2;
		iTempValue += iDefenseCombatValue;
		iTempValue += kUnitInfo.getBombardRate() * 50;
		iValue += iTempValue;
		break;

	case UNITAI_OFFENSIVE:
		iTempValue = iOffenseCombatValue;
		iTempValue += iDefenseCombatValue / 2;
		iTempValue += kUnitInfo.getBombardRate() * 50;

		iTempValue *= 2 + kUnitInfo.getMoves();
		iTempValue /= 3;
		iValue += iTempValue;
		break;

	case UNITAI_COUNTER:
		iTempValue = iOffenseCombatValue * 2 / 3;
		iTempValue += iDefenseCombatValue * 2 / 3;
		iTempValue += kUnitInfo.getBombardRate() * 50;

		iTempValue *= 2 + kUnitInfo.getMoves();
		iTempValue /= 3;
		iValue += iTempValue;
		break;

	//TAC Whaling, ray
	case UNITAI_WORKER_SEA:
		//iValue += ((4 + kUnitInfo.getMoves()) * ((kUnitInfo.getNumYieldsGatherable() * 250) + iDefenseCombatValue / 2)) / 6;

		iTempValue = ((4 + kUnitInfo.getMoves()) * (iCargoValue + iDefenseCombatValue / 2)) / 6;
		iTempValue += (kUnitInfo.getNumYieldsGatherable() * 250) / 6;
		iValue += iTempValue;
		break;

	case UNITAI_TRANSPORT_SEA:

		iValue += ((4 + kUnitInfo.getMoves()) * (iCargoValue + iDefenseCombatValue / 2)) / 7;

		break;

	case UNITAI_ASSAULT_SEA:
		// TAC - AI Assault Sea - koma13 - START
		FAssert(kUnitInfo.getSpecialCargo() == NO_SPECIALUNIT);
		if (kUnitInfo.getSpecialCargo() == NO_SPECIALUNIT)
		{
			iTempValue = iDefenseCombatValue + iCargoValue;
			iTempValue *= 4 + kUnitInfo.getMoves();
			iTempValue /= 7;
			if (kUnitInfo.isHiddenNationality())
			{
				iTempValue /= 2;
			}
			iValue += iTempValue;
		}
		// TAC - AI Assault Sea - koma13 - END
		break;

	case UNITAI_COMBAT_SEA:
		iTempValue = iOffenseCombatValue * 2 + iDefenseCombatValue;
		iTempValue *= 4 + kUnitInfo.getMoves();
		iTempValue /= 8;
		if (kUnitInfo.isHiddenNationality())
		{
			iTempValue /= 2;
		}
		iValue += iTempValue;
		break;

	case UNITAI_PIRATE_SEA:
		iTempValue = iOffenseCombatValue + iDefenseCombatValue + iCargoValue / 2;
		iTempValue *= 3 + kUnitInfo.getMoves();
		iTempValue /= 6;
		iValue += iTempValue;
		break;

	// TAC - AI Escort Sea - koma13 - START
	case UNITAI_ESCORT_SEA:
		iTempValue = iOffenseCombatValue * 2 + iDefenseCombatValue;
		iTempValue *= 4 + kUnitInfo.getMoves();
		iTempValue /= 8;
		if (kUnitInfo.isHiddenNationality())
		{
			iTempValue /= 2;
		}
		iValue += iTempValue;
		break;
	// TAC - AI Escort Sea - koma13 - END

	default:
		FAssert(false);
		break;
	}

#if 0
	const int iEuropeCost = kUnitInfo.getEuropeCost();

	// Normalize unit costs to prevent the AI from repeating purchases of the same
	// unit when the cost escalates
	if (iEuropeCost > 0)
	{
		// Since the value is not scaled and only compared to similarly scaled values
		// it's ok to multiply here to prevent truncatin after the division.
		iValue *= 100;
		iValue /= iEuropeCost;
	}

	//iValue +=  kUnitInfo.getAIWeight();
#endif
	return std::max(0, iValue);
}

//WTP, Schmiddie, Unit Availability Change Projekt START - more units 
int CvPlayerAI::AI_unitValuePercent(UnitTypes eUnit, UnitAITypes* peUnitAI, CvArea* pArea)
{
	FAssertMsg(eUnit != NO_UNIT, "Unit is not assigned a valid value");
	CvUnitInfo& kUnitInfo = GC.getUnitInfo(eUnit);

	int iValue = 0;
	int iGoldCost = getEuropeUnitBuyPrice(eUnit);

	if (iGoldCost <= 0)
	{
		return -1;
	}

	const int iCurrentYear = GC.getGameINLINE().getGameTurnYear();

	// Basic AI core fleet, fully data-driven:
	// Make sure the AI buys at least one currently relevant large trade ship
	// and one currently relevant heavy warship.
	if (kUnitInfo.getDomainType() == DOMAIN_SEA)
	{
		bool bIsLargeTradeShip = (kUnitInfo.getCargoSpace() >= 6);
		bool bIsHeavyWarship = (!kUnitInfo.isHiddenNationality() && !kUnitInfo.isOnlyDefensive() && kUnitInfo.getCombat() >= 8 && kUnitInfo.getMoves() > 0);

		if (bIsLargeTradeShip || bIsHeavyWarship)
		{
			bool bHasLargeTradeShip = false;
			bool bHasHeavyWarship = false;

			int iLoop;
			CvUnit* pLoopUnit;
			for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
			{
				const CvUnitInfo& kLoopUnitInfo = pLoopUnit->getUnitInfo();

				if (kLoopUnitInfo.getDomainType() != DOMAIN_SEA)
				{
					continue;
				}

				const int iLoopObsoleteYear = kLoopUnitInfo.getGameYearObsolete();

				if (iLoopObsoleteYear > 0 && iCurrentYear > iLoopObsoleteYear)
				{
					continue;
				}

				if (kLoopUnitInfo.getCargoSpace() >= 6)
				{
					bHasLargeTradeShip = true;
				}

				if (!kLoopUnitInfo.isHiddenNationality() && !kLoopUnitInfo.isOnlyDefensive() && kLoopUnitInfo.getCombat() >= 8 && kLoopUnitInfo.getMoves() > 0)
				{
					bHasHeavyWarship = true;
				}
			}

			if (bIsLargeTradeShip && !bHasLargeTradeShip)
			{
				iValue += 900 + kUnitInfo.getCargoSpace() * 80;
			}

			if (bIsHeavyWarship && !bHasHeavyWarship)
			{
				iValue += 900 + kUnitInfo.getCombat() * 40 + kUnitInfo.getMoves() * 20;
			}
		}
	}

	//Transport Sea
	int iCargoSpace = kUnitInfo.getCargoSpace();
	if ((iCargoSpace > 1) && (kUnitInfo.getDomainType() == DOMAIN_SEA))
	{
		iValue += iCargoSpace * 30;

		int iTransportCount = AI_totalUnitAIs(UNITAI_TRANSPORT_SEA);
		int iCoastalCityCount = countNumCoastalCities();
		int iNeededTransports = std::max(6, 2 + iCoastalCityCount);

		if (iTransportCount == 0)
		{
			iValue += 300 + 100 * iCargoSpace;
		}
		else if (AI_totalUnitAIs(UNITAI_TREASURE) > 0)
		{
			int iLargestTreasureUnit = 0;
			int iTotalTreasure = 0;
			bool bValid = true;

			int iLoop;
			CvUnit* pLoopUnit;
			for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
			{
				if (pLoopUnit->canMove())
				{
					if (pLoopUnit->cargoSpace() > 0)
					{
						if (pLoopUnit->cargoSpace() >= iCargoSpace)
						{
							bValid = false;
							break;
						}
					}
					if (pLoopUnit->AI_getUnitAIType() == UNITAI_TREASURE)
					{
						int iSize = pLoopUnit->getUnitInfo().getRequiredTransportSize();
						if (iSize > 1)
						{
							iLargestTreasureUnit = std::max(iLargestTreasureUnit, iSize);
							if (iCargoSpace >= iSize)
							{
								iTotalTreasure += pLoopUnit->getYieldStored(); // WTP: the gold, not the yield type
							}
						}
					}
				}
			}

			if (kUnitInfo.getCargoSpace() >= iLargestTreasureUnit)
			{
				iValue += 100 + ((100 * iTotalTreasure) / iGoldCost);
			}
		}

		if (iTransportCount < iNeededTransports)
		{
			int iBestTransportSize = 1;
			int iLoop;
			CvUnit* pLoopUnit;
			for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
			{
				if (pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA)
				{
					iBestTransportSize = std::max(iBestTransportSize, pLoopUnit->cargoSpace());
				}
			}

			iValue += 100 + 40 * (iNeededTransports - iTransportCount);

			if (iCargoSpace == iBestTransportSize)
			{
				iValue += 50;
			}
			else if (iCargoSpace > iBestTransportSize)
			{
				iValue += 150 + 80 * (iCargoSpace - iBestTransportSize);
			}
		}
	}

	//Warships
	if (kUnitInfo.getDomainType() == DOMAIN_SEA)
	{
		if (kUnitInfo.getCombat() > 0)
		{
			int iCoastalCityCount = countNumCoastalCities();

			//Pirate
			if (getTotalPopulation() > 8)
			{
				if (kUnitInfo.isHiddenNationality())
				{
					int iPirateCount = AI_totalUnitAIs(UNITAI_PIRATE_SEA);
					int iNeededPirates = std::max(1, iCoastalCityCount / 4);

					if (iPirateCount < iNeededPirates)
					{
						iValue += 100 + 50 * (iNeededPirates - iPirateCount);
						iValue += getNumCities() * 8;
						iValue += kUnitInfo.getCombat() * 4;
					}
				}
			}

			//Regular warships
			if (!kUnitInfo.isHiddenNationality() && !kUnitInfo.isOnlyDefensive() && kUnitInfo.getMoves() > 0)
			{
				int iWarshipCount = AI_totalUnitAIs(UNITAI_COMBAT_SEA) + AI_totalUnitAIs(UNITAI_ESCORT_SEA) + AI_numWarshipsAsTransports();
				int iNeededWarships = std::max(6, 2 + iCoastalCityCount);

				if (iWarshipCount < iNeededWarships)
				{
					iValue += 150 + 60 * (iNeededWarships - iWarshipCount);
					iValue += kUnitInfo.getCombat() * 8;
					iValue += kUnitInfo.getMoves() * 15;
				}
			}
		}
	}

	return iValue;
}
//WTP, Schmiddie, Unit Availability Change Projekt END - more units

int CvPlayerAI::AI_totalUnitAIs(UnitAITypes eUnitAI)
{
	return (AI_getNumTrainAIUnits(eUnitAI) + AI_getNumAIUnits(eUnitAI));
}

// WTP, Schmiddie, Native military unit cap START
bool CvPlayerAI::AI_hasNativeColonialBorderContact() const
{
	if (!isNative())
	{
		return false;
	}

	for (int iPlot = 0; iPlot < GC.getMap().numPlotsINLINE(); ++iPlot)
	{
		CvPlot* pPlot = GC.getMap().plotByIndexINLINE(iPlot);

		if (pPlot == NULL || pPlot->getOwnerINLINE() != getID())
		{
			continue;
		}

		for (int iDirection = 0; iDirection < NUM_DIRECTION_TYPES; ++iDirection)
		{
			CvPlot* pAdjacentPlot = plotDirection(
				pPlot->getX_INLINE(),
				pPlot->getY_INLINE(),
				(DirectionTypes)iDirection);

			if (pAdjacentPlot == NULL)
			{
				continue;
			}

			const PlayerTypes eAdjacentOwner = pAdjacentPlot->getOwnerINLINE();

			if (eAdjacentOwner != NO_PLAYER &&
				GET_PLAYER(eAdjacentOwner).is(CIV_CATEGORY_COLONIAL))
			{
				return true;
			}
		}
	}

	return false;
}
bool CvPlayerAI::AI_isNativeMilitaryUnitCapReached()
{
	if (!isNative())
	{
		return false;
	}

	const int iMilitaryUnitCount =
		AI_totalUnitAIs(UNITAI_DEFENSIVE) +
		AI_totalUnitAIs(UNITAI_OFFENSIVE) +
		AI_totalUnitAIs(UNITAI_COUNTER);

	const int iBaseCap =
		GC.getDefineINT("NATIVE_MILITARY_UNIT_CAP");

	const int iBorderContactCap =
		GC.getDefineINT("NATIVE_MILITARY_UNIT_CAP_BORDER_CONTACT");

	if (iMilitaryUnitCount < iBaseCap)
	{
		return false;
	}

	if (iMilitaryUnitCount >= iBorderContactCap)
	{
		return true;
	}

	return !AI_hasNativeColonialBorderContact();
}
// WTP, Schmiddie, Native military unit cap END

int CvPlayerAI::AI_totalAreaUnitAIs(CvArea* pArea, UnitAITypes eUnitAI)
{
	return (pArea->getNumTrainAIUnits(getID(), eUnitAI) + pArea->getNumAIUnits(getID(), eUnitAI));
}


int CvPlayerAI::AI_totalWaterAreaUnitAIs(CvArea* pArea, UnitAITypes eUnitAI)
{
	CvCity* pLoopCity;
	int iCount;
	int iLoop;
	int iI;

	iCount = AI_totalAreaUnitAIs(pArea, eUnitAI);

	for (iI = 0; iI < MAX_PLAYERS; iI++)
	{
		if (GET_PLAYER((PlayerTypes)iI).isAlive())
		{
			for (pLoopCity = GET_PLAYER((PlayerTypes)iI).firstCity(&iLoop); pLoopCity != NULL; pLoopCity = GET_PLAYER((PlayerTypes)iI).nextCity(&iLoop))
			{
				if (pLoopCity->waterArea() == pArea)
				{
					iCount += pLoopCity->plot()->plotCount(PUF_isUnitAIType, eUnitAI, -1, getID());

					if (pLoopCity->getOwnerINLINE() == getID())
					{
						iCount += pLoopCity->getNumTrainUnitAI(eUnitAI);
					}
				}
			}
		}
	}


	return iCount;
}

bool CvPlayerAI::AI_hasSeaTransport(const CvUnit* pCargo) const
{
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit != pCargo && pLoopUnit->getDomainType() == DOMAIN_SEA)
		{
			if (pLoopUnit->cargoSpace() >= pCargo->getUnitInfo().getRequiredTransportSize())
			{
				return true;
			}

		}
	}
	return false;
}

int CvPlayerAI::AI_neededExplorers(CvArea* pArea)
{
	FAssert(pArea != NULL);
	int iNeeded = 0;

	if (pArea->isWater())
	{
		iNeeded = std::min(iNeeded + (pArea->getNumUnrevealedTiles(getTeam()) / 400), std::min(2, ((getNumCities() / 2) + 1)));
	}
	else
	{
		iNeeded = std::min(iNeeded + (pArea->getNumUnrevealedTiles(getTeam()) / 150), std::min(3, ((getNumCities() / 3) + 2)));
	}

	if (0 == iNeeded)
	{
		if ((GC.getGameINLINE().countCivTeamsAlive() - 1) > GET_TEAM(getTeam()).getHasMetCivCount())
		{
			if (pArea->isWater())
			{
				if (GC.getMap().findBiggestArea(true) == pArea)
				{
					iNeeded++;
				}
			}
			else
			{
				if (getPrimaryCity() != NULL && pArea->getID() == getPrimaryCity()->getArea())
				{
					for (int iPlayer = 0; iPlayer < MAX_PLAYERS; iPlayer++)
					{
						CvPlayerAI& kPlayer = GET_PLAYER((PlayerTypes)iPlayer);
						if (kPlayer.isAlive() && kPlayer.getTeam() != getTeam())
						{
							if (!GET_TEAM(getTeam()).isHasMet(kPlayer.getTeam()))
							{
								if (pArea->getCitiesPerPlayer(kPlayer.getID()) > 0)
								{
									iNeeded++;
									break;
								}
							}
						}
					}
				}
			}
		}
	}
	return iNeeded;

}

//Returns an estimate of needed pioneers. If pArea is NULL then all areas will be considered (i.e. the total need of all our cities)
int CvPlayerAI::AI_neededWorkers(CvArea* pArea) const
{
	CvCity* pLoopCity;
	int iCount;
	int iLoop;

	iCount = 0;

	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if ((pArea == NULL) || (pLoopCity->getArea() == pArea->getID()))
		{
			iCount += pLoopCity->AI_getWorkersNeeded();
		}
	}

	// Subtract pioneers waiting on the docks
	for (uint i = 0; i < m_aEuropeUnits.size(); ++i)
	{
		if (m_aEuropeUnits[i]->AI_getUnitAIType() == UNITAI_WORKER)
		{
			--iCount;
		}
	}

	// WTP: on top of the improvement needs about one pioneer per colony (roads, later improvements)
	int iCities = 0;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if ((pArea == NULL) || (pLoopCity->getArea() == pArea->getID()))
		{
			++iCities;
		}
	}

	if (iCount <= 0)
	{
		return iCities;
	}

	// WTP: at most two pioneers more than colonies (7 pioneers were wanted for 3 colonies)
	return std::min(iCities + 2, std::max(1, (iCount * 2) / 3) + iCities);
}

int CvPlayerAI::AI_adjacentPotentialAttackers(CvPlot* pPlot, bool bTestCanMove)
{
	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	CvPlot* pLoopPlot;
	int iCount;
	int iI;

	iCount = 0;

	for (iI = 0; iI < NUM_DIRECTION_TYPES; iI++)
	{
		pLoopPlot = plotDirection(pPlot->getX_INLINE(), pPlot->getY_INLINE(), ((DirectionTypes)iI));

		if (pLoopPlot != NULL)
		{
			if (pLoopPlot->area() == pPlot->area())
			{
				pUnitNode = pLoopPlot->headUnitNode();

				while (pUnitNode != NULL)
				{
					pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

					if (pLoopUnit != NULL && pLoopUnit->getOwnerINLINE() == getID())
					{
						if (pLoopUnit->getDomainType() == ((pPlot->isWater()) ? DOMAIN_SEA : DOMAIN_LAND))
						{
							if (pLoopUnit->canAttack())
							{
								if (!bTestCanMove || pLoopUnit->canMove())
								{
									if (!(pLoopUnit->AI_isCityAIType()))
									{
										iCount++;
									}
								}
							}
						}
					}
				}
			}
		}
	}

	return iCount;
}


// TAC - AI City Defense - koma13 - START
//int CvPlayerAI::AI_totalMissionAIs(MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup)
int CvPlayerAI::AI_totalMissionAIs(MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup, UnitAITypes eUnitAI)
// TAC - AI City Defense - koma13 - END
{
	PROFILE_FUNC();

	CvSelectionGroup* pLoopSelectionGroup;
	int iCount;
	int iLoop;

	iCount = 0;

	for(pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if (pLoopSelectionGroup != pSkipSelectionGroup)
		{
			if (pLoopSelectionGroup->AI_getMissionAIType() == eMissionAI)
			{
				// TAC - AI City Defense - koma13 - START
				//iCount += pLoopSelectionGroup->getNumUnits();
				if ((eUnitAI == NO_UNITAI) || (pLoopSelectionGroup->getHeadUnitAI() == eUnitAI))
				{
					iCount += pLoopSelectionGroup->getNumUnits();
				}
				// TAC - AI City Defense - koma13 - END
			}
		}
	}

	return iCount;
}

int CvPlayerAI::AI_areaMissionAIs(CvArea* pArea, MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup)
{
	PROFILE_FUNC();

	CvSelectionGroup* pLoopSelectionGroup;
	CvPlot* pMissionPlot;
	int iCount;
	int iLoop;

	iCount = 0;

	for(pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if (pLoopSelectionGroup != pSkipSelectionGroup)
		{
			if (pLoopSelectionGroup->AI_getMissionAIType() == eMissionAI)
			{
				pMissionPlot = pLoopSelectionGroup->AI_getMissionAIPlot();

				if (pMissionPlot != NULL)
				{
					if (pMissionPlot->getArea() == pArea->getID())
					{
						iCount += pLoopSelectionGroup->getNumUnits();
					}
				}
			}
		}
	}

	return iCount;
}


int CvPlayerAI::AI_adjacantToAreaMissionAIs(CvArea* pArea, MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup)
{
	PROFILE_FUNC();

	CvSelectionGroup* pLoopSelectionGroup;
	CvPlot* pMissionPlot;
	int iCount;
	int iLoop;

	iCount = 0;

	for(pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if (pLoopSelectionGroup != pSkipSelectionGroup)
		{
			if (pLoopSelectionGroup->AI_getMissionAIType() == eMissionAI)
			{
				pMissionPlot = pLoopSelectionGroup->AI_getMissionAIPlot();

				if (pMissionPlot != NULL)
				{
					if (pMissionPlot->isAdjacentToArea(pArea->getID()))
					{
						iCount += pLoopSelectionGroup->getNumUnits();
					}
				}
			}
		}
	}

	return iCount;
}


int CvPlayerAI::AI_plotTargetMissionAIs(CvPlot* pPlot, MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup, int iRange)
{
	int iClosestTargetRange;
	return AI_plotTargetMissionAIs(pPlot, &eMissionAI, 1, iClosestTargetRange, pSkipSelectionGroup, iRange);
}

int CvPlayerAI::AI_plotTargetMissionAIs(CvPlot* pPlot, MissionAITypes eMissionAI, int& iClosestTargetRange, CvSelectionGroup* pSkipSelectionGroup, int iRange)
{
	return AI_plotTargetMissionAIs(pPlot, &eMissionAI, 1, iClosestTargetRange, pSkipSelectionGroup, iRange);
}

int CvPlayerAI::AI_plotTargetMissionAIs(CvPlot* pPlot, MissionAITypes* aeMissionAI, int iMissionAICount, int& iClosestTargetRange, CvSelectionGroup* pSkipSelectionGroup, int iRange)
{
	PROFILE_FUNC();

	int iCount = 0;
	iClosestTargetRange = MAX_INT;

	CvSelectionGroup* pTransportSelectionGroup = NULL;
	if (pSkipSelectionGroup != NULL)
	{
		CvUnit* pHeadUnit = pSkipSelectionGroup->getHeadUnit();
		if (pHeadUnit->getTransportUnit() != NULL)
		{
			pTransportSelectionGroup = pHeadUnit->getTransportUnit()->getGroup();
		}
	}
	int iLoop;
	for(CvSelectionGroup* pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if ((pSkipSelectionGroup == NULL) || ((pLoopSelectionGroup != pSkipSelectionGroup) && (pLoopSelectionGroup != pTransportSelectionGroup)))
		{
			CvPlot* pMissionPlot = pLoopSelectionGroup->AI_getMissionAIPlot();

			if (pMissionPlot != NULL)
			{
				MissionAITypes eGroupMissionAI = pLoopSelectionGroup->AI_getMissionAIType();
				int iDistance = stepDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pMissionPlot->getX_INLINE(), pMissionPlot->getY_INLINE());

				if (iDistance <= iRange)
				{
					for (int iMissionAIIndex = 0; iMissionAIIndex < iMissionAICount; iMissionAIIndex++)
					{
						if (eGroupMissionAI == aeMissionAI[iMissionAIIndex] || aeMissionAI[iMissionAIIndex] == NO_MISSIONAI)
						{
							if ((eGroupMissionAI != MISSIONAI_GUARD_CITY) || ((eGroupMissionAI == MISSIONAI_GUARD_CITY) && (pMissionPlot != pLoopSelectionGroup->plot())))	// TAC - AI City Defense - koma13
							{
								iCount += pLoopSelectionGroup->getNumUnits();

								if (iDistance < iClosestTargetRange)
								{
									iClosestTargetRange = iDistance;
								}
							}
						}
					}
				}
			}
		}
	}

	return iCount;
}
// TAC - AI Improved Naval AI - koma13 - START
int CvPlayerAI::AI_cargoSpaceToEurope(CvSelectionGroup* pSkipSelectionGroup)
{
	int iCount = 0;
	int iLoop;

	for(CvSelectionGroup* pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if ((pSkipSelectionGroup == NULL) || (pLoopSelectionGroup != pSkipSelectionGroup))
		{
			CvUnit* pHeadUnit = pLoopSelectionGroup->getHeadUnit();

			if ((pHeadUnit != NULL) && (pHeadUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA))
			{
				MissionAITypes eMissionAI = pLoopSelectionGroup->AI_getMissionAIType();

				if ((eMissionAI == MISSIONAI_SAIL_TO_EUROPE) || (eMissionAI == MISSIONAI_SAIL_TO_AFRICA) || ((pHeadUnit->AI_getUnitAIState() == UNITAI_STATE_SAIL) && (eMissionAI != MISSIONAI_PICKUP)))
				{
					iCount += pHeadUnit->cargoSpace();
				}
			}
		}
	}

	return iCount;
}
// TAC - AI Improved Naval AI - koma13 - END

// TAC - AI Attack City - koma13, jdog5000(BBAI) - START
int CvPlayerAI::AI_cityTargetUnitsByPath(CvCity* pCity, CvSelectionGroup* pSkipSelectionGroup, int iMaxPathTurns) const
{
	PROFILE_FUNC();

	int iCount = 0;

	int iLoop;
	int iPathTurns;
	for(CvSelectionGroup* pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if (pLoopSelectionGroup != pSkipSelectionGroup && pLoopSelectionGroup->plot() != NULL && pLoopSelectionGroup->getNumUnits() > 0)
		{
			CvPlot* pMissionPlot = pLoopSelectionGroup->AI_getMissionAIPlot();

			if (pMissionPlot != NULL )
			{
				int iDistance = stepDistance(pCity->getX_INLINE(), pCity->getY_INLINE(), pMissionPlot->getX_INLINE(), pMissionPlot->getY_INLINE());

				if (iDistance <= 1)
				{
					if( pLoopSelectionGroup->generatePath(pLoopSelectionGroup->plot(), pMissionPlot, 0, true, &iPathTurns) )
					{
						if( !(pLoopSelectionGroup->canAllMove()) )
						{
							iPathTurns++;
						}

						if( iPathTurns <= iMaxPathTurns )
						{
							iCount += pLoopSelectionGroup->getNumUnits();
						}
					}
				}
			}
		}
	}

	return iCount;
}
// TAC - AI Attack City - koma13, jdog5000(BBAI) - END

// TAC - AI Attack City - koma13 - START
int CvPlayerAI::AI_unitTargetMissionAIs_(const CvUnit* pUnit, MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup) const
{
	return AI_unitTargetMissionAIs(pUnit, &eMissionAI, 1, pSkipSelectionGroup, -1, NO_UNITAI);
}

int CvPlayerAI::AI_unitTargetMissionAIs(const CvUnit* pUnit, MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup, UnitAITypes eUnitAI) const
{
	return AI_unitTargetMissionAIs(pUnit, &eMissionAI, 1, pSkipSelectionGroup, -1, eUnitAI);
}

int CvPlayerAI::AI_unitTargetMissionAIs(const CvUnit* pUnit, MissionAITypes* aeMissionAI, int iMissionAICount, CvSelectionGroup* pSkipSelectionGroup) const
{
	return AI_unitTargetMissionAIs(pUnit, aeMissionAI, iMissionAICount, pSkipSelectionGroup, -1, NO_UNITAI);
}

int CvPlayerAI::AI_unitTargetMissionAIs(const CvUnit* pUnit, MissionAITypes* aeMissionAI, int iMissionAICount, CvSelectionGroup* pSkipSelectionGroup, int iMaxPathTurns) const
{
	return AI_unitTargetMissionAIs(pUnit, aeMissionAI, iMissionAICount, pSkipSelectionGroup, iMaxPathTurns, NO_UNITAI);
}

int CvPlayerAI::AI_unitTargetMissionAIs(const CvUnit* pUnit, MissionAITypes* aeMissionAI, int iMissionAICount, CvSelectionGroup* pSkipSelectionGroup, int iMaxPathTurns, UnitAITypes eUnitAI) const
// TAC - AI Attack City - koma13 - END
{
	PROFILE_FUNC();

	CvSelectionGroup* pLoopSelectionGroup;
	int iCount;
	int iLoop;

	iCount = 0;

	for (pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if (pLoopSelectionGroup != pSkipSelectionGroup)
		{
			if (pLoopSelectionGroup->AI_getMissionAIUnit() == pUnit)
			{
				MissionAITypes eGroupMissionAI = pLoopSelectionGroup->AI_getMissionAIType();
				int iPathTurns = MAX_INT;

				if (iMaxPathTurns >= 0 && (pUnit->plot() != NULL) && (pLoopSelectionGroup->plot() != NULL))
				{
					pLoopSelectionGroup->generatePath(pUnit->plot(), pLoopSelectionGroup->plot(), 0, false, &iPathTurns);
					if (!(pLoopSelectionGroup->canAllMove()))
					{
						iPathTurns++;
					}
				}

				if ((iMaxPathTurns == -1) || (iPathTurns <= iMaxPathTurns))
				{
					for (int iMissionAIIndex = 0; iMissionAIIndex < iMissionAICount; iMissionAIIndex++)
					{
						if (eGroupMissionAI == aeMissionAI[iMissionAIIndex] || NO_MISSIONAI == aeMissionAI[iMissionAIIndex])
						{
							// TAC - AI Attack City - koma13 - START
							//iCount += pLoopSelectionGroup->getNumUnits();
							// R&R, ray, fix of Assert
							if (pLoopSelectionGroup->getNumUnits() > 0 && pLoopSelectionGroup->getHeadUnit() != NULL)
							{
								iCount += pLoopSelectionGroup->countNumUnitAIType(eUnitAI); //iCount += (pLoopSelectionGroup->getHeadUnit() != NULL) ? pLoopSelectionGroup->countNumUnitAIType(eUnitAI) : 0;
							}
							// TAC - AI Attack City - koma13 - END
						}
					}
				}
			}
		}
	}

	return iCount;
}
// TAC - AI Assault Sea - koma13, jdog5000(BBAI) - END

int CvPlayerAI::AI_enemyTargetMissionAIs(MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup)
{
	return AI_enemyTargetMissionAIs(&eMissionAI, 1, pSkipSelectionGroup);
}

int CvPlayerAI::AI_enemyTargetMissionAIs(MissionAITypes* aeMissionAI, int iMissionAICount, CvSelectionGroup* pSkipSelectionGroup)
{
	PROFILE_FUNC();

	CvSelectionGroup* pTransportSelectionGroup = NULL;
	if (pSkipSelectionGroup != NULL)
	{
		CvUnit* pHeadUnit = pSkipSelectionGroup->getHeadUnit();
		if (pHeadUnit->getTransportUnit() != NULL)
		{
			pTransportSelectionGroup = pHeadUnit->getTransportUnit()->getGroup();
		}
	}

	int iCount = 0;
	int iLoop;
	for(CvSelectionGroup* pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if ((pSkipSelectionGroup == NULL) || ((pLoopSelectionGroup != pSkipSelectionGroup) && (pLoopSelectionGroup != pTransportSelectionGroup)))
		{
			CvPlot* pMissionPlot = pLoopSelectionGroup->AI_getMissionAIPlot();

			if (NULL != pMissionPlot && pMissionPlot->isOwned())
			{
				MissionAITypes eGroupMissionAI = pLoopSelectionGroup->AI_getMissionAIType();
				for (int iMissionAIIndex = 0; iMissionAIIndex < iMissionAICount; iMissionAIIndex++)
				{
						if (eGroupMissionAI == aeMissionAI[iMissionAIIndex] || NO_MISSIONAI == aeMissionAI[iMissionAIIndex])
					{
						if (GET_TEAM(getTeam()).AI_isChosenWar(pMissionPlot->getTeam()))
						{
							iCount += pLoopSelectionGroup->getNumUnits();
							iCount += pLoopSelectionGroup->getCargo();
						}
					}
				}
			}
		}
	}

	return iCount;
}

int CvPlayerAI::AI_wakePlotTargetMissionAIs(CvPlot* pPlot, MissionAITypes eMissionAI, CvSelectionGroup* pSkipSelectionGroup)
{
	PROFILE_FUNC();

	FAssert(pPlot != NULL);

	CvSelectionGroup* pTransportSelectionGroup = NULL;
	if (pSkipSelectionGroup != NULL)
	{
		CvUnit* pHeadUnit = pSkipSelectionGroup->getHeadUnit();
		if (pHeadUnit->getTransportUnit() != NULL)
		{
			pTransportSelectionGroup = pHeadUnit->getTransportUnit()->getGroup();
		}
	}

	int iCount = 0;

	int iLoop;
	for(CvSelectionGroup* pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
	{
		if ((pSkipSelectionGroup == NULL) || ((pLoopSelectionGroup != pSkipSelectionGroup) && (pLoopSelectionGroup != pTransportSelectionGroup)))
		{
			MissionAITypes eGroupMissionAI = pLoopSelectionGroup->AI_getMissionAIType();
			if (eMissionAI == NO_MISSIONAI || eMissionAI == eGroupMissionAI)
			{
				CvPlot* pMissionPlot = pLoopSelectionGroup->AI_getMissionAIPlot();
				if (pMissionPlot != NULL && pMissionPlot == pPlot)
				{
					iCount += pLoopSelectionGroup->getNumUnits();
					pLoopSelectionGroup->setActivityType(ACTIVITY_AWAKE);
				}
			}
		}
	}

	return iCount;
}

CivicTypes CvPlayerAI::AI_bestCivic(CivicOptionTypes eCivicOption)
{
	CivicTypes eBestCivic;
	int iValue;
	int iBestValue;
	int iI;

	iBestValue = MIN_INT;
	eBestCivic = NO_CIVIC;

	for (iI = 0; iI < GC.getNumCivicInfos(); iI++)
	{
		if (GC.getCivicInfo((CivicTypes)iI).getCivicOptionType() == eCivicOption)
		{
			if (canDoCivics((CivicTypes)iI))
			{
				iValue = AI_civicValue((CivicTypes)iI);

				if (isCivic((CivicTypes)iI))
				{
					iValue *= 16;
					iValue /= 15;
				}

				if (iValue > iBestValue)
				{
					iBestValue = iValue;
					eBestCivic = ((CivicTypes)iI);
				}
			}
		}
	}

	return eBestCivic;
}


int CvPlayerAI::AI_civicValue(CivicTypes eCivic)
{
	PROFILE_FUNC();

	FAssertMsg(eCivic < GC.getNumCivicInfos(), "eCivic is expected to be within maximum bounds (invalid Index)");
	FAssertMsg(eCivic >= 0, "eCivic is expected to be non-negative (invalid Index)");

	CvCivicInfo& kCivic = GC.getCivicInfo(eCivic);

	int iValue = (getNumCities() * 6);

	iValue += (GC.getCivicInfo(eCivic).getAIWeight() * getNumCities());

	iValue *= 10 + GC.getGameINLINE().getSorenRandNum(90, "AI choose revolution civics");

	return iValue;
}

int CvPlayerAI::AI_getAttackOddsChange()
{
	return m_iAttackOddsChange;
}


void CvPlayerAI::AI_setAttackOddsChange(int iNewValue)
{
	m_iAttackOddsChange = iNewValue;
}

int CvPlayerAI::AI_getExtraGoldTarget() const
{
	return m_iExtraGoldTarget;
}

void CvPlayerAI::AI_setExtraGoldTarget(int iNewValue)
{
	m_iExtraGoldTarget = iNewValue;
}

void CvPlayerAI::AI_chooseCivic(CivicOptionTypes eCivicOption)
{
	int iBestValue = MIN_INT;
	CivicTypes eBestCivic = NO_CIVIC;

	for (int iCivic = 0; iCivic < GC.getNumCivicInfos(); ++iCivic)
	{
		if (GC.getCivicInfo((CivicTypes) iCivic).getCivicOptionType() == eCivicOption)
		{
			if (canDoCivics((CivicTypes) iCivic))
			{
				int iValue = AI_civicValue((CivicTypes) iCivic);
				if (iValue > iBestValue)
				{
					iBestValue = iValue;
					eBestCivic = (CivicTypes) iCivic;
				}
			}
		}
	}

	if (eBestCivic != NO_CIVIC)
	{
		setCivic(eCivicOption, eBestCivic);
	}
}

bool CvPlayerAI::AI_chooseGoody(GoodyTypes eGoody)
{
	return true;
}

CvCity* CvPlayerAI::AI_findBestCity() const
{
	CvCity* pBestCity = NULL;
	int iBestValue = 0;

	CvPlot* pTerritoryCenter = AI_getTerritoryCenter();

	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		int iValue = 1000 * (1 + pLoopCity->getPopulation());
		iValue *= 100 + 20 * (pLoopCity->plot()->getYield(YIELD_FOOD) - GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION);
		iValue /= std::max(1, pLoopCity->plot()->getDistanceToOcean());

		iValue *= 1 + pLoopCity->area()->getCitiesPerPlayer(getID());
		iValue /= 4 + stepDistance(pTerritoryCenter->getX_INLINE(), pTerritoryCenter->getY_INLINE(), pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE());

		if (iValue > iBestValue)
		{
			pBestCity = pLoopCity;
			iBestValue = iValue;
		}
	}

	FAssert(pBestCity != NULL);
	return pBestCity;
}

// TAC - AI Economy - koma13 - START
//CvCity* CvPlayerAI::AI_findBestPort() const
CvCity* CvPlayerAI::AI_findBestPort(CvArea* pArea, CvCity* pNoAccessCity) const
// TAC - AI Economy - koma13 - END
{
	if (getParent() == NO_PLAYER)
	{
		return NULL;
	}

	// TAC - AI Economy - koma13 - START
	if (pArea != NULL && pArea->getCitiesPerPlayer(getID()) <= 1)
	{
		pArea = NULL;
	}
	// TAC - AI Economy - koma13 - END

	CvCity* pBestCity = NULL;
	int iBestValue = 0;
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pArea == NULL || pArea == pLoopCity->area())	// TAC - AI Economy - koma13
		{
			if (pLoopCity->plot()->getNearestEurope() != NO_EUROPE)
			{
				// TAC - AI Economy - koma13 - START
				CvPlayerAI& kPlayer = GET_PLAYER(getID());

				if (pNoAccessCity == NULL || kPlayer.AI_isTradeRouteConnection(pNoAccessCity, pLoopCity))
				{
				// TAC - AI Economy - koma13 - END
					int iPortValue = 100000 / std::max(2, pLoopCity->plot()->getDistanceToOcean());
					iPortValue /= 100 + pLoopCity->getGameTurnFounded();

					// WTP: the port has to be reached by our transports; the more of them reach it (deep water), the
					// better (a colony on shallow coast or a large river is no port for galleons)
					int iTransports = 0;
					int iReaching = 0;
					int iUnitLoop;
					for (CvUnit* pLoopUnit = firstUnit(&iUnitLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iUnitLoop))
					{
						if (pLoopUnit->getDomainType() == DOMAIN_SEA && pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA)
						{
							++iTransports;
							if (AI_shipCanReachCity(pLoopUnit->getUnitType(), *pLoopCity))
							{
								++iReaching;
							}
						}
					}
					if (iTransports > 0)
					{
						if (iReaching == 0)
						{
							continue;
						}
						iPortValue = iPortValue * (1 + 4 * iReaching / iTransports) + 1;
					}

					if (iPortValue > iBestValue)
					{
						pBestCity = pLoopCity;
						iBestValue = iPortValue;
					}
				}
			}
		}
	}

	return pBestCity;
}

// TAC - AI Economy - koma13 - START
void CvPlayerAI::AI_updateBestPortCities()
{
	if (isHuman() || (getParent() == NO_PLAYER))
	{
		return;
	}

	if (getNumCities() == 0)
	{
		return;
	}

	CvCity* pLoopCity;
	int iLoop;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		pLoopCity->AI_setPort(false);
	}

	if (getNumCities() == 1) // || AI_getNumAIUnits(UNITAI_WAGON) == 0)
	{
		CvCity* pBestPort = AI_findBestPort();
		if (pBestPort != NULL)
		{
			pBestPort->AI_setPort(true);
			return;
		}
	}

	std::vector<CvCity*> pNoAccessToBestPort;

	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		CvArea* pLoopArea = pLoopCity->area();

		if (pLoopArea->getCitiesPerPlayer(getID()) > 1)
		{
			CvCity* pBestAreaPort = AI_findBestPort(pLoopArea);

			if (pBestAreaPort != NULL)
			{
				pBestAreaPort->AI_setPort(true);

				if (pBestAreaPort != pLoopCity)
				{
					CvPlayerAI& kPlayer = GET_PLAYER(getID());

					if (!kPlayer.AI_isTradeRouteConnection(pLoopCity, pBestAreaPort))
					{
						pNoAccessToBestPort.push_back(pLoopCity);
					}
				}
			}
		}
	}

	for (uint i = 0; i < pNoAccessToBestPort.size(); ++i)
	{
		CvCity* pBestAccessPort = AI_findBestPort(pNoAccessToBestPort[i]->area(), pNoAccessToBestPort[i]);
		if (pBestAccessPort != NULL)
		{
			pBestAccessPort->AI_setPort(true);
		}
	}
}
// TAC - AI Economy - koma13 - END
int CvPlayerAI::AI_getNumTrainAIUnits(UnitAITypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < NUM_UNITAI_TYPES, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_iNumTrainAIUnits.get(eIndex);
}


void CvPlayerAI::AI_changeNumTrainAIUnits(UnitAITypes eIndex, int iChange)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < NUM_UNITAI_TYPES, "eIndex is expected to be within maximum bounds (invalid Index)");
	m_em_iNumTrainAIUnits.add(eIndex, iChange);
	FAssert(AI_getNumTrainAIUnits(eIndex) >= 0);
}


int CvPlayerAI::AI_getNumAIUnits(UnitAITypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < NUM_UNITAI_TYPES, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_iNumAIUnits.get(eIndex);
}


void CvPlayerAI::AI_changeNumAIUnits(UnitAITypes eIndex, int iChange)
{
	if (eIndex != NO_UNITAI)
	{
		m_em_iNumAIUnits.add(eIndex, iChange);
		FAssert(AI_getNumAIUnits(eIndex) >= 0);

#ifdef _DEBUG
		if (iChange > 0)
		{
			int iLoop;
			int iNumUnitAI = 0;
			for (CvUnit* pLoopUnit = firstUnit(&iLoop); NULL != pLoopUnit; pLoopUnit = nextUnit(&iLoop))
			{
				if (pLoopUnit->AI_getUnitAIType() == eIndex)
				{
					++iNumUnitAI;
				}
			}
			for (uint i = 0; i < m_aEuropeUnits.size(); ++i)
			{
				if (m_aEuropeUnits[i]->AI_getUnitAIType() == eIndex)
				{
					++iNumUnitAI;
				}
			}
			for (uint i = 0; i < m_aAfricaUnits.size(); ++i)
			{
				if (m_aAfricaUnits[i]->AI_getUnitAIType() == eIndex)
				{
					++iNumUnitAI;
				}
			}
			for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
			{
				for (int i = 0; i < pLoopCity->getPopulation(); ++i)
				{
					CvUnit* pLoopUnit = pLoopCity->getPopulationUnitByIndex(i);
					if (pLoopUnit->AI_getUnitAIType() == eIndex)
					{
						++iNumUnitAI;
					}
				}
			}
			FAssert(AI_getNumAIUnits(eIndex) == iNumUnitAI);
		}
#endif
	}
}

int CvPlayerAI::AI_getNumRetiredAIUnits(UnitAITypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < NUM_UNITAI_TYPES, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_iNumRetiredAIUnits.get(eIndex);
}


void CvPlayerAI::AI_changeNumRetiredAIUnits(UnitAITypes eIndex, int iChange)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < NUM_UNITAI_TYPES, "eIndex is expected to be within maximum bounds (invalid Index)");
	m_em_iNumRetiredAIUnits.add(eIndex, iChange);
	FAssert(AI_getNumRetiredAIUnits(eIndex) >= 0);
}

int CvPlayerAI::AI_getPeacetimeTradeValue(PlayerTypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_iPeacetimeTradeValue.get(eIndex);
}


void CvPlayerAI::AI_changePeacetimeTradeValue(PlayerTypes eIndex, int iChange)
{
	PROFILE_FUNC();

	int iI;

	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");

	if (iChange != 0)
	{
		m_em_iPeacetimeTradeValue.add(eIndex, iChange);
		FAssert(AI_getPeacetimeTradeValue(eIndex) >= 0);

		FAssert(iChange > 0);

		if (iChange > 0)
		{
			if (GET_PLAYER(eIndex).getTeam() != getTeam())
			{
				for (iI = 0; iI < MAX_TEAMS; iI++)
				{
					if (GET_TEAM((TeamTypes)iI).isAlive())
					{
						if (GET_TEAM((TeamTypes)iI).AI_getWorstEnemy() == getTeam())
						{
							GET_TEAM((TeamTypes)iI).AI_changeEnemyPeacetimeTradeValue(GET_PLAYER(eIndex).getTeam(), iChange);
						}
					}
				}
			}
		}
	}
}


int CvPlayerAI::AI_getPeacetimeGrantValue(PlayerTypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_iPeacetimeGrantValue.get(eIndex);
}


void CvPlayerAI::AI_changePeacetimeGrantValue(PlayerTypes eIndex, int iChange)
{
	PROFILE_FUNC();

	int iI;

	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");

	if (iChange != 0)
	{
		m_em_iPeacetimeGrantValue.add(eIndex, iChange);
		FAssert(AI_getPeacetimeGrantValue(eIndex) >= 0);

		FAssert(iChange > 0);

		if (iChange > 0)
		{
			if (GET_PLAYER(eIndex).getTeam() != getTeam())
			{
				for (iI = 0; iI < MAX_TEAMS; iI++)
				{
					if (GET_TEAM((TeamTypes)iI).isAlive())
					{
						if (GET_TEAM((TeamTypes)iI).AI_getWorstEnemy() == getTeam())
						{
							GET_TEAM((TeamTypes)iI).AI_changeEnemyPeacetimeGrantValue(GET_PLAYER(eIndex).getTeam(), iChange);
						}
					}
				}
			}
		}
	}
}


int CvPlayerAI::AI_getGoldTradedTo(PlayerTypes eIndex) const
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_iGoldTradedTo.get(eIndex);
}


void CvPlayerAI::AI_changeGoldTradedTo(PlayerTypes eIndex, int iChange)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	m_em_iGoldTradedTo.add(eIndex, iChange);
	FAssert(AI_getGoldTradedTo(eIndex) >= 0);
}


int CvPlayerAI::AI_getAttitudeExtra(PlayerTypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_iAttitudeExtra.get(eIndex);
}


void CvPlayerAI::AI_setAttitudeExtra(PlayerTypes eIndex, int iNewValue)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	m_em_iAttitudeExtra.set(eIndex, iNewValue);
}


void CvPlayerAI::AI_changeAttitudeExtra(PlayerTypes eIndex, int iChange)
{
	AI_setAttitudeExtra(eIndex, (AI_getAttitudeExtra(eIndex) + iChange));
}


bool CvPlayerAI::AI_isFirstContact(PlayerTypes eIndex)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	return m_em_bFirstContact.get(eIndex);
}


void CvPlayerAI::AI_setFirstContact(PlayerTypes eIndex, bool bNewValue)
{
	FAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex < MAX_PLAYERS, "eIndex is expected to be within maximum bounds (invalid Index)");
	m_em_bFirstContact.set(eIndex, bNewValue);
}


int CvPlayerAI::AI_getContactTimer(PlayerTypes eIndex1, ContactTypes eIndex2)
{
	FAssertMsg(eIndex1 >= 0, "eIndex1 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex1 < MAX_PLAYERS, "eIndex1 is expected to be within maximum bounds (invalid Index)");
	FAssertMsg(eIndex2 >= 0, "eIndex2 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex2 < NUM_CONTACT_TYPES, "eIndex2 is expected to be within maximum bounds (invalid Index)");
	return m_em_iContactTimer[eIndex1].get(eIndex2);
}


void CvPlayerAI::AI_changeContactTimer(PlayerTypes eIndex1, ContactTypes eIndex2, int iChange)
{
	FAssertMsg(eIndex1 >= 0, "eIndex1 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex1 < MAX_PLAYERS, "eIndex1 is expected to be within maximum bounds (invalid Index)");
	FAssertMsg(eIndex2 >= 0, "eIndex2 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex2 < NUM_CONTACT_TYPES, "eIndex2 is expected to be within maximum bounds (invalid Index)");
	m_em_iContactTimer[eIndex1].add(eIndex2, iChange);
	FAssert(AI_getContactTimer(eIndex1, eIndex2) >= 0);
}


int CvPlayerAI::AI_getMemoryCount(PlayerTypes eIndex1, MemoryTypes eIndex2)
{
	FAssertMsg(eIndex1 >= 0, "eIndex1 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex1 < MAX_PLAYERS, "eIndex1 is expected to be within maximum bounds (invalid Index)");
	FAssertMsg(eIndex2 >= 0, "eIndex2 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex2 < NUM_MEMORY_TYPES, "eIndex2 is expected to be within maximum bounds (invalid Index)");
	return m_em_iMemoryCount[eIndex1].get(eIndex2);
}


void CvPlayerAI::AI_changeMemoryCount(PlayerTypes eIndex1, MemoryTypes eIndex2, int iChange)
{
	FAssertMsg(eIndex1 >= 0, "eIndex1 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex1 < MAX_PLAYERS, "eIndex1 is expected to be within maximum bounds (invalid Index)");
	FAssertMsg(eIndex2 >= 0, "eIndex2 is expected to be non-negative (invalid Index)");
	FAssertMsg(eIndex2 < NUM_MEMORY_TYPES, "eIndex2 is expected to be within maximum bounds (invalid Index)");
	m_em_iMemoryCount[eIndex1].add(eIndex2, iChange);
	FAssert(AI_getMemoryCount(eIndex1, eIndex2) >= 0);
}

// Protected Functions...

void CvPlayerAI::AI_doTradeRoutes()
{
	//Yields are divided into several classes:
	//1) Final Products - Port cities import these and export to europe. Other cities, export them.
	//2) Utility such as Lumber, Tools - These are set Import/Export with the Maintain Level used to indicate how much are needed.
	//3) Raw Materials - Cities which consume these to produce final products, Import/Export, with a high maintain level.
	//	 Port cities Import/Export with no maintain level.

	//Generally, utility yields are set to Import/Export

	//Best Yield Destinations
	std::vector<CvCity*> yield_dests(NUM_YIELD_TYPES, NULL);

	for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
	{
		YieldTypes eLoopYield = (YieldTypes)iYield;

		if (GC.getYieldInfo(eLoopYield).isCargo() && (eLoopYield != YIELD_FOOD))
		{
			if (AI_isYieldFinalProduct(eLoopYield))
			{
				yield_dests[eLoopYield] = NULL;
			}
			else if ((eLoopYield == YIELD_TOOLS) || (eLoopYield == YIELD_LUMBER) || (eLoopYield == YIELD_STONE) || (eLoopYield == YIELD_HARDWOOD)|| (eLoopYield == YIELD_CLAY))
			{
				yield_dests[eLoopYield] = NULL;
			}
			else if ((eLoopYield == YIELD_BLADES) || (eLoopYield == YIELD_MUSKETS) || (eLoopYield == YIELD_CANNONS) || (eLoopYield == YIELD_BLACK_POWDER) || (eLoopYield == YIELD_HORSES))
			{
				yield_dests[eLoopYield] = NULL;
			}
			else
			{
				CvCity* pBestYieldCity = NULL;
				int iBestCityValue = 0;

				int iLoop;
				CvCity* pLoopCity;
				for(pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
				{
					for (int iProfession = 0; iProfession < GC.getNumProfessionInfos(); ++iProfession)
					{
						CvProfessionInfo& kLoopProfession = GC.getProfessionInfo((ProfessionTypes)iProfession);
						if (kLoopProfession.getYieldsConsumed(0) == eLoopYield)
						{
							int iValue = pLoopCity->getProfessionOutput((ProfessionTypes)iProfession, NULL);
							if (iValue > 0)
							{
								iValue *= 100;
								iValue += pLoopCity->getPopulation();
								if (iValue > iBestCityValue)
								{
									iBestCityValue = iValue;
									pBestYieldCity = pLoopCity;
								}
							}
						}
					}
				}
				yield_dests[eLoopYield] = pBestYieldCity;
				if (GC.getGameINLINE().getGameTurn() > 50)
				{
					// R&R, ray, catch NULL-POINTER
					if (pBestYieldCity != NULL)
					{
						// R&R, ray, fix for new storage capacity
						int iNewMaintainLevelForNewCapacity = GC.getGameINLINE().getCargoYieldCapacity();
						pBestYieldCity->setMaintainLevel(eLoopYield, iNewMaintainLevelForNewCapacity);
					}
				}
			}
		}
	}

	CvCity* pLoopCity;
	int iLoop;
	//Setup export trade routes.
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		int aiYields[NUM_YIELD_TYPES];
		pLoopCity->calculateNetYields(aiYields);

		int iCapacity = pLoopCity->getMaxYieldCapacity();
		for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
		{
			YieldTypes eLoopYield = (YieldTypes)iYield;
			bool bAvailable = (aiYields[eLoopYield] > 0) || (pLoopCity->getYieldStored(eLoopYield) > 0);
			bool bShouldImport = false;
			bool bShouldExport = false;
			if (GC.getYieldInfo(eLoopYield).isCargo())
			{
				if (eLoopYield == YIELD_FOOD)
				{
					int iThreshold = pLoopCity->growthThreshold() / (2 + std::max(0, aiYields[YIELD_FOOD]));

					pLoopCity->setMaintainLevel(YIELD_FOOD, iThreshold);

					if (aiYields[YIELD_FOOD] > 0)
					{
						bShouldExport = true;
						pLoopCity->AI_setAvoidGrowth(false);
					}
					else
					{
						bShouldImport = true;
						pLoopCity->AI_setAvoidGrowth(true);
					}
				}
				// TAC - AI Economy - koma13 - START
				//else
				else if (eLoopYield != YIELD_LUMBER && eLoopYield != YIELD_STONE && eLoopYield != YIELD_HARDWOOD && eLoopYield != YIELD_CLAY)
				// TAC - AI Economy - koma13 - END
				{
					if ((AI_isYieldFinalProduct(eLoopYield)) || AI_isYieldForSale(eLoopYield))
					{
						if (pLoopCity->AI_isPort())
						{
							bShouldImport = true;
						}
						else if (bAvailable)
						{
							bShouldExport = true;
						}
					}
					if (yield_dests[eLoopYield] == pLoopCity)
					{
						bShouldImport = true;
					}
					else if (yield_dests[eLoopYield] != NULL)
					{
						// TAC - AI Economy - koma13 - START
						for (int iProfession = 0; iProfession < GC.getNumProfessionInfos(); ++iProfession)
						{
							CvProfessionInfo& kLoopProfession = GC.getProfessionInfo((ProfessionTypes)iProfession);
							// R&R, ray , MYCP partially based on code of Aymerick - START
							bool bYieldSearched = false;
							if (kLoopProfession.getNumYieldsConsumed() == 1)
							{
								if (kLoopProfession.getYieldsConsumed(0) == eLoopYield)
								{
									bYieldSearched = true;
								}
							}
							else if (kLoopProfession.getNumYieldsConsumed() == 2)
							{
								if (kLoopProfession.getYieldsConsumed(0) == eLoopYield || kLoopProfession.getYieldsConsumed(1) == eLoopYield)
								{
									bYieldSearched = true;
								}
							}
							if (bYieldSearched)
							// R&R, ray , MYCP partially based on code of Aymerick - END
							{
								if (pLoopCity->professionCount((ProfessionTypes)iProfession) > 0)
								{
									bShouldImport = true;
									// R&R, ray, fix for new storage capacity
									int iNewMaintainLevelForNewCapacity = GC.getGameINLINE().getCargoYieldCapacity();
									pLoopCity->setMaintainLevel(eLoopYield, iNewMaintainLevelForNewCapacity);
								}
							}
						}
						//if (bAvailable)
						if (bAvailable && !bShouldImport)
						// TAC - AI Economy - koma13 - END
						{
							bShouldExport = true;
						}
					}
					else
					{
						int iMaintainLevel = pLoopCity->getMaintainLevel(eLoopYield);
						int iStored = pLoopCity->getYieldStored(eLoopYield);

						int iCargoYields = 0;

						for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_YIELD_TYPES; ++eYield)
						{
							if (GC.getYieldInfo(eYield).isIgnoredForStorageCapacity())
									continue;

							if ((pLoopCity->getYieldStored(eYield) > 0) && (GC.getYieldInfo(eYield).isCargo()))
							{
								iCargoYields++;
							}
						}
						if (iCargoYields < 1)
						{
							iCargoYields = 1;
						}

						iMaintainLevel /= iCargoYields;
						if (iStored > iMaintainLevel)
						{
							if ((aiYields[eLoopYield] > 0) || (pLoopCity->getTotalYieldStored() > (iCapacity * 90) / 100) || pLoopCity->isBestPortCity())
							{
								bShouldExport = true;
							}
						}
						else if (iMaintainLevel > 0 && iStored < iMaintainLevel)
						{
							bShouldImport = true;
						}
					}
				}
				if (bShouldImport)
				{
					pLoopCity->addImport(eLoopYield);
				}
				else
				{
					pLoopCity->removeImport(eLoopYield);
				}
				if (bShouldExport)
				{
					pLoopCity->addExport(eLoopYield);
				}
				else
				{
					pLoopCity->removeExport(eLoopYield);
				}
			}
		}
	}
}

void CvPlayerAI::AI_doCounter()
{
	const CvLeaderHeadInfo& kLeader = GC.getLeaderHeadInfo(getPersonalityType());

	// Decay contacts & memories
	for (PlayerTypes eP = FIRST_PLAYER; eP < NUM_PLAYER_TYPES; ++eP)
	{
		if (GET_PLAYER(eP).isAlive())
		{
			for (ContactTypes eC = FIRST_CONTACT; eC < NUM_CONTACT_TYPES; ++eC)
			{
				if (AI_getContactTimer(eP, eC) > 0)
					AI_changeContactTimer(eP, eC, -1);
			}
			for (MemoryTypes eM = FIRST_MEMORY; eM < NUM_MEMORY_TYPES; ++eM)
				{
				int iCnt = AI_getMemoryCount(eP, eM);
				if (iCnt > 0)
					{
					int iRand = kLeader.getMemoryDecayRand(eM);
					if (iRand > 0 &&
						GC.getGameINLINE().getSorenRandNum(iRand, "Memory Decay") == 0)
						{
						AI_changeMemoryCount(eP, eM, -1);
					}
				}
			}
		}
	}

	// Decay naval danger maps
	for (int i = 0; i < GC.getMap().numPlotsINLINE(); ++i)
	{
		CvPlot* p = GC.getMap().plotByIndexINLINE(i);
		int d = p->getDangerMap(getID());
		if (d > 0)
			p->setDangerMap(getID(), d - 1);
	}

	// Revolution timer: increment only while waves remain
	const HandicapTypes eHandicap = GC.getGameINLINE().getHandicapType();
	const int iNumWaves = GC.getHandicapInfo(eHandicap).getNumWaves();
	const int iWaveIndex = AI_getWaveIndex();
	if (iWaveIndex < iNumWaves)
	{
		int iLast = AI_getLastWave();
		if (iLast > -1)
		{
			int iNext = std::min(iLast + 1, GC.getHandicapInfo(eHandicap).getWaveTurns());
			AI_setLastWave(iNext);
	}
	}
}

void CvPlayerAI::AI_doMilitary()
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_doMilitary (native)" : "CvPlayerAI::AI_doMilitary");
#endif


	AI_setAttackOddsChange(GC.getLeaderHeadInfo(getPersonalityType()).getBaseAttackOddsChange() +
		GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getAttackOddsChangeRand(), "AI Attack Odds Change #1") +
		GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getAttackOddsChangeRand(), "AI Attack Odds Change #2"));
}

void CvPlayerAI::AI_doDiplo()
{
#ifdef PROFILING_ENABLED
	Profiler profileSession(isNative() ? "CvPlayerAI::AI_doDiplo (native)" : "CvPlayerAI::AI_doDiplo");
#endif
	PROFILE_FUNC();

	FAssert(!isHuman());

	if (GC.getUSE_AI_DO_DIPLO_CALLBACK()) // K-Mod. block unused python callbacks
	{
		// allow python to handle it
		CyArgsList argsList;
		argsList.add(getID());
		long lResult = 0;
		gDLL->getPythonIFace()->callFunction(PYGameModule, "AI_doDiplo", argsList.makeFunctionArgs(), &lResult);
		if (lResult == 1)
		{
			return;
		}
	}

	// R&R, ray, the Church - START
	if (GC.getGameINLINE().isChurchPlayer(getID()))
	{
		return; // we do not want Church to initiate Trade, because currently that is useless
	}
	// R&R, ray, the Church - END

	std::vector<bool> abContacted(MAX_TEAMS, false);

	for (int iPass = 0; iPass < 2; iPass++)
	{
		for (int iI = 0; iI < MAX_PLAYERS; iI++)
		{
			PlayerTypes ePlayer = (PlayerTypes) iI;
			CvPlayer& kPlayer = GET_PLAYER(ePlayer);
			if (kPlayer.isAlive() && ePlayer != getID())
			{
				if (iPass == 0 && !isNative() && kPlayer.isNative())
				{
					AI_doNativeLandReturn(ePlayer);
				}

				//WTP, ray, fixing precalcuated Diplo Event Issue - START
				// the King triggers it only for its own "Colonies"
				// we only need to do this once for iPass 1
				if (kPlayer.getParent() == getID() && (iPass == 1))
				{
					//these methods catch all other errors themselves
					kPlayer.checkForNativeMercs(); //TAC Native Mercs
					kPlayer.checkForNativeSlaves(); // R&R, ray, Native Slave, START
					kPlayer.checkForAfricanSlaves(); // R&R, ray, African Slaves, START
					kPlayer.checkForPrisonsCrowded(); // R&R, ray, Prisons Crowded - START
					kPlayer.checkForRevolutionaryNoble(); // R&R, ray, Revolutionary Noble - START
					kPlayer.checkForBishop(); // R&R, ray, Bishop - START
					kPlayer.checkForChurchContact(); // R&R, ray, the Church - START
					kPlayer.checkForChurchWar(); // R&R, ray, Church War
					kPlayer.checkForColonialInterventionInNativeWar(); //WTP, ray, Colonial Intervention In Native War - START
					kPlayer.checkForColonialAndNativeAlliesWar(); // WTP, ray, Big Colonies and Native Allies War - START
					kPlayer.checkForSmugglers(); // R&R, ray, Smuggling - START
					kPlayer.checkForRangers(); // R&R, ray, Rangers - START
					kPlayer.checkForConquistadors(); // R&R, ray, Conquistadors - START
					kPlayer.checkForPirates(); // R&R, ray, Pirates - START
					kPlayer.checkForEuropeanPeace(); // R&R, ray, European Peace, START
					kPlayer.checkForRoyalIntervention(); // WTP, ray, Royal Intervention, START
					kPlayer.checkForPrivateersAccusation(); // WTP, ray, Privateers DLL Diplo Event - START
					kPlayer.checkForEuropeanWars(); //TAC European Wars
					kPlayer.checkForStealingImmigrant(); // R&R, Stealing Immigrant
					kPlayer.checkForContinentalGuard(); // R&R, ray, Continental Guard - START
					kPlayer.checkForMortar(); // R&R, ray, Mortar - START
					kPlayer.checkForMilitiaOrUnrest(); // R&R, ray, Militia or Unrest - START
				}
				//WTP, ray, fixing precalcuated Diplo Event Issue - END

				if (kPlayer.isHuman() == (iPass == 1))
				{
					if (AI_doDiploCancelDeals((PlayerTypes) iI))
					{
						if (kPlayer.isHuman())
						{
							abContacted[kPlayer.getTeam()] = true;
						}
					}

					if (canContact(ePlayer) && AI_isWillingToTalk(ePlayer))
					{
						if (kPlayer.getTeam() != getTeam() && !(GET_TEAM(getTeam()).isHuman()) && (kPlayer.isHuman() || !(GET_TEAM(kPlayer.getTeam()).isHuman())))
						{
							FAssertMsg(iI != getID(), "iI is not expected to be equal with getID()");

							if (!(GET_TEAM(getTeam()).isAtWar(kPlayer.getTeam())))
							{
								if (AI_doDiploOfferCity(ePlayer))
								{
									if (kPlayer.isHuman())
									{
										abContacted[kPlayer.getTeam()] = true;
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploOfferAlliance(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
										else
										{
											// move on to the next player since we are on the same team now
											break;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploAskJoinWar(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploAskStopTrading(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploGiveHelp(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploAskForHelp(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploDemandTribute(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploKissPinky(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploOpenBorders(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploDefensivePact(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploTradeMap(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}

								if (!kPlayer.isHuman() || !abContacted[kPlayer.getTeam()])
								{
									if (AI_doDiploDeclareWar(ePlayer))
									{
										if (kPlayer.isHuman())
										{
											abContacted[kPlayer.getTeam()] = true;
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
}


bool CvPlayerAI::AI_doDiploCancelDeals(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (kPlayer.getTeam() == getTeam())
	{
		return false;
	}

	bool bKilled = false;

	int iLoop;
	for (CvDeal* pLoopDeal = GC.getGameINLINE().firstDeal(&iLoop); pLoopDeal != NULL; pLoopDeal = GC.getGameINLINE().nextDeal(&iLoop))
	{
		if (pLoopDeal->isCancelable(getID()))
		{
			if ((GC.getGameINLINE().getGameTurn() - pLoopDeal->getInitialGameTurn()) >= (GC.getDefineINT("PEACE_TREATY_LENGTH") * 2))
			{
				bool bCancelDeal = false;

				if ((pLoopDeal->getFirstPlayer() == getID()) && (pLoopDeal->getSecondPlayer() == ePlayer))
				{
					if (kPlayer.isHuman())
					{
						if (!AI_considerOffer(ePlayer, pLoopDeal->getSecondTrades(), pLoopDeal->getFirstTrades(), -1))
						{
							bCancelDeal = true;
						}
					}
					else
					{
						for (CLLNode<TradeData>* pNode = pLoopDeal->getFirstTrades()->head(); pNode; pNode = pLoopDeal->getFirstTrades()->next(pNode))
						{
							if (getTradeDenial(ePlayer, pNode->m_data) != NO_DENIAL)
							{
								bCancelDeal = true;
								break;
							}
						}
					}
				}
				else if ((pLoopDeal->getFirstPlayer() == ePlayer) && (pLoopDeal->getSecondPlayer() == getID()))
				{
					if (kPlayer.isHuman())
					{
						if (!AI_considerOffer(ePlayer, pLoopDeal->getFirstTrades(), pLoopDeal->getSecondTrades(), -1))
						{
							bCancelDeal = true;
						}
					}
					else
					{
						for (CLLNode<TradeData>* pNode = pLoopDeal->getSecondTrades()->head(); pNode; pNode = pLoopDeal->getSecondTrades()->next(pNode))
						{
							if (getTradeDenial(ePlayer, pNode->m_data) != NO_DENIAL)
							{
								bCancelDeal = true;
								break;
							}
						}
					}
				}

				if (bCancelDeal)
				{
					if (canContact(ePlayer) && AI_isWillingToTalk(ePlayer))
					{
						if (kPlayer.isHuman())
						{
							CLinkList<TradeData> ourList;
							CLinkList<TradeData> theirList;

							for (CLLNode<TradeData>* pNode = pLoopDeal->headFirstTradesNode(); (pNode != NULL); pNode = pLoopDeal->nextFirstTradesNode(pNode))
							{
								if (pLoopDeal->getFirstPlayer() == getID())
								{
									ourList.insertAtEnd(pNode->m_data);
								}
								else
								{
									theirList.insertAtEnd(pNode->m_data);
								}
							}

							for (CLLNode<TradeData>* pNode = pLoopDeal->headSecondTradesNode(); (pNode != NULL); pNode = pLoopDeal->nextSecondTradesNode(pNode))
							{
								if (pLoopDeal->getSecondPlayer() == getID())
								{
									ourList.insertAtEnd(pNode->m_data);
								}
								else
								{
									theirList.insertAtEnd(pNode->m_data);
								}
							}

							CvDiploParameters* pDiplo = new CvDiploParameters(getID());
							pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_CANCEL_DEAL"));
							pDiplo->setAIContact(true);
							pDiplo->setOurOfferList(theirList);
							pDiplo->setTheirOfferList(ourList);
							gDLL->beginDiplomacy(pDiplo, ePlayer);
						}
					}

					pLoopDeal->kill(true, getTeam()); // XXX test this for AI...
					bKilled = true;
				}
			}
		}
	}

	return bKilled;
}


// WTP: a native player can still claim this plot if one of its villages is within 3 plots
static bool nativeCanClaimPlot(const CvPlot& kPlot, PlayerTypes eNative)
{
	int iLoop;
	CvPlayer& kNative = GET_PLAYER(eNative);
	for (CvCity* pVillage = kNative.firstCity(&iLoop); pVillage != NULL; pVillage = kNative.nextCity(&iLoop))
	{
		if (plotDistance(pVillage->getX_INLINE(), pVillage->getY_INLINE(), kPlot.getX_INLINE(), kPlot.getY_INLINE()) <= 3)
		{
			return true;
		}
	}
	return false;
}

// WTP: take back up to 3 plots per turn whose influence was given up, if no native village can claim them
// anymore or we are stronger than every native player with a claim on them
void CvPlayerAI::AI_doReclaimTerritory()
{
	MOD_PROFILE("CvPlayerAI::AI_doReclaimTerritory");
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || getNumCities() == 0)
	{
		return;
	}
	int iReclaimed = 0;
	for (int iI = 0; iI < GC.getMap().numPlotsINLINE() && iReclaimed < 3; ++iI)
	{
		CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
		if (!pLoopPlot->isNotCulture(getID()))
		{
			continue;
		}
		bool bReclaim = true;
		for (int iPlayer = 0; iPlayer < MAX_PLAYERS && bReclaim; ++iPlayer)
		{
			const PlayerTypes eNative = (PlayerTypes)iPlayer;
			const CvPlayer& kNative = GET_PLAYER(eNative);
			if (kNative.isAlive() && kNative.isNative() && nativeCanClaimPlot(*pLoopPlot, eNative) && getPower() - getNavalPower() <= kNative.getPower())
			{
				bReclaim = false;
			}
		}
		if (bReclaim)
		{
			pLoopPlot->setNotCulture(getID(), false);
			++iReclaimed;
			if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_STATUS, getID(), "WTP decision: Player %d (%S) turn %d reclaims plot %d,%d", getID(), getCivilizationDescription(0),
				GC.getGameINLINE().getGameTurn(), pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
		}
	}
}

void CvPlayerAI::AI_doNativeLandReturn(PlayerTypes eNativePlayer)
{
	CvPlayerAI& kNativePlayer = GET_PLAYER(eNativePlayer);

	if (!kNativePlayer.isNative())
	{
		return;
	}

	// WTP: no land is given back to natives we are stronger than (it would be reclaimed right away)
	if (getPower() - getNavalPower() > kNativePlayer.getPower())
	{
		return;
	}

	if (GET_TEAM(getTeam()).isAtWar(kNativePlayer.getTeam()))
	{
		return;
	}

	if (kNativePlayer.AI_getAttitude(getID()) != ATTITUDE_FURIOUS)
	{
		return;
	}

	int iPlotsReturned = 0;

	for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); ++iI)
	{
		CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);

		if (pLoopPlot->getOwnerINLINE() != getID())
		{
			continue;
		}

		if (pLoopPlot->isNotCulture(getID()))
		{
			continue;
		}

		if (pLoopPlot->getCulture(eNativePlayer) <= pLoopPlot->getCulture(getID()))
		{
			continue;
		}

		if (pLoopPlot->getBonusType() != NO_BONUS)
		{
			continue;
		}

		if (pLoopPlot->getImprovementType() != NO_IMPROVEMENT)
		{
			continue;
		}

		if (!pLoopPlot->canSetNotCulture(getID()))
		{
			continue;
		}

		pLoopPlot->setNotCulture(getID(), true);

		++iPlotsReturned;

		if (iPlotsReturned >= 3)
		{
			break;
		}
	}
}


bool CvPlayerAI::AI_doDiploOfferCity(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (AI_getAttitude(ePlayer) < ATTITUDE_CAUTIOUS)
	{
		return false;
	}

	bool bOffered = false;
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity->getPreviousOwner() != ePlayer)
		{
			if (((pLoopCity->getGameTurnAcquired() + 4) % 20) == (GC.getGameINLINE().getGameTurn() % 20))
			{
				int iCount = 0;
				int iPossibleCount = 0;

				for (int iJ = 0; iJ < NUM_CITY_PLOTS; iJ++)
				{
					CvPlot* pLoopPlot = plotCity(pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE(), iJ);

					if (pLoopPlot != NULL)
					{
						if (pLoopPlot->getOwnerINLINE() == ePlayer)
						{
							++iCount;
						}

						++iPossibleCount;
					}
				}

				if (iCount >= (iPossibleCount / 2))
				{
					TradeData item;
					setTradeItem(&item, TRADE_CITIES, pLoopCity->getID(), NULL);

					if (canTradeItem((ePlayer), item, true))
					{
						CLinkList<TradeData> ourList;
						ourList.insertAtEnd(item);

						if (kPlayer.isHuman())
						{
							CvDiploParameters* pDiplo = new CvDiploParameters(getID());
							pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_OFFER_CITY"));
							pDiplo->setAIContact(true);
							pDiplo->setTheirOfferList(ourList);
							gDLL->beginDiplomacy(pDiplo, ePlayer);
						}
						else
						{
							GC.getGameINLINE().implementDeal(getID(), (ePlayer), &ourList, NULL);
						}
						bOffered = true;
					}
				}
			}
		}
	}

	return bOffered;
}


bool CvPlayerAI::AI_doDiploOfferAlliance(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (kPlayer.getParent() == getID())
	{
		return false;
	}

	if (AI_getContactTimer((ePlayer), CONTACT_PERMANENT_ALLIANCE) > 0)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_PERMANENT_ALLIANCE), "AI Diplo Alliance") != 0)
	{
		return false;
	}

	bool bOffered = false;
	TradeData item;
	setTradeItem(&item, TRADE_PERMANENT_ALLIANCE, 0, NULL);

	if (canTradeItem((ePlayer), item, true) && kPlayer.canTradeItem(getID(), item, true))
	{
		CLinkList<TradeData> ourList;
		CLinkList<TradeData> theirList;
		ourList.insertAtEnd(item);
		theirList.insertAtEnd(item);

		bOffered = true;

		if (kPlayer.isHuman())
		{
			AI_changeContactTimer(ePlayer, CONTACT_PERMANENT_ALLIANCE, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_PERMANENT_ALLIANCE));
			CvDiploParameters* pDiplo = new CvDiploParameters(getID());
			FAssertMsg(pDiplo != NULL, "pDiplo must be valid");
			pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_OFFER_DEAL"));
			pDiplo->setAIContact(true);
			pDiplo->setOurOfferList(theirList);
			pDiplo->setTheirOfferList(ourList);
			gDLL->beginDiplomacy(pDiplo, ePlayer);
		}
		else
		{
			GC.getGameINLINE().implementDeal(getID(), (ePlayer), &ourList, &theirList);
		}
	}

	return bOffered;
}


bool CvPlayerAI::AI_doDiploAskJoinWar(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (!kPlayer.isHuman())
	{
		return false;
	}

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if ((AI_getMemoryCount(ePlayer, MEMORY_DECLARED_WAR) > 0) || (AI_getMemoryCount(ePlayer, MEMORY_HIRED_WAR_ALLY) > 0))
	{
		return false;
	}

	if (AI_getContactTimer(ePlayer, CONTACT_JOIN_WAR) > 0)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_JOIN_WAR), "AI Diplo Join War") != 0)
	{
		return false;
	}

	int iBestValue = 0;
	TeamTypes eBestTeam = NO_TEAM;
	for (int iJ = 0; iJ < MAX_TEAMS; ++iJ)
	{
		TeamTypes eLoopTeam = (TeamTypes) iJ;
		CvTeam& kLoopTeam = GET_TEAM(eLoopTeam);
		if (kLoopTeam.isAlive())
		{
			if (atWar(eLoopTeam, getTeam()) && !atWar(eLoopTeam, kPlayer.getTeam()))
			{
				if (GET_TEAM(kPlayer.getTeam()).isHasMet(eLoopTeam))
				{
					if (GET_TEAM(kPlayer.getTeam()).canDeclareWar(eLoopTeam))
					{
						int iValue = (1 + GC.getGameINLINE().getSorenRandNum(10000, "AI Joining War"));

						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							eBestTeam = eLoopTeam;
						}
					}
				}
			}
		}
	}

	if (eBestTeam == NO_TEAM)
	{
		return false;
	}

	AI_changeContactTimer(ePlayer, CONTACT_JOIN_WAR, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_JOIN_WAR));
	CvDiploParameters* pDiplo = new CvDiploParameters(getID());
	pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_JOIN_WAR"));
	pDiplo->addDiploCommentVariable(GET_PLAYER(GET_TEAM(eBestTeam).getLeaderID()).getCivilizationAdjectiveKey());
	pDiplo->setAIContact(true);
	pDiplo->setData(eBestTeam);
	gDLL->beginDiplomacy(pDiplo, ePlayer);

	return true;
}

bool CvPlayerAI::AI_doDiploAskStopTrading(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (!kPlayer.isHuman())
	{
		return false;
	}

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (AI_getContactTimer((ePlayer), CONTACT_STOP_TRADING) > 0)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_STOP_TRADING), "AI Diplo Stop Trading") != 0)
	{
		return false;
	}

	TeamTypes eBestTeam = GET_TEAM(getTeam()).AI_getWorstEnemy();
	if (eBestTeam == NO_TEAM)
	{
		return false;
	}

	if (!GET_TEAM(kPlayer.getTeam()).isHasMet(eBestTeam))
	{
		return false;
	}

	if (GET_TEAM(getTeam()).isParentOf(eBestTeam) && !::atWar(getTeam(), eBestTeam))
	{
		return false;
	}

	if (!kPlayer.canStopTradingWithTeam(eBestTeam))
	{
		return false;
	}

	FAssert(!atWar(kPlayer.getTeam(), eBestTeam));
	FAssert(kPlayer.getTeam() != eBestTeam);

	AI_changeContactTimer(ePlayer, CONTACT_STOP_TRADING, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_STOP_TRADING));
	CvDiploParameters* pDiplo = new CvDiploParameters(getID());
	pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_STOP_TRADING"));
	pDiplo->addDiploCommentVariable(GET_PLAYER(GET_TEAM(eBestTeam).getLeaderID()).getCivilizationAdjectiveKey());
	pDiplo->setAIContact(true);
	pDiplo->setData(eBestTeam);
	gDLL->beginDiplomacy(pDiplo, ePlayer);

	return true;
}

bool CvPlayerAI::AI_doDiploGiveHelp(PlayerTypes ePlayer)
									{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (!kPlayer.isHuman())
	{
		return false;
	}

	if (isNative())
	{
		return false;
	}

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (AI_getAttitude(ePlayer) <= GC.getLeaderHeadInfo(kPlayer.getPersonalityType()).getNoGiveHelpAttitudeThreshold())
	{
		return false;
	}

	if (AI_getContactTimer((ePlayer), CONTACT_GIVE_HELP) > 0)
	{
		return false;
	}

	if (GET_TEAM(kPlayer.getTeam()).getAssets() > GET_TEAM(getTeam()).getAssets() / 2)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_GIVE_HELP), "AI Diplo Give Help") != 0)
	{
		return false;
	}

	int iGold = AI_maxGoldTrade(ePlayer);
	if (iGold <= 0)
	{
		return false;
	}

	TradeData item;
	setTradeItem(&item, TRADE_GOLD, iGold, NULL);
	if (!canTradeItem(ePlayer, item, true))
	{
		return false;
	}

	CLinkList<TradeData> ourList;
	ourList.insertAtEnd(item);

	AI_changeContactTimer((ePlayer), CONTACT_GIVE_HELP, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_GIVE_HELP));
	CvDiploParameters* pDiplo = new CvDiploParameters(getID());
	pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_GIVE_HELP"));
	pDiplo->setTheirOfferList(ourList);
	pDiplo->setAIContact(true);
	gDLL->beginDiplomacy(pDiplo, ePlayer);

	return true;
}


bool CvPlayerAI::AI_doDiploAskForHelp(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (!kPlayer.isHuman())
	{
		return false;
	}

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (AI_getContactTimer((ePlayer), CONTACT_ASK_FOR_HELP) > 0)
	{
		return false;
	}

	if (GET_TEAM(kPlayer.getTeam()).getAssets() <= GET_TEAM(getTeam()).getAssets() / 2)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_ASK_FOR_HELP), "AI Diplo Ask for Help") != 0)
	{
		return false;
	}

	int iGold = kPlayer.AI_maxGoldTrade(getID()) * GC.getGameINLINE().getSorenRandNum(100, "Ask for gold percent") / 100;
	if (iGold <= 0)
	{
		return false;
	}

	TradeData item;
	setTradeItem(&item, TRADE_GOLD, iGold, NULL);
	if (!canTradeItem(ePlayer, item, true))
	{
		return false;
	}

	CLinkList<TradeData> theirList;
	theirList.insertAtEnd(item);

	AI_changeContactTimer((ePlayer), CONTACT_GIVE_HELP, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_GIVE_HELP));
	CvDiploParameters* pDiplo = new CvDiploParameters(getID());
	pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_ASK_FOR_HELP"));
	pDiplo->setOurOfferList(theirList);
	pDiplo->setAIContact(true);
	gDLL->beginDiplomacy(pDiplo, ePlayer);

	return true;
}


bool CvPlayerAI::AI_doDiploDemandTribute(PlayerTypes ePlayer)
{
	CvPlayerAI& kPlayer = GET_PLAYER(ePlayer);

	if (!kPlayer.isHuman())
	{
		return false;
	}

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (!GET_TEAM(getTeam()).canDeclareWar(kPlayer.getTeam()))
	{
		return false;
	}

	if (GET_TEAM(getTeam()).AI_isSneakAttackPreparing(kPlayer.getTeam()))
	{
		return false;
	}

	if (GET_TEAM(kPlayer.getTeam()).getDefensivePower() >= GET_TEAM(getTeam()).getPower())
	{
		return false;
	}

	if (AI_getAttitude(ePlayer) > GC.getLeaderHeadInfo(kPlayer.getPersonalityType()).getDemandTributeAttitudeThreshold())
	{
		return false;
	}

	if (AI_getContactTimer((ePlayer), CONTACT_DEMAND_TRIBUTE) > 0)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_DEMAND_TRIBUTE), "AI Diplo Demand Tribute") != 0)
	{
		return false;
	}

	TradeData item;
	int iReceiveGold = std::min(std::max(0, (kPlayer.getGold() - 50)), kPlayer.AI_goldTarget());
	iReceiveGold -= (iReceiveGold % GC.getDefineINT("DIPLOMACY_VALUE_REMAINDER"));
	if (iReceiveGold > 50)
	{
		setTradeItem(&item, TRADE_GOLD, iReceiveGold, NULL);
	}
	else if (GET_TEAM(getTeam()).AI_mapTradeVal(kPlayer.getTeam()) > 100)
	{
		setTradeItem(&item, TRADE_MAPS, 0, NULL);
	}

	if (!canTradeItem(ePlayer, item, true))
	{
		return false;
	}

	CLinkList<TradeData> theirList;
	theirList.insertAtEnd(item);

	AI_changeContactTimer((ePlayer), CONTACT_DEMAND_TRIBUTE, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_DEMAND_TRIBUTE));
	CvDiploParameters* pDiplo = new CvDiploParameters(getID());
	pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_DEMAND_TRIBUTE"));
	pDiplo->setAIContact(true);
	pDiplo->setOurOfferList(theirList);
	gDLL->beginDiplomacy(pDiplo, ePlayer);

	return true;
}

//WTP, ray, factor Population into this
//WTP, ray, method completely rewritten
int CvPlayerAI::NBMOD_GetGoldAsk(PlayerTypes ePlayer) const
{
	// get current gold of player, needed for safety check
	int iMaxGoldPlayerHas = GET_PLAYER(ePlayer).getGold();

	// first get the population and Attitude
	int iPopulation = GET_PLAYER(ePlayer).getTotalPopulation();
	int kingAttitude = AI_getAttitude(ePlayer);

	// now we factor in GameSpeed, HandyCap and XML balancing values
	int iGameSpeedMod = GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getTrainPercent(); // must later be divieded by 100
	int iHandiCapMod = GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).NBMOD_GetMaxGoldAskPercent();
	int iMinKissPinkyPopulationGold = iPopulation * GC.getDefineINT("MIN_KISS_PINKY_GOLD_PER_POPULATION");
	int iMaxKissPinkyPopulationGold = iPopulation * GC.getDefineINT("MAX_KISS_PINKY_GOLD_PER_POPULATION");
	int iKissPinkyAttitudeBonus = kingAttitude * GC.getDefineINT("KISS_PINKY_ATTITUDE_MODIFIER"); // can be positive or negative

	// we need to ensure the absolute Min Pop Equivalent for really high Populations
	// otherwise the player will never ever save enough gold and never trigger reqeuests for iMin
	int iPinkyPopGoldEqivalent = GC.getDefineINT("PINKY_GOLD_POP_EQUIVALENT_FOR_LARGE_CIVS");
	int iReasonableGoldForLargePopulations = iPinkyPopGoldEqivalent * GC.getDefineINT("MAX_KISS_PINKY_GOLD_PER_POPULATION");

	// Gamespeed, Handicap, Attitude
	iMinKissPinkyPopulationGold = iMinKissPinkyPopulationGold * iGameSpeedMod / 100 * iHandiCapMod / 100 * (100 - iKissPinkyAttitudeBonus) / 100;
	iMaxKissPinkyPopulationGold = iMaxKissPinkyPopulationGold * iGameSpeedMod / 100 * iHandiCapMod / 100 * (100 - iKissPinkyAttitudeBonus) / 100;
	iReasonableGoldForLargePopulations = iReasonableGoldForLargePopulations * iGameSpeedMod / 100 * iHandiCapMod / 100 * (100 - iKissPinkyAttitudeBonus) / 100;

	// otherwise we will never save enough gold for High Populations and always return 0 thus never trigger Pinky Gold
	// we need to set the min value to reasonable values then
	if (iPopulation > iPinkyPopGoldEqivalent)
	{
		if (iMinKissPinkyPopulationGold > iReasonableGoldForLargePopulations)
		{
			iMinKissPinkyPopulationGold = iReasonableGoldForLargePopulations;
		}
	}

	int iWantedGold = iMaxKissPinkyPopulationGold * GC.getGameINLINE().getSorenRandNum(100, "Ask for pinky gold percent") / 100;

	// so if the King would want more than what we have, we would need to fall back to "max gold we have"
	if (iWantedGold > iMaxGoldPlayerHas)
	{
		iWantedGold = iMaxGoldPlayerHas * GC.getGameINLINE().getSorenRandNum(100, "Ask for pinky gold percent") / 100;
	}

	// but if the King now wants less than he actually should we return 0
	// this causes the Kiss Pinky to be delayed until we have proper Gold
	if (iWantedGold < iMinKissPinkyPopulationGold)
	{
		iWantedGold = 0;
	}

	return iWantedGold;
}

/** NBMOD TAX **/

bool CvPlayerAI::AI_doDiploKissPinky(PlayerTypes ePlayer)
{
	CvPlayerAI& kPlayer = GET_PLAYER(ePlayer);

	if (!kPlayer.isHuman())
	{
		return false;
	}

	if (GC.getEraInfo(kPlayer.getCurrentEra()).isRevolution())
	{
		return false;
	}

	if (kPlayer.getNumCities() == 0)
	{
		return false;
	}

	if (kPlayer.getParent() != getID())
	{
		return false;
	}

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (AI_getContactTimer((ePlayer), CONTACT_DEMAND_TRIBUTE) > 0)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_DEMAND_TRIBUTE), "AI Diplo Kiss Pinky") != 0)
	{
		return false;
	}

    /** NBMOD TAX **/
    /** Original
	int iMaxGoldPercent = GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAIDeclareWarProb() * kPlayer.AI_maxGoldTrade(getID()) / 100;
	int iReceiveGold = iMaxGoldPercent * GC.getGameINLINE().getSorenRandNum(100, "Ask for pinky gold percent") / 100;
	iReceiveGold -= (iReceiveGold % GC.getDefineINT("DIPLOMACY_VALUE_REMAINDER"));
	**/

	int iReceiveGold = NBMOD_GetGoldAsk(ePlayer);

	/** NBMOD TAX **/
	if (iReceiveGold <= 0)
	{
		return false;
	}

	AI_changeContactTimer((ePlayer), CONTACT_DEMAND_TRIBUTE, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_DEMAND_TRIBUTE));

	CvDiploParameters* pDiplo = new CvDiploParameters(getID());
	pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_KING_ASK_FOR_GOLD"));
	pDiplo->addDiploCommentVariable(iReceiveGold);
	pDiplo->setData(iReceiveGold);
	pDiplo->setAIContact(true);
	gDLL->beginDiplomacy(pDiplo, ePlayer);

	return true;
}

bool CvPlayerAI::AI_doDiploOpenBorders(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (getNumCities() == 0)
	{
		return false;
	}

	if (kPlayer.getNumCities() == 0)
	{
		return false;
	}

	if (AI_getContactTimer(ePlayer, CONTACT_OPEN_BORDERS) > 0)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_OPEN_BORDERS), "AI Diplo Open Borders") != 0)
	{
		return false;
	}

	TradeData item;
	setTradeItem(&item, TRADE_OPEN_BORDERS, 0, NULL);

	if (!canTradeItem(ePlayer, item, true) || !kPlayer.canTradeItem(getID(), item, true))
	{
		return false;
	}

	CLinkList<TradeData> theirList;
	theirList.insertAtEnd(item);
	CLinkList<TradeData> ourList;
	ourList.insertAtEnd(item);

	if (kPlayer.isHuman())
	{
		AI_changeContactTimer(ePlayer, CONTACT_OPEN_BORDERS, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_OPEN_BORDERS));
		CvDiploParameters* pDiplo = new CvDiploParameters(getID());
		pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_OFFER_DEAL"));
		pDiplo->setAIContact(true);
		pDiplo->setOurOfferList(theirList);
		pDiplo->setTheirOfferList(ourList);
		gDLL->beginDiplomacy(pDiplo, ePlayer);
	}
	else
	{
		GC.getGameINLINE().implementDeal(getID(), (ePlayer), &ourList, &theirList);
	}

	return true;
}

bool CvPlayerAI::AI_doDiploDefensivePact(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (GET_TEAM(getTeam()).getLeaderID() != getID())
	{
		return false;
	}

	if (kPlayer.getParent() == getID())
	{
		return false;
	}

	if (isEurope())
	{
		return false;
	}

	if (AI_getContactTimer(ePlayer, CONTACT_DEFENSIVE_PACT) > 0)
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_DEFENSIVE_PACT), "AI Diplo Defensive Pact") != 0)
	{
		return false;
	}

	TradeData item;
	setTradeItem(&item, TRADE_DEFENSIVE_PACT, 0, NULL);

	if (!canTradeItem(ePlayer, item, true) || !kPlayer.canTradeItem(getID(), item, true))
	{
		return false;
	}

	CLinkList<TradeData> theirList;
	theirList.insertAtEnd(item);
	CLinkList<TradeData> ourList;
	ourList.insertAtEnd(item);

	if (kPlayer.isHuman())
	{
		AI_changeContactTimer(ePlayer, CONTACT_DEFENSIVE_PACT, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_DEFENSIVE_PACT));
		CvDiploParameters* pDiplo = new CvDiploParameters(getID());
		pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_OFFER_DEAL"));
		pDiplo->setAIContact(true);
		pDiplo->setOurOfferList(theirList);
		pDiplo->setTheirOfferList(ourList);
		gDLL->beginDiplomacy(pDiplo, ePlayer);
	}
	else
	{
		GC.getGameINLINE().implementDeal(getID(), ePlayer, &ourList, &theirList);
	}

	return true;
}

bool CvPlayerAI::AI_doDiploTradeMap(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (AI_getContactTimer(ePlayer, CONTACT_TRADE_MAP) > 0)
	{
		return false;
	}

	if (isEurope())
	{
		return false;
	}

	if (GC.getGameINLINE().getSorenRandNum(GC.getLeaderHeadInfo(getPersonalityType()).getContactRand(CONTACT_TRADE_MAP), "AI Diplo Trade Map") != 0)
	{
		return false;
	}

	TradeData item;
	setTradeItem(&item, TRADE_MAPS, 0, NULL);

	if (!kPlayer.canTradeItem(getID(), item, true) || !canTradeItem(ePlayer, item, true))
	{
		return false;
	}

	if (kPlayer.isHuman() && GET_TEAM(getTeam()).AI_mapTradeVal(kPlayer.getTeam()) < GET_TEAM(kPlayer.getTeam()).AI_mapTradeVal(getTeam()))
	{
		return false;
	}

	CLinkList<TradeData> theirList;
	theirList.insertAtEnd(item);
	CLinkList<TradeData> ourList;
	ourList.insertAtEnd(item);

	if (kPlayer.isHuman())
	{
		AI_changeContactTimer((ePlayer), CONTACT_TRADE_MAP, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_TRADE_MAP));
		CvDiploParameters* pDiplo = new CvDiploParameters(getID());
		pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_OFFER_DEAL"));
		pDiplo->setAIContact(true);
		pDiplo->setOurOfferList(theirList);
		pDiplo->setTheirOfferList(ourList);
		gDLL->beginDiplomacy(pDiplo, ePlayer);
	}
	else
	{
		GC.getGameINLINE().implementDeal(getID(), ePlayer, &ourList, &theirList);
	}

	return true;
}

bool CvPlayerAI::AI_doDiploDeclareWar(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	int iDeclareWarTradeRand = GC.getLeaderHeadInfo(getPersonalityType()).getDeclareWarTradeRand();
	int iMinAtWarCounter = MAX_INT;
	for (int iJ = 0; iJ < MAX_TEAMS; iJ++)
	{
		if (GET_TEAM((TeamTypes)iJ).isAlive())
		{
			if (atWar(((TeamTypes)iJ), getTeam()))
			{
				int iAtWarCounter = GET_TEAM(getTeam()).AI_getAtWarCounter((TeamTypes)iJ);
				if (GET_TEAM(getTeam()).AI_getWarPlan((TeamTypes)iJ) == WARPLAN_DOGPILE)
				{
					iAtWarCounter *= 2;
					iAtWarCounter += 5;
				}
				iMinAtWarCounter = std::min(iAtWarCounter, iMinAtWarCounter);
			}
		}
	}

	if (iMinAtWarCounter < 10)
	{
		iDeclareWarTradeRand *= iMinAtWarCounter;
		iDeclareWarTradeRand /= 10;
		iDeclareWarTradeRand ++;
	}

	if (iMinAtWarCounter < 4)
	{
		iDeclareWarTradeRand /= 4;
		iDeclareWarTradeRand ++;
	}

	if (GC.getGameINLINE().getSorenRandNum(iDeclareWarTradeRand, "AI Diplo Declare War Trade") != 0)
	{
		return false;
	}

	int iGoldValuePercent = AI_goldTradeValuePercent();

	int iBestValue = 0;
	TeamTypes eBestTeam = NO_TEAM;
	for (int iJ = 0; iJ < MAX_TEAMS; iJ++)
	{
		if (GET_TEAM((TeamTypes)iJ).isAlive())
		{
			if (atWar(((TeamTypes) iJ), getTeam()) && !atWar(((TeamTypes) iJ), kPlayer.getTeam()))
			{
				if (GET_TEAM((TeamTypes)iJ).getAtWarCount() < std::max(2, (GC.getGameINLINE().countCivTeamsAlive() / 2)))
				{
					TradeData item;
					setTradeItem(&item, TRADE_WAR, iJ, NULL);

					if (kPlayer.canTradeItem(getID(), item, true))
					{
						int iValue = (1 + GC.getGameINLINE().getSorenRandNum(1000, "AI Declare War Trading"));

						iValue *= (101 + GET_TEAM((TeamTypes)iJ).AI_getAttitudeWeight(getTeam()));
						iValue /= 100;

						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							eBestTeam = ((TeamTypes)iJ);
						}
					}
				}
			}
		}
	}

	if (eBestTeam == NO_TEAM)
	{
		return false;
	}

	iBestValue = 0;
	int iOurValue = GET_TEAM(getTeam()).AI_declareWarTradeVal(eBestTeam, kPlayer.getTeam());
	int iTheirValue = 0;
	int iReceiveGold = 0;
	int iGiveGold = 0;

	if (iTheirValue > iOurValue)
	{
		int iGold = std::min(((iTheirValue - iOurValue) * 100) / iGoldValuePercent, kPlayer.AI_maxGoldTrade(getID()));

		if (iGold > 0)
		{
			TradeData item;
			setTradeItem(&item, TRADE_GOLD, iGold, NULL);

			if (kPlayer.canTradeItem(getID(), item, true))
			{
				iReceiveGold = iGold;
				iOurValue += (iGold * iGoldValuePercent) / 100;
			}
		}
	}
	else if (iOurValue > iTheirValue)
	{
		int iGold = std::min(((iOurValue - iTheirValue) * 100) / iGoldValuePercent, AI_maxGoldTrade(ePlayer));

		if (iGold > 0)
		{
			TradeData item;
			setTradeItem(&item, TRADE_GOLD, iGold, NULL);

			if (canTradeItem((ePlayer), item, true))
			{
				iGiveGold = iGold;
				iTheirValue += (iGold * iGoldValuePercent) / 100;
			}
		}
	}

	if (iTheirValue <= (iOurValue * 3 / 4))
	{
		return false;
	}

	CLinkList<TradeData> theirList;
	CLinkList<TradeData> ourList;

	TradeData item;
	setTradeItem(&item, TRADE_WAR, eBestTeam, NULL);
	theirList.insertAtEnd(item);

	if (iGiveGold != 0)
	{
		setTradeItem(&item, TRADE_GOLD, iGiveGold, NULL);
		ourList.insertAtEnd(item);
	}

	if (iReceiveGold != 0)
	{
		setTradeItem(&item, TRADE_GOLD, iReceiveGold, NULL);
		theirList.insertAtEnd(item);
	}

	GC.getGameINLINE().implementDeal(getID(), (ePlayer), &ourList, &theirList);

	return true;
}


//Convert units from city population to map units (such as pioneers)
void CvPlayerAI::AI_doProfessions()
{
	std::map<int, bool> cityDanger;

	int iLoop;

	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		cityDanger[pLoopCity->getID()] = AI_getPlotDanger(pLoopCity->plot(), 5, true);
	}

	for (int iI = 0; iI < NUM_UNITAI_TYPES; ++iI)
	{
		const UnitAITypes eUnitAI = (UnitAITypes)iI;

		if (!((AI_unitAIValueMultipler(eUnitAI) > 0) && eUnitAI != UNITAI_DEFENSIVE))
			continue;

		const ProfessionTypes eProfession = AI_idealProfessionForUnitAIType(eUnitAI);

		if ((eProfession != NO_PROFESSION) && (eUnitAI == UNITAI_SETTLER || (eProfession != GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession())))
		{
			const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);

			bool bDone = false;

			// WTP: one unit per unit AI and turn (the need is checked again next turn); it took one from every colony
			for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL && !bDone; pLoopCity = nextCity(&iLoop))
			{
				if (pLoopCity->getPopulation() > ((pLoopCity->getHighestPopulation() * 2) / 3))
				{
					if (!cityDanger[pLoopCity->getID()] || AI_unitAIIsCombat(eUnitAI))
					{
						for (int i = 0; i < pLoopCity->getPopulation(); ++i)
						{
							CvUnit* pUnit = pLoopCity->getPopulationUnitByIndex(i);
							if (pUnit != NULL)
							{
								// R&R, ray, code changes for Ideal Profession - START
								// ProfessionTypes eIdealProfession = AI_idealProfessionForUnit(pUnit->getUnitType());
								UnitClassTypes eUnitClassType = (UnitClassTypes)pUnit->getUnitClassType();

								// we need 3 possible ideal professions
								ProfessionTypes eIdealProfession = NO_PROFESSION;
								ProfessionTypes eSecondIdealProfession = NO_PROFESSION;
								ProfessionTypes eThirdIdealProfession = NO_PROFESSION;

								for (int iI = 0; iI < GC.getNumProfessionInfos(); ++iI)
								{

									ProfessionTypes eLoopProfession = (ProfessionTypes)iI;
									CvProfessionInfo& kProfession = GC.getProfessionInfo(eLoopProfession);

									if (kProfession.isCitizen())
									{
										if(eIdealProfession == NO_PROFESSION)
										{
											if ((UnitClassTypes)kProfession.LbD_getExpert() == eUnitClassType)
											{
												eIdealProfession = eLoopProfession;
											}
										}
										else if(eSecondIdealProfession == NO_PROFESSION)
										{
											if ((UnitClassTypes)kProfession.LbD_getExpert() == eUnitClassType)
											{
												eSecondIdealProfession = eLoopProfession;
											}
										}
										else
										{
											if ((UnitClassTypes)kProfession.LbD_getExpert() == eUnitClassType)
											{
												eThirdIdealProfession = eLoopProfession;
												break;
											}
										}
									}
								}

								if (eIdealProfession == NO_PROFESSION || pUnit->getProfession() != eIdealProfession)
								{
									if (eSecondIdealProfession == NO_PROFESSION || pUnit->getProfession() != eSecondIdealProfession)
									{
										if (eThirdIdealProfession == NO_PROFESSION || pUnit->getProfession() != eThirdIdealProfession)
										{
											if (eUnitAI == UNITAI_SETTLER)
											{
												// TODO: Replace this with AdvCiv's AI() accessor
												static_cast<CvCityAI*>(pLoopCity)->AI_doSettlerProfessionCheat();
											}

											// WTP: experts become settlers only if nobody else is available (second pass)
											if (eUnitAI == UNITAI_SETTLER && pUnit->AI_getIdealProfession() != NO_PROFESSION)
											{
												continue;
											}
											if (pUnit->canHaveProfession(eProfession, false) && (AI_professionSuitability(pUnit, eProfession, pLoopCity->plot()) > 100))
											{
												pLoopCity->removePopulationUnit(CREATE_ASSERT_DATA, pUnit, false, eProfession);
												pUnit->AI_setUnitAIType(eUnitAI);
												bDone = true;

												if (gPlayerLogLevel >= 1)
												{
													CvWString szTempString;
													getUnitAIString(szTempString, eUnitAI);

													logBBAI(" Player(%S) City(%S) emits civilian Unit(bdone) (%S) with UnitAI (%S)",
														getCivilizationDescription(), pLoopCity->getNameKey(), pUnit->getNameAndProfession().GetCString(), szTempString.GetCString());
												}
												break;
											}
										}
									}
									// R&R, ray, code changes for Ideal Profession - END
								}
								// R&R, ray, code changes for Ideal Profession - END
							}
						}
					}
				}
			}

			if (!bDone)
			{
				// WTP: the best unit over all colonies
				int iBestValue = 0;
				CvUnit* pBestUnit = NULL;
				CvCity* pBestCity = NULL;
				for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
				{

					if (!cityDanger[pLoopCity->getID()] || AI_unitAIIsCombat(eUnitAI))
					{
						if (pLoopCity->getPopulation() > ((pLoopCity->getHighestPopulation() * 2) / 3))
						{
							for (int i = 0; i < pLoopCity->getPopulation(); ++i)
							{
								CvUnit* pUnit = pLoopCity->getPopulationUnitByIndex(i);
								if (pUnit != NULL)
								{
									if (pUnit->canHaveProfession(eProfession, false))
									{
										int iValue = AI_professionSuitability(pUnit, eProfession, pLoopCity->plot());

										if (pUnit->getProfession() == NO_PROFESSION)
										{
											int iValue = AI_professionSuitability(pUnit, eProfession, pLoopCity->plot());

										}
										else if (pUnit->getProfession() == pUnit->AI_getIdealProfession())
										{
											iValue /= 4;
										}

										if (eUnitAI == UNITAI_SETTLER)
										{
											// TODO: Replace this with AdvCiv's AI() accessor
											static_cast<CvCityAI*>(pLoopCity)->AI_doSettlerProfessionCheat();

											// WTP: experts only if nobody else is available (in any colony)
											if (pUnit->AI_getIdealProfession() != NO_PROFESSION)
											{
												iValue /= 1000;
											}
											else
											{
												iValue *= 120;
												iValue /= 100;
											}
										}
										else if (eUnitAI == UNITAI_SCOUT && pLoopCity->plot()->area()->getNumAIUnits(pLoopCity->getOwnerINLINE(), UNITAI_SCOUT) == 0)
										{
											iValue *= 250;
											iValue /= 100;
										}

										iValue *= 50;
										iValue += GC.getGameINLINE().getSorenRandNum(100, "AI pick unit");

										if (iValue > iBestValue)
										{
											iBestValue = iValue;
											pBestUnit = pUnit;
											pBestCity = pLoopCity;
										}
									}
								}
							}
						}
					}
				}
				if (pBestUnit != NULL)
				{
					pBestCity->removePopulationUnit(CREATE_ASSERT_DATA, pBestUnit, false, eProfession);
					pBestUnit->AI_setUnitAIType(eUnitAI);

					if (gPlayerLogLevel >= 1)
					{
						CvWString szTempString;
						getUnitAIString(szTempString, eUnitAI);

						logBBAI(" Player(%S) City (%S) emits civilian Unit (!bdone) (%S) with UnitAI (%S)",
							getCivilizationDescription(), pBestCity->getNameKey(), pBestUnit->getNameAndProfession().GetCString(), szTempString.GetCString());

					}
				}
			}
		}
	}

	//Military
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity->AI_isDanger())
		{
			const ProfessionTypes eProfession = AI_idealProfessionForUnitAIType(UNITAI_DEFENSIVE, pLoopCity);

			if ((eProfession != NO_PROFESSION) && (eProfession != GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession()))
			{
				CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
				bool bDone = false;

				const int iNeededDefenders = pLoopCity->AI_neededDefenders();
				const int iHaveDefenders = pLoopCity->AI_numDefenders(true, false);

				if (iHaveDefenders < iNeededDefenders)
				{
					while (!bDone)
					{
						int iBestValue = 0;
						CvUnit* pBestUnit = NULL;

						for (int i = 0; i < pLoopCity->getPopulation(); ++i)
						{
							CvUnit* pUnit = pLoopCity->getPopulationUnitByIndex(i);
							if (pUnit != NULL)
							{
								if (pUnit->canHaveProfession(eProfession, false))
								{
									int iValue = AI_professionSuitability(pUnit, eProfession, pLoopCity->plot());

									if (pUnit->getProfession() == NO_PROFESSION)
									{

									}
									else if (pUnit->getProfession() == pUnit->AI_getIdealProfession())
									{
										iValue /= 4;
									}

									iValue *= 100;
									iValue += GC.getGameINLINE().getSorenRandNum(100, "AI pick unit");

									if (iValue > iBestValue)
									{
										iBestValue = iValue;
										pBestUnit = pUnit;
									}
								}
							}
						}

						if (pBestUnit != NULL)
						{
							pLoopCity->removePopulationUnit(CREATE_ASSERT_DATA, pBestUnit, false, eProfession);
							pBestUnit->AI_setUnitAIType(UNITAI_DEFENSIVE);
							if (gPlayerLogLevel >= 1) logBBAI(" Player (%S)'s City (%S) emits military Unit (%S)",
								getCivilizationDescription(), pLoopCity->getNameKey(), pBestUnit->getNameAndProfession().GetCString());
						}
						else
						{
							bDone = true;
						}
					}
				}
			}
		}
	}



}

void CvPlayerAI::AI_doEurope()
{
	MOD_PROFILE("CvPlayerAI::AI_doEurope");
	//Buy Units.
	UnitTypes eBuyUnit;
	UnitAITypes eBuyUnitAI;
	int iBuyUnitValue;

	if (!canTradeWithEurope())
	{
		return;
	}

	if (AI_isEarlyPhaseActive())
	{
		// WTP: phase 0 and the early strategies: only their own purchases
		AI_doEarlyPhase();
		return;
	}

	if (!isHuman() && !isNative() && !isEurope())
	{
		// WTP: the second scout before anything else; while saving for it nothing else is bought
		if (AI_doScoutPurchase())
		{
			return;
		}

		// WTP: then an expert whose learning was blocked (saving until it is bought)
		if (AI_doWfExpertPurchase())
		{
			return;
		}

		// WTP: from 5 waiting treasures a galleon (the only ship that carries them) comes first
		if (AI_doTreasureShipPurchase())
		{
			return;
		}

		// WTP: then at least one transport per colony (inland colonies too)
		if (AI_doColonyTransportPurchase())
		{
			return;
		}

		// WTP, Schmiddie, AI Minimum Fleet - START
		// WTP: no minimum warship any more, only a transport if there is none
		UnitAITypes eRequiredShipAI = NO_UNITAI;

		if (AI_totalUnitAIs(UNITAI_TRANSPORT_SEA) < 1)
		{
			eRequiredShipAI = UNITAI_TRANSPORT_SEA;
		}

		if (eRequiredShipAI != NO_UNITAI)
		{
			// WTP: transports by value per price; they must reach our colonies
			const UnitTypes eBestShip = AI_bestShipToBuy(eRequiredShipAI, false);

			if (eBestShip != NO_UNIT)
			{
				int iShipPrice = getEuropeUnitBuyPrice(eBestShip);

				if (getGold() > iShipPrice)
				{
					CvUnit* pUnit = buyEuropeUnit(eBestShip, 100);

					FAssert(pUnit != NULL);

					if (pUnit != NULL)
					{
						pUnit->AI_setUnitAIType(eRequiredShipAI);
					}
				}
				else if (gPlayerLogLevel >= 1)
				{
					logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) saves for its first transport %S (%d gold, have %d)",
						getID(), getCivilizationDescription(0), GC.getUnitInfo(eBestShip).getDescription(), iShipPrice, getGold());
				}

				return;
			}
		}
		// WTP, Schmiddie, AI Minimum Fleet - END

		// WTP: enough ships to bring the people waiting in Europe come first; while saving for one, buy nothing else
		if (AI_doTransportPurchase())
		{
			return;
		}

		// WTP: after the ships the missing pioneers come first
		AI_doEquipPioneersInEurope();

		// WTP: two defenders per colony (in peace too) before civilian units are bought; in war an even mix of military
		// units instead, with up to 30% of the gold spent in the war going to civilian purchases
		const bool bWar = GET_TEAM(getTeam()).getAnyWarPlanCount() > 0;
		if (!bWar)
		{
			m_iWarMilitarySpent = 0;
			m_iWarCivilianSpent = 0;
			if (!AI_doMilitiaPurchase())
			{
				AI_doCivilianPurchases();
			}
		}
		else
		{
			const int iGoldBefore = getGold();
			AI_doCivilianPurchases();
			m_iWarCivilianSpent += std::max(0, iGoldBefore - getGold());
			AI_doWarMilitaryPurchase();
		}

		//Always refresh at start of new turn (maybe do this smarter but it's okay for now)
		AI_updateNextBuyUnit();
		// Don't by profession units for now until we find a better way to decide who we can productively employ
		//AI_updateNextBuyProfession();
	}

	// TAC - AI More Immigrants - koma13 - START
	if (GC.getGameINLINE().getGameTurn() < GC.getGameINLINE().AI_adjustedTurn(20))
	{
		AI_hurryDocks();
	}
	// TAC - AI More Immigrants - koma13 - END

	eBuyUnit = AI_nextBuyUnit(&eBuyUnitAI, &iBuyUnitValue);

	UnitTypes eBuyProfessionUnit = NO_UNIT;
	ProfessionTypes eBuyProfession = NO_PROFESSION;
	UnitAITypes eBuyProfessionAI = NO_UNITAI;
	int iBuyProfessionValue = -1;

	//eBuyProfessionUnit = AI_nextBuyProfessionUnit(&eBuyProfession, &eBuyProfessionAI, &iBuyProfessionValue);

	int iUnitPrice = 0;

	if (eBuyUnit != NO_UNIT)
	{
		iUnitPrice = getEuropeUnitBuyPrice(eBuyUnit);
	}

	if ((eBuyUnit != NO_UNIT) && ((iBuyUnitValue > iBuyProfessionValue) || (iUnitPrice < getGold())))
	{
		if (getGold() > iUnitPrice)
		{
			CvUnit* pUnit = buyEuropeUnit(eBuyUnit, 100);

			FAssert(pUnit != NULL);
			FAssert(eBuyUnitAI != NO_UNITAI);

			if (pUnit != NULL)
			{
				pUnit->AI_setUnitAIType(eBuyUnitAI);

				// TAC - AI Military Buildup - koma13 - START
				if (AI_isStrategy(STRATEGY_MILITARY_BUILDUP))
				{
					AI_clearStrategy(STRATEGY_MILITARY_BUILDUP);
				}
				// TAC - AI Military Buildup - koma13 - END
			}

			AI_updateNextBuyUnit();
		}
	}
}

void CvPlayerAI::AI_nativeYieldGift(CvUnit* pUnit)
{
	FAssert(pUnit != NULL);
	FAssert(pUnit->isOnMap_());
	FAssert(pUnit->plot()->isCity());
	FAssert(isNative());

	CvCity* pHomeCity = pUnit->getHomeCity();
	if (pHomeCity == NULL)
	{
		pHomeCity = GC.getMap().findCity(pUnit->getX_INLINE(), pUnit->getY_INLINE(), pUnit->getOwnerINLINE());
		pUnit->setHomeCity(pHomeCity);
	}

	if (pHomeCity == NULL)
	{
		return;
	}

	YieldTypes eBestYield = getBestYieldForTrade(*pHomeCity, "Native Yield Gift: pick yield");
	if (eBestYield == NO_YIELD)
	{
		return;
	}

	CvCity* pOtherCity = pUnit->plot()->getPlotCity();
	FAssert(pOtherCity != NULL);
	FAssert(!pOtherCity->isNative());

	if(pOtherCity != NULL)
	{
		//give some yields from pBestCity to pOtherCity
		int iYieldPercent = 5 * (AI_getAttitudeVal(pOtherCity->getOwnerINLINE(), false) + GC.getGameINLINE().getSorenRandNum(10, "Native Yield Gift: pick amount"));
		int iYieldAmount = iYieldPercent * pHomeCity->getYieldStored(eBestYield) / 100;
		iYieldAmount = std::min(iYieldAmount, pHomeCity->getYieldStored(eBestYield));
		iYieldAmount = std::max(iYieldAmount, 0);
		if(iYieldAmount > 0)
		{
			pHomeCity->changeYieldStored(eBestYield, -iYieldAmount);
			pOtherCity->changeYieldStored(eBestYield, iYieldAmount);

			//AI_changeContactTimer(pOtherCity->getOwnerINLINE(), CONTACT_YIELD_GIFT, GC.getLeaderHeadInfo(getPersonalityType()).getContactDelay(CONTACT_YIELD_GIFT));

			//popup dialog
			CvPlayer& kOtherPlayer = GET_PLAYER(pOtherCity->getOwnerINLINE());
			if(kOtherPlayer.isHuman())
			{
				CvDiploParameters* pDiplo = new CvDiploParameters(getID());
				pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_NATIVES_YIELD_GIFT"));
				pDiplo->addDiploCommentVariable(iYieldAmount);
				pDiplo->addDiploCommentVariable(GC.getYieldInfo(eBestYield).getChar());
				pDiplo->addDiploCommentVariable(pOtherCity->getNameKey());
				pDiplo->setAIContact(true);
				gDLL->beginDiplomacy(pDiplo, kOtherPlayer.getID());
			}
		}
	}
}

// R&R, ray, Natives Trading - START
// WTP: price per unit of goods the natives bring to a colony: 1 less than their usual price, fur 2
int CvPlayerAI::AI_nativeDeliveryPrice(YieldTypes eYield)
{
	if (eYield == YIELD_FUR)
	{
		return 2;
	}
	return std::max(0, GC.getYieldInfo(eYield).getNativeSellPrice() - 1);
}

void CvPlayerAI::AI_nativeTrade(CvUnit* pUnit)
{
	FAssert(pUnit != NULL);
	FAssert(pUnit->isOnMap_());
	FAssert(pUnit->plot()->isCity());
	FAssert(isNative());

	CvCity* pHomeCity = pUnit->getHomeCity();
	if (pHomeCity == NULL)
	{
		pHomeCity = GC.getMap().findCity(pUnit->getX_INLINE(), pUnit->getY_INLINE(), pUnit->getOwnerINLINE());
		pUnit->setHomeCity(pHomeCity);
	}

	if (pHomeCity == NULL)
	{
		return;
	}
	
	YieldTypes eBestYield = getBestYieldForTrade(*pHomeCity, "Native Yield Gift: pick yield");
	if (eBestYield == NO_YIELD)
	{
		return;
	}

	CvCity* pOtherCity = pUnit->plot()->getPlotCity();
	FAssert(pOtherCity != NULL);
	FAssert(!pOtherCity->isNative());

	if(pOtherCity != NULL)
	{
		//give some yields from pBestCity to pOtherCity
		int iYieldPercent = 5 * (AI_getAttitudeVal(pOtherCity->getOwnerINLINE(), false) + GC.getGameINLINE().getSorenRandNum(10, "Native Yield Trade: pick amount"));
		int iYieldAmount = iYieldPercent * pHomeCity->getYieldStored(eBestYield) / 100;
		iYieldAmount = std::min(iYieldAmount, pHomeCity->getYieldStored(eBestYield));
		iYieldAmount = std::max(iYieldAmount, 0);
		if(iYieldAmount > 0)
		{
			CvPlayer& kOtherPlayer = GET_PLAYER(pOtherCity->getOwnerINLINE());
			int priceToPay = AI_nativeDeliveryPrice(eBestYield) * iYieldAmount; // WTP

			// simple case for AI
			if(!kOtherPlayer.isHuman())
			{
				if (kOtherPlayer.getGold() >= priceToPay)
				{
					// WTP, ray trying to fix negative storage bug - START
					// prevent that there is negative storage caused in the Native City
					// although above it was actually already check by std::min ...
					if (pHomeCity->getYieldStored(eBestYield) >= iYieldAmount)
					{
						OOS_LOG_3("CvPlayerAI::AI_nativeTrade", getTypeStr(eBestYield), priceToPay);
						pOtherCity->changeYieldStored(eBestYield, iYieldAmount);
						kOtherPlayer.changeGold(-priceToPay);
						pHomeCity->changeYieldStored(eBestYield, -iYieldAmount);
						GET_PLAYER(pHomeCity->getOwnerINLINE()).changeGold(priceToPay);
					}
				}
			}

			// popup dialog for Human
			else
			{
				CvDiploParameters* pDiplo = new CvDiploParameters(getID());
				if (kOtherPlayer.getGold() >= priceToPay)
				{
					pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_NATIVE_TRADE"));
				}
				else
				{
					pDiplo->setDiploComment((DiploCommentTypes)GC.getInfoTypeForString("AI_DIPLOCOMMENT_NATIVE_TRADE_CANNOT_AFFORD"));
				}
				pDiplo->addDiploCommentVariable(iYieldAmount);
				pDiplo->addDiploCommentVariable(GC.getYieldInfo(eBestYield).getChar());
				pDiplo->addDiploCommentVariable(pOtherCity->getNameKey());
				pDiplo->addDiploCommentVariable(priceToPay);
				pDiplo->setData(pUnit->getID());
				// caching data in Unit
				pUnit->setYieldForNativeTrade(eBestYield);
				pUnit->setAmountForNativeTrade(iYieldAmount);

				pDiplo->setAIContact(true);
				gDLL->beginDiplomacy(pDiplo, kOtherPlayer.getID());
			}
		}
	}
}

// Function randomly chooses a yield weighted proportionally by the amount of valid yields available in settlement
YieldTypes CvPlayerAI::getBestYieldForTrade(const CvCity& city, const char* pszLog) const
{
	// Compute the total weight of all items together.
	int totalYieldsStored = 0;
	for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield) 
	{
		if (isYieldValidForNativeTrade(eYield, city))
		{
			int iYieldWeight = city.getYieldStored(eYield);
			if (eYield == YIELD_FOOD)
			{
				iYieldWeight /= 4;
			}
			totalYieldsStored += iYieldWeight;
		}
	}

	// Choose a random yield with probability weighted on how much yield is stored.
	int rand = GC.getGameINLINE().getSorenRandNum(totalYieldsStored, pszLog);
	for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield) 
	{
		if (isYieldValidForNativeTrade(eYield, city))
		{
			int iYieldWeight = city.getYieldStored(eYield);
			if (eYield == YIELD_FOOD)
			{
				iYieldWeight /= 4;
			}
			rand -= iYieldWeight;
			if (rand <= 0)
			{
				return eYield;
			}
		}
	}

	return NO_YIELD;
}

bool CvPlayerAI::isYieldValidForNativeTrade(YieldTypes yield, const CvCity& city) const
{
	return yield >= FIRST_YIELD
		&& yield < NUM_CARGO_YIELD_TYPES
		&& (YIELD_HORSES != yield)
		&& city.canProduceYield(yield)
		&& GC.getYieldInfo(yield).getNativeSellPrice() > 0;
}

// R&R, ray, Natives Trading - END

bool CvPlayerAI::AI_isYieldForSale(YieldTypes eYield) const
{
	if (!GC.getYieldInfo(eYield).isCargo())
	{
		return false;
	}

	switch (eYield)
	{
		case YIELD_FOOD:
		case YIELD_LUMBER:
		case YIELD_HARDWOOD:
		case YIELD_STONE:
		case YIELD_CLAY:
			return false;
			break;
		case YIELD_HEMP:
		case YIELD_FLAX:
		case YIELD_ORE:
		case YIELD_COAL:
		case YIELD_CHAR_COAL:
		case YIELD_PEAT:
			return true;
			break;
		case YIELD_SHEEP:
		case YIELD_GOATS:
		case YIELD_PIGS:
		case YIELD_CATTLE:
		case YIELD_CHICKEN:
		case YIELD_GEESE:
			return true;
			break;
		case YIELD_HORSES:
			return !AI_isYieldNeeded(eYield); // R&R, ray, AI improvement for horses - START
			break;
		case YIELD_COCOA_FRUITS:
		case YIELD_COFFEE_BERRIES:
		case YIELD_PEANUTS:
		case YIELD_MILK:
		case YIELD_TOBACCO:
		case YIELD_YERBA_LEAVES:
		case YIELD_COTTON:
		case YIELD_INDIGO:
		case YIELD_LOGWOOD:
		case YIELD_COCHINEAL:
		case YIELD_WOOL:
		case YIELD_GOAT_HIDES:
		case YIELD_PIG_SKIN:
		case YIELD_HIDES:
		case YIELD_DOWNS:
		case YIELD_FUR:
		case YIELD_PREMIUM_FUR:
		case YIELD_RAW_SALT:
		case YIELD_RED_PEPPER:
		case YIELD_VANILLA_PODS:
		case YIELD_BARLEY:
		case YIELD_SUGAR:
		case YIELD_FRUITS:
		case YIELD_GRAPES:
		case YIELD_OLIVES:
		case YIELD_RAPE:
		case YIELD_WHALE_BLUBBER:
		case YIELD_VALUABLE_WOOD:
			return true;
			break;
		case YIELD_TRADE_GOODS:
			return false;
			break;
		case YIELD_BAKERY_GOODS:
		case YIELD_ROPE:
		case YIELD_SAILCLOTH:
			return !AI_isYieldNeeded(eYield);
			break;
		// Erik: It does the AI little good to sell these yields
		case YIELD_TOOLS:
		case YIELD_BLADES:
		case YIELD_MUSKETS:
		case YIELD_CANNONS:
		case YIELD_BLACK_POWDER:
			return false;
			break;
		case YIELD_RICE:
		case YIELD_CASSAVA:
		case YIELD_COCA_LEAVES:
		case YIELD_MAPLE_SIRUP:
		case YIELD_KAUTSCHUK:
		case YIELD_WILD_FEATHERS:
		case YIELD_SILVER:
		case YIELD_GOLD:
		case YIELD_GEMS:
		case YIELD_COCOA:
		case YIELD_COFFEE:
		case YIELD_ROASTED_PEANUTS:
		case YIELD_CHEESE:
		case YIELD_CIGARS:
		case YIELD_YERBA_TEA:
		case YIELD_CLOTH:
		case YIELD_COLOURED_CLOTH:
		case YIELD_FESTIVE_CLOTHES:
		case YIELD_WOOL_CLOTH:
		case YIELD_COLOURED_WOOL_CLOTH:
		case YIELD_EVERYDAY_CLOTHES:
		case YIELD_PIG_LEATHER:
		case YIELD_LEATHER:
		case YIELD_GOAT_HIDE_BOOTS:
		case YIELD_PADDED_LEATHER_COATS:
		case YIELD_COATS:
		case YIELD_PREMIUM_COATS:
		case YIELD_SALT:
		case YIELD_SPICES:
		case YIELD_VANILLA:
		case YIELD_CHOCOLATE:
		case YIELD_BEER:
		case YIELD_RUM:
		case YIELD_HOOCH:
		case YIELD_WINE:
		case YIELD_OLIVE_OIL:
		case YIELD_RAPE_OIL:
		case YIELD_WHALE_OIL:
		case YIELD_POTTERY:
		case YIELD_FURNITURE:
		case YIELD_PADDED_FURNITURE:
		case YIELD_FIELD_WORKER_TOOLS:
		case YIELD_HOUSEHOLD_GOODS:
			return true;
			break;
		case YIELD_LUXURY_GOODS:
			return false;
			break;
		case YIELD_HAMMERS:
		case YIELD_BELLS:
		case YIELD_CROSSES:
		case YIELD_CULTURE:
		case YIELD_HEALTH: // R&R, ray, Health - START
		case YIELD_EDUCATION:
		case YIELD_HAPPINESS: // WTP, ray, Happiness - START
		case YIELD_UNHAPPINESS: // WTP, ray, Happiness - START
		case YIELD_LAW: // WTP, ray, Crime and Law - START
		case YIELD_CRIME: // WTP, ray, Crime and Law - START
			FAssertMsg(false, "Selling intangibles?");
			break;
		default:
			FAssert(false);
	}

	return false;
}
// TAC - AI Economy - koma13 - START
bool CvPlayerAI::AI_isYieldNeeded(YieldTypes eYield, int iCapacityPercent, CvCity* pCity) const
{
	FAssert (eYield != NO_YIELD);

	if (eYield == YIELD_FOOD)
	{
		return true;
	}

	// R&R, ray, AI improvement for horses - START
	if (eYield == YIELD_HORSES)
	{
		int iHorsesAveragePerCity = 0;
		int itotalHorsesStored = 0;
		int iNumCities = 0;

		int iThreshold = 50;
		iThreshold *= GC.getEraInfo(GC.getGameINLINE().getStartEra()).getTrainPercent();
		iThreshold /= 100;

		CvCity* xLoopCity;
		int xLoop;

		for (xLoopCity = firstCity(&xLoop); xLoopCity != NULL; xLoopCity = nextCity(&xLoop))
		{
			if (pCity == NULL || pCity == xLoopCity)
			{
				itotalHorsesStored += xLoopCity->getYieldStored(eYield);
				iNumCities +=1;
			}
		}
		if (iNumCities != 0)
		{
			iHorsesAveragePerCity = itotalHorsesStored / iNumCities;
		}

		if (iHorsesAveragePerCity <= iThreshold)
		{
			return true;
		}
		else
		{
			return false;
		}
	}
	// R&R, ray, AI improvement for horses - END

	int iTotalNeeded = 0;

	CvCity* pLoopCity;
	int iLoop;

	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pCity == NULL || pCity == pLoopCity)
		{
			if (iCapacityPercent == -1)
			{
				iTotalNeeded += pLoopCity->AI_getRequiredYieldLevel(eYield) - pLoopCity->getYieldStored(eYield);
			}
			else
			{
				// R&R, ray, fix for new storage capacity
				iTotalNeeded += ((pLoopCity->getMaxYieldCapacity() / 5 * iCapacityPercent) / 100) - pLoopCity->getYieldStored(eYield);
			}
		}
	}

	if (iTotalNeeded > 0)
	{
		return true;
	}

	return false;
}
// TAC - AI Economy - koma13 - END

bool CvPlayerAI::AI_isYieldFinalProduct(YieldTypes eYield) const
{
	if (!GC.getYieldInfo(eYield).isCargo())
	{
		return false;
	}

	bool bFinal = true;

	switch (eYield)
	{
		case YIELD_FOOD:
		case YIELD_LUMBER:
		case YIELD_HARDWOOD:
		case YIELD_STONE:
		case YIELD_CLAY:
			bFinal = false;
			break;
		case YIELD_HEMP:
		case YIELD_FLAX:
		case YIELD_ORE:
		case YIELD_COAL:
		case YIELD_CHAR_COAL:
		case YIELD_PEAT:
		case YIELD_SHEEP:
		case YIELD_GOATS:
		case YIELD_PIGS:
		case YIELD_CATTLE:
		case YIELD_CHICKEN:
		case YIELD_GEESE:
			{
				int iLoop;
				CvCity* pLoopCity = NULL;
				for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
				{
					if (pLoopCity->AI_getNeededYield(eYield) > 0)
					{
						bFinal = false;
						break;
					}
				}
			}
			break;
		case YIELD_HORSES:
			bFinal = false;
			break;
		case YIELD_COCOA_FRUITS:
		case YIELD_COFFEE_BERRIES:
		case YIELD_PEANUTS:
		case YIELD_MILK:
		case YIELD_TOBACCO:
		case YIELD_YERBA_LEAVES:
		case YIELD_COTTON:
		case YIELD_INDIGO:
		case YIELD_LOGWOOD:
		case YIELD_COCHINEAL:
		case YIELD_WOOL:
		case YIELD_GOAT_HIDES:
		case YIELD_PIG_SKIN:
		case YIELD_HIDES:
		case YIELD_DOWNS:
		case YIELD_FUR:
		case YIELD_PREMIUM_FUR:
		case YIELD_RAW_SALT:
		case YIELD_RED_PEPPER:
		case YIELD_VANILLA_PODS:
		case YIELD_BARLEY:
		case YIELD_SUGAR:
		case YIELD_FRUITS:
		case YIELD_GRAPES:
		case YIELD_OLIVES:
		case YIELD_RAPE:
		case YIELD_WHALE_BLUBBER:
		case YIELD_VALUABLE_WOOD:
			{
				int iLoop;
				CvCity* pLoopCity = NULL;
				for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
				{
					if (pLoopCity->AI_getNeededYield(eYield) > 0)
					{
						bFinal = false;
						break;
					}
				}
			}
			break;
		case YIELD_TRADE_GOODS:
			bFinal = true;
			break;
		case YIELD_BAKERY_GOODS:
		case YIELD_ROPE:
		case YIELD_SAILCLOTH:
		case YIELD_TOOLS:
		case YIELD_BLADES:
		case YIELD_MUSKETS:
		case YIELD_CANNONS:
		case YIELD_BLACK_POWDER:
			bFinal = false;
			break;
		case YIELD_RICE:
		case YIELD_CASSAVA:
		case YIELD_COCA_LEAVES:
		case YIELD_MAPLE_SIRUP:
		case YIELD_KAUTSCHUK:
		case YIELD_WILD_FEATHERS:
		case YIELD_SILVER:
		case YIELD_GOLD:
		case YIELD_GEMS:
		case YIELD_COCOA:
		case YIELD_COFFEE:
		case YIELD_ROASTED_PEANUTS:
		case YIELD_CHEESE:
		case YIELD_CIGARS:
		case YIELD_YERBA_TEA:
			bFinal = true;
			break;
		case YIELD_CLOTH:
		case YIELD_WOOL_CLOTH:
		case YIELD_COLOURED_CLOTH:
		case YIELD_COLOURED_WOOL_CLOTH:
			{
				int iLoop;
				CvCity* pLoopCity = NULL;
				for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
				{
					if (pLoopCity->AI_getNeededYield(eYield) > 0)
					{
						bFinal = false;
						break;
					}
				}
			}
			break;
		case YIELD_FESTIVE_CLOTHES:
		case YIELD_EVERYDAY_CLOTHES:
		case YIELD_PIG_LEATHER:
		case YIELD_LEATHER:
		case YIELD_GOAT_HIDE_BOOTS:
		case YIELD_PADDED_LEATHER_COATS:
		case YIELD_COATS:
		case YIELD_PREMIUM_COATS:
		case YIELD_SALT:
		case YIELD_SPICES:
		case YIELD_VANILLA:
		case YIELD_CHOCOLATE:
		case YIELD_BEER:
		case YIELD_RUM:
		case YIELD_HOOCH:
		case YIELD_WINE:
		case YIELD_OLIVE_OIL:
		case YIELD_RAPE_OIL:
		case YIELD_WHALE_OIL:
		case YIELD_POTTERY:
		case YIELD_FURNITURE:
		case YIELD_PADDED_FURNITURE:
		case YIELD_FIELD_WORKER_TOOLS:
		case YIELD_HOUSEHOLD_GOODS:
		case YIELD_LUXURY_GOODS:
			bFinal = true;
			break;
		case YIELD_HAMMERS:
		case YIELD_BELLS:
		case YIELD_CROSSES:
		case YIELD_CULTURE:
		case YIELD_HEALTH: // R&R, ray, Health - START
		case YIELD_EDUCATION:
		case YIELD_HAPPINESS: // WTP, ray, Happiness - START
		case YIELD_UNHAPPINESS: // WTP, ray, Happiness - START
		case YIELD_LAW: // WTP, ray, Crime and Law - START
		case YIELD_CRIME: // WTP, ray, Crime and Law - START
			bFinal = false;
			FAssertMsg(false, "Selling intangibles?");
			break;
		default:
			FAssert(false);
	}

	return bFinal;
}

bool CvPlayerAI::AI_shouldBuyFromEurope(YieldTypes eYield) const
{
	if (!GC.getYieldInfo(eYield).isCargo())
	{
		return false;
	}

	bool bBuy = false;

	switch (eYield)
	{
		case YIELD_FOOD:
		case YIELD_LUMBER:
		case YIELD_STONE:
		case YIELD_CLAY:
		case YIELD_HARDWOOD:
		case YIELD_HEMP:
		case YIELD_FLAX:
		case YIELD_ORE:
		case YIELD_COAL:
		case YIELD_CHAR_COAL:
		case YIELD_PEAT:
		case YIELD_SHEEP:
		case YIELD_GOATS:
		case YIELD_PIGS:
		case YIELD_CATTLE:
		case YIELD_CHICKEN:
		case YIELD_GEESE:
		case YIELD_HORSES:
		case YIELD_RICE:
		case YIELD_CASSAVA:
		case YIELD_COCA_LEAVES:
		case YIELD_MAPLE_SIRUP:
		case YIELD_KAUTSCHUK:
		case YIELD_COCOA_FRUITS:
		case YIELD_COFFEE_BERRIES:
		case YIELD_PEANUTS:
		case YIELD_MILK:
		case YIELD_TOBACCO:
		case YIELD_YERBA_LEAVES:
		case YIELD_COTTON:
		case YIELD_INDIGO:
		case YIELD_LOGWOOD:
		case YIELD_COCHINEAL:
		case YIELD_WOOL:
		case YIELD_GOAT_HIDES:
		case YIELD_PIG_SKIN:
		case YIELD_HIDES:
		case YIELD_DOWNS:
		case YIELD_FUR:
		case YIELD_PREMIUM_FUR:
		case YIELD_RAW_SALT:
		case YIELD_RED_PEPPER:
		case YIELD_VANILLA_PODS:
		case YIELD_BARLEY:
		case YIELD_SUGAR:
		case YIELD_FRUITS:
		case YIELD_GRAPES:
		case YIELD_OLIVES:
		case YIELD_RAPE:
		case YIELD_WHALE_BLUBBER:
		case YIELD_VALUABLE_WOOD:
		case YIELD_TRADE_GOODS:
		case YIELD_BAKERY_GOODS:
		case YIELD_ROPE:
		case YIELD_SAILCLOTH:
			bBuy = false;
			break;
		case YIELD_TOOLS:
			bBuy = AI_isYieldNeeded(eYield);
			break;
		case YIELD_BLADES:
		case YIELD_MUSKETS:
		case YIELD_CANNONS:
		case YIELD_BLACK_POWDER:
		case YIELD_WILD_FEATHERS:
		case YIELD_SILVER:
		case YIELD_GOLD:
		case YIELD_GEMS:
		case YIELD_COCOA:
		case YIELD_COFFEE:
		case YIELD_ROASTED_PEANUTS:
		case YIELD_CHEESE:
		case YIELD_CIGARS:
		case YIELD_YERBA_TEA:
		case YIELD_CLOTH:
		case YIELD_COLOURED_CLOTH:
		case YIELD_FESTIVE_CLOTHES:
		case YIELD_WOOL_CLOTH:
		case YIELD_COLOURED_WOOL_CLOTH:
		case YIELD_EVERYDAY_CLOTHES:
		case YIELD_PIG_LEATHER:
		case YIELD_LEATHER:
		case YIELD_GOAT_HIDE_BOOTS:
		case YIELD_PADDED_LEATHER_COATS:
		case YIELD_COATS:
		case YIELD_PREMIUM_COATS:
		case YIELD_SALT:
		case YIELD_SPICES:
		case YIELD_VANILLA:
		case YIELD_CHOCOLATE:
		case YIELD_BEER:
		case YIELD_RUM:
		case YIELD_HOOCH:
		case YIELD_WINE:
		case YIELD_OLIVE_OIL:
		case YIELD_RAPE_OIL:
		case YIELD_WHALE_OIL:
		case YIELD_POTTERY:
		case YIELD_FURNITURE:
		case YIELD_PADDED_FURNITURE:
		case YIELD_FIELD_WORKER_TOOLS:
		case YIELD_HOUSEHOLD_GOODS:
		case YIELD_LUXURY_GOODS:
			bBuy = false;
			break;
		case YIELD_HAMMERS:
		case YIELD_BELLS:
		case YIELD_CROSSES:
		case YIELD_CULTURE:
		case YIELD_HEALTH: // R&R, ray, Health - START
		case YIELD_EDUCATION:
		case YIELD_HAPPINESS: // WTP, ray, Happiness - START
		case YIELD_UNHAPPINESS: // WTP, ray, Happiness - START
		case YIELD_LAW: // WTP, ray, Crime and Law - START
		case YIELD_CRIME: // WTP, ray, Crime and Law - START
			bBuy = false;
			FAssertMsg(false, "Selling intangibles?");
			break;
		default:
			FAssert(false);
	}

	return bBuy;
}

// TAC - AI More food - koma13 - START
//int CvPlayerAI::AI_yieldValue(YieldTypes eYield, bool bProduce, int iAmount)
int CvPlayerAI::AI_yieldValue(YieldTypes eYield, bool bProduce, int iAmount, bool bFood) const
// TAC - AI More food - koma13 - END
{
	int iValue = 0;
	if (bProduce)
	{
		iValue += 100 * (isNative() ? GC.getYieldInfo(eYield).getNativeBaseValue() : GC.getYieldInfo(eYield).getAIBaseValue());
	}
	if (eYield == YIELD_FOOD)
	{
		// TAC - AI More food - koma13 - START

		if (bFood)
		{
			iValue += 75 * (isNative() ? GC.getYieldInfo(eYield).getNativeBaseValue() : GC.getYieldInfo(eYield).getAIBaseValue());
		}
		// TAC - AI More food - koma13 - END
	}

	else if (isNative())
	{
		CvYieldInfo& kYieldInfo = GC.getYieldInfo(eYield);
		int iPrice = 0;
		int iValidPrices = 0;
		if (kYieldInfo.getNativeBuyPrice() > 0)
		{
			iPrice += kYieldInfo.getNativeBuyPrice();
			iValidPrices++;
		}
		if (kYieldInfo.getNativeSellPrice() > 0)
		{
			iPrice += kYieldInfo.getNativeSellPrice();
			iValidPrices++;
		}

		if (iPrice > 0)
		{
			//If both buy and sell, use average. Otherwise, use 2/3rd the value.
			iPrice *= 2;
			iPrice /= 2 + iValidPrices;

			iValue += iPrice * 100;
		}
	}
	else
	{
		iValue += m_em_iYieldValuesTimes100.get(eYield);
	}

	iValue *= AI_yieldWeight(eYield);
	iValue /= 100;

	if (bProduce)
	{
		int iWeaponsMultiplier = 100;
		if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
		{
			iWeaponsMultiplier += 50;
		}

		int iGoodsMultiplier = 100;
		if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
		{
			iGoodsMultiplier -= 15;
			iWeaponsMultiplier += 25;
		}
		else if (AI_isStrategy(STRATEGY_CASH_FOCUS))
		{
			iGoodsMultiplier += 50;
		}

		if (AI_isStrategy(STRATEGY_REVOLUTION))
		{
			iGoodsMultiplier -= 15;
			iWeaponsMultiplier += 50;
		}

		switch (eYield)
		{
			case YIELD_FOOD:
				break;
			case YIELD_LUMBER:
			case YIELD_HARDWOOD:
			case YIELD_STONE:
			case YIELD_CLAY:
				iValue *= 100;
				iValue /= iGoodsMultiplier;
				break;
			case YIELD_HEMP:
			case YIELD_FLAX:
			case YIELD_ORE:
			case YIELD_COAL:
			case YIELD_CHAR_COAL:
			case YIELD_PEAT:
			case YIELD_SHEEP:
			case YIELD_GOATS:
			case YIELD_PIGS:
			case YIELD_CATTLE:
			case YIELD_CHICKEN:
			case YIELD_GEESE:
				iValue *= iGoodsMultiplier;
				iValue /= 100;
				break;
			case YIELD_HORSES:
				iValue *= iWeaponsMultiplier;
				iValue /= 100;
				break;
			case YIELD_COCOA_FRUITS:
			case YIELD_COFFEE_BERRIES:
			case YIELD_PEANUTS:
			case YIELD_MILK:
			case YIELD_TOBACCO:
			case YIELD_YERBA_LEAVES:
			case YIELD_COTTON:
			case YIELD_INDIGO:
			case YIELD_LOGWOOD:
			case YIELD_COCHINEAL:
			case YIELD_WOOL:
			case YIELD_GOAT_HIDES:
			case YIELD_PIG_SKIN:
			case YIELD_HIDES:
			case YIELD_DOWNS:
			case YIELD_FUR:
			case YIELD_PREMIUM_FUR:
			case YIELD_RAW_SALT:
			case YIELD_RED_PEPPER:
			case YIELD_VANILLA_PODS:
			case YIELD_BARLEY:
			case YIELD_SUGAR:
			case YIELD_FRUITS:
			case YIELD_GRAPES:
			case YIELD_OLIVES:
			case YIELD_RAPE:
			case YIELD_WHALE_BLUBBER:
			case YIELD_VALUABLE_WOOD:
				iValue *= iGoodsMultiplier;
				iValue /= 100;
				break;
			case YIELD_TRADE_GOODS:
				break;
			case YIELD_ROPE:
			case YIELD_BAKERY_GOODS:
			case YIELD_SAILCLOTH:
				iValue *= iWeaponsMultiplier;
				iValue /= 100;
				break;
			case YIELD_TOOLS:
				break;
			case YIELD_BLADES:
			case YIELD_MUSKETS:
			case YIELD_CANNONS:
			case YIELD_BLACK_POWDER:
				iValue *= iWeaponsMultiplier;
				iValue /= 100;
				break;
			case YIELD_RICE:
			case YIELD_CASSAVA:
			case YIELD_COCA_LEAVES:
			case YIELD_MAPLE_SIRUP:
			case YIELD_KAUTSCHUK:
			case YIELD_WILD_FEATHERS:
			case YIELD_SILVER:
			case YIELD_GOLD:
			case YIELD_GEMS:
			case YIELD_COCOA:
			case YIELD_COFFEE:
			case YIELD_ROASTED_PEANUTS:
			case YIELD_CHEESE:
			case YIELD_CIGARS:
			case YIELD_YERBA_TEA:
			case YIELD_CLOTH:
			case YIELD_COLOURED_CLOTH:
			case YIELD_FESTIVE_CLOTHES:
			case YIELD_WOOL_CLOTH:
			case YIELD_COLOURED_WOOL_CLOTH:
			case YIELD_EVERYDAY_CLOTHES:
			case YIELD_PIG_LEATHER:
			case YIELD_LEATHER:
			case YIELD_GOAT_HIDE_BOOTS:
			case YIELD_PADDED_LEATHER_COATS:
			case YIELD_COATS:
			case YIELD_PREMIUM_COATS:
			case YIELD_SALT:
			case YIELD_SPICES:
			case YIELD_VANILLA:
			case YIELD_CHOCOLATE:
			case YIELD_BEER:
			case YIELD_RUM:
			case YIELD_HOOCH:
			case YIELD_WINE:
			case YIELD_OLIVE_OIL:
			case YIELD_RAPE_OIL:
			case YIELD_WHALE_OIL:
			case YIELD_POTTERY:
			case YIELD_FURNITURE:
			case YIELD_PADDED_FURNITURE:
			case YIELD_FIELD_WORKER_TOOLS:
			case YIELD_HOUSEHOLD_GOODS:
				iValue *= iGoodsMultiplier;
				iValue /= 100;
				break;
			case YIELD_LUXURY_GOODS:
				break;
			case YIELD_HAMMERS:
				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iValue *= 125;
					iValue /= 100;
					if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
					{
						iValue *= 125;
						iValue /= 100;
					}
				}
				// R&R, ray, AI builds stronger - START
				else
				{
					iValue *= 150;
					iValue /= 100;
				}
				// R&R, ray, AI builds stronger - END
				break;
			case YIELD_BELLS:
				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iValue *= 150;
					iValue /= 100;
					if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
					{
						iValue *= 150;
						iValue /= 100;
					}
				}
				break;
			case YIELD_CROSSES:
			case YIELD_CULTURE:
			case YIELD_HEALTH: // R&R, ray, Health - START
			case YIELD_EDUCATION:
			case YIELD_HAPPINESS: // WTP, ray, Happiness - START
				iValue *= 100;
				iValue /= iWeaponsMultiplier;
				break;
			case YIELD_UNHAPPINESS: // WTP, ray, Happiness - START
				iValue = 0; // this Yield is just bad, it is never supposed to be created purolsely
			case YIELD_LAW: // WTP, ray, Crime and Law - START
				iValue *= 100;
				iValue /= iWeaponsMultiplier;
				break;
			case YIELD_CRIME: // WTP, ray, Crime and Law - START
				iValue = 0; // this Yield is just bad, it is never supposed to be created purolsely
			default:
			break;
		}
	}

	iValue *= iAmount;
	iValue /= 100;

	return iValue;
}

void CvPlayerAI::AI_updateYieldValues()
{
	if (!is(CIV_CATEGORY_COLONIAL) && !is(CIV_CATEGORY_KING))
	{
		return;
	}

	PlayerTypes eParent = getParent();

	if (getParent() == NO_PLAYER)
	{
		eParent = getID();
	}

	CvPlayer& kParent = GET_PLAYER(eParent);
	for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_YIELD_TYPES; ++eYield)
	{
		int iValue = 0;
		switch (eYield)
		{
			case YIELD_FOOD:
				iValue += (kParent.getYieldSellPrice(eYield) + kParent.getYieldBuyPrice(eYield)) / 2;
				break;
			// WTP: the average of buy and sell price only for goods the colony mostly buys in Europe or needs itself;
			// everything else is valued at the price Europe pays for it
			case YIELD_LUMBER:
			case YIELD_STONE:
			case YIELD_CLAY:
			case YIELD_TOOLS:
			case YIELD_BLADES:
			case YIELD_MUSKETS:
			case YIELD_CANNONS:
			case YIELD_BLACK_POWDER:
			case YIELD_HORSES:
			case YIELD_HARDWOOD:
			case YIELD_TRADE_GOODS:
			case YIELD_ROPE:
			case YIELD_SAILCLOTH:
			case YIELD_BAKERY_GOODS:
			case YIELD_LUXURY_GOODS:
				iValue += (kParent.getYieldSellPrice(eYield) + kParent.getYieldBuyPrice(eYield)) / 2;
				break;
			case YIELD_HEMP:
			case YIELD_FLAX:
			case YIELD_ORE:
			case YIELD_COAL:
			case YIELD_CHAR_COAL:
			case YIELD_PEAT:
			case YIELD_SHEEP:
			case YIELD_GOATS:
			case YIELD_PIGS:
			case YIELD_CATTLE:
			case YIELD_CHICKEN:
			case YIELD_GEESE:
				iValue += kParent.getYieldBuyPrice(eYield);
				break;
			case YIELD_COCOA_FRUITS:
			case YIELD_COFFEE_BERRIES:
			case YIELD_PEANUTS:
			case YIELD_MILK:
			case YIELD_TOBACCO:
			case YIELD_YERBA_LEAVES:
			case YIELD_COTTON:
			case YIELD_INDIGO:
			case YIELD_LOGWOOD:
			case YIELD_COCHINEAL:
			case YIELD_WOOL:
			case YIELD_GOAT_HIDES:
			case YIELD_PIG_SKIN:
			case YIELD_HIDES:
			case YIELD_DOWNS:
			case YIELD_FUR:
			case YIELD_PREMIUM_FUR:
			case YIELD_RAW_SALT:
			case YIELD_RED_PEPPER:
			case YIELD_VANILLA_PODS:
			case YIELD_BARLEY:
			case YIELD_SUGAR:
			case YIELD_FRUITS:
			case YIELD_GRAPES:
			case YIELD_OLIVES:
			case YIELD_RAPE:
			case YIELD_WHALE_BLUBBER:
			case YIELD_VALUABLE_WOOD:
				iValue += kParent.getYieldBuyPrice(eYield);
				break;
			case YIELD_RICE:
			case YIELD_CASSAVA:
			case YIELD_COCA_LEAVES:
			case YIELD_MAPLE_SIRUP:
			case YIELD_KAUTSCHUK:
			case YIELD_WILD_FEATHERS:
			case YIELD_SILVER:
			case YIELD_GOLD:
			case YIELD_GEMS:
			case YIELD_COCOA:
			case YIELD_COFFEE:
			case YIELD_ROASTED_PEANUTS:
			case YIELD_CHEESE:
			case YIELD_CIGARS:
			case YIELD_YERBA_TEA:
			case YIELD_CLOTH:
			case YIELD_COLOURED_CLOTH:
			case YIELD_FESTIVE_CLOTHES:
			case YIELD_WOOL_CLOTH:
			case YIELD_COLOURED_WOOL_CLOTH:
			case YIELD_EVERYDAY_CLOTHES:
			case YIELD_PIG_LEATHER:
			case YIELD_LEATHER:
			case YIELD_GOAT_HIDE_BOOTS:
			case YIELD_PADDED_LEATHER_COATS:
			case YIELD_COATS:
			case YIELD_PREMIUM_COATS:
			case YIELD_SALT:
			case YIELD_SPICES:
			case YIELD_VANILLA:
			case YIELD_CHOCOLATE:
			case YIELD_BEER:
			case YIELD_RUM:
			case YIELD_HOOCH:
			case YIELD_WINE:
			case YIELD_OLIVE_OIL:
			case YIELD_RAPE_OIL:
			case YIELD_WHALE_OIL:
			case YIELD_POTTERY:
			case YIELD_FURNITURE:
			case YIELD_PADDED_FURNITURE:
			case YIELD_FIELD_WORKER_TOOLS:
			case YIELD_HOUSEHOLD_GOODS:
				iValue += kParent.getYieldBuyPrice(eYield);
				break;
			case YIELD_HAMMERS:
			case YIELD_BELLS:
			case YIELD_CROSSES:
			case YIELD_CULTURE:
			case YIELD_HEALTH:
			case YIELD_EDUCATION:
			case YIELD_HAPPINESS: // WTP, ray, Happiness - START
			case YIELD_UNHAPPINESS: // WTP, ray, Happiness - START
			case YIELD_LAW: // WTP, ray, Crime and Law - START
			case YIELD_CRIME: // WTP, ray, Crime and Law - START
				break;
			default:
				FAssert(false);
		}

		m_em_iYieldValuesTimes100.set(eYield, 100 * iValue);
	}
	int iCrossValue = m_em_iYieldValuesTimes100.get(YIELD_FOOD) * getGrowthThreshold(1) / (50 + immigrationThreshold() * GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getGrowthPercent() / 100);
	iCrossValue /= 2;

	//Crosses
	m_em_iYieldValuesTimes100.set(YIELD_CROSSES, std::max(m_em_iYieldValuesTimes100.get(YIELD_CROSSES), iCrossValue));

	//The function is quite simple. Iterate over every citizen which has input yield.
	//Calculate the value of their output yield, and assign half of that to the input.
	if (!isHuman())
	{
		int iLoop;
		CvCity* pLoopCity;
		for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
		{
			for (int i = 0; i < pLoopCity->getPopulation(); ++i)
			{
				CvUnit* pLoopUnit = pLoopCity->getPopulationUnitByIndex(i);

				ProfessionTypes eProfession = pLoopUnit->getProfession();
				if (eProfession != NO_PROFESSION)
				{
					CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
					if (kProfession.getYieldsConsumed(0) != NO_YIELD)
					{
						int iValue = 0;
						// R&R, ray , MYCP partially based on code of Aymerick - START
						FAssert(kProfession.getYieldsProduced(0) != NO_YIELD);//damn welfware cheats.
						int iInput = pLoopCity->getProfessionInput(eProfession, pLoopUnit);
						int iOutput = pLoopCity->getProfessionOutput(eProfession, pLoopUnit);

						int iProfit = (m_em_iYieldValuesTimes100.get((YieldTypes)kProfession.getYieldsProduced(0)) * iOutput);
						// R&R, ray , MYCP partially based on code of Aymerick - END
						// R&R, ray, fix on Zero Division
						if (iInput == 0)
						{
							iInput = 1;
						}
						int iInputValue = iProfit / (2 * iInput); //Assign 50% of the yield value to the input.
						m_em_iYieldValuesTimes100.set((YieldTypes)kProfession.getYieldsConsumed(0), std::max(iInputValue, m_em_iYieldValuesTimes100.get((YieldTypes)kProfession.getYieldsConsumed(0))));
					}
				}


				// R&R, ray, code changes for Ideal Profession - START
				// ProfessionTypes eIdealProfesion = AI_idealProfessionForUnit(pLoopUnit->getUnitType());
				UnitClassTypes eUnitClassType = (UnitClassTypes)pLoopUnit->getUnitClassType();

				// we need 2 possible ideal professions for professions in buildings
				ProfessionTypes eIdealProfession = NO_PROFESSION;
				ProfessionTypes eSecondIdealProfession = NO_PROFESSION;

				for (int iI = 0; iI < GC.getNumProfessionInfos(); ++iI)
				{
					ProfessionTypes eLoopProfession = (ProfessionTypes)iI;
					CvProfessionInfo& kProfession = GC.getProfessionInfo(eLoopProfession);

					if (kProfession.isCitizen() && !kProfession.isWorkPlot()) // we are only interested in professions in buildings
					{
						if(eIdealProfession == NO_PROFESSION)
						{
							if ((UnitClassTypes)kProfession.LbD_getExpert() == eUnitClassType)
							{
								eIdealProfession = eLoopProfession;
							}
						}
						else
						{
							if ((UnitClassTypes)kProfession.LbD_getExpert() == eUnitClassType)
							{
								eSecondIdealProfession = eLoopProfession;
								break;
							}
						}
					}
				}


				if (eIdealProfession != NO_PROFESSION && eIdealProfession != eProfession)
				{
					CvProfessionInfo& kIdealPro = GC.getProfessionInfo(eIdealProfession);
					YieldTypes eYieldConsumed = (YieldTypes)kIdealPro.getYieldsConsumed(0);
					if (eYieldConsumed != NO_YIELD)
					{
						// R&R, ray , MYCP partially based on code of Aymerick - START
						YieldTypes eYieldProduced = (YieldTypes)kIdealPro.getYieldsProduced(0);
						FAssert(kIdealPro.getYieldsProduced(0) != NO_YIELD);
						// R&R, ray , MYCP partially based on code of Aymerick - END
						int iInputValue = m_em_iYieldValuesTimes100.get(eYieldProduced) / 2;
						m_em_iYieldValuesTimes100.set(eYieldConsumed, std::max(iInputValue, m_em_iYieldValuesTimes100.get(eYieldConsumed)));
					}

					if (eSecondIdealProfession != NO_PROFESSION && eSecondIdealProfession != eProfession)
					{

						CvProfessionInfo& kIdealPro2 = GC.getProfessionInfo(eSecondIdealProfession);
						YieldTypes eYieldConsumed2 = (YieldTypes)kIdealPro2.getYieldsConsumed(0);
						if (eYieldConsumed2 != NO_YIELD)
						{
							// R&R, ray , MYCP partially based on code of Aymerick - START
							YieldTypes eYieldProduced2 = (YieldTypes)kIdealPro2.getYieldsProduced(0);
							FAssert(kIdealPro2.getYieldsProduced(0) != NO_YIELD);
							// R&R, ray , MYCP partially based on code of Aymerick - END
							int iInputValue = m_em_iYieldValuesTimes100.get(eYieldProduced2) / 2;
							m_em_iYieldValuesTimes100.set(eYieldConsumed2, std::max(iInputValue, m_em_iYieldValuesTimes100.get(eYieldConsumed2)));
						}
					}
				}
				// R&R, ray, code changes for Ideal Profession - END
			}
		}
	}
}

int CvPlayerAI::AI_transferYieldValue(const IDInfo target, YieldTypes eYield, int iAmount)
{
	FAssertMsg(eYield > NO_YIELD, "Index out of bounds");
	FAssertMsg(eYield < NUM_YIELD_TYPES, "Index out of bounds");

	const IDInfo kEurope(getID(), CvTradeRoute::EUROPE_CITY_ID);
	CvCity* pCity = ::getCity(target);

	int iValue = 0;
	if (pCity != NULL)
	{
		int iStored = pCity->getYieldStored(eYield);
		int iMaxCapacity = (eYield == YIELD_FOOD) ? pCity->growthThreshold() : pCity->getMaxYieldCapacity();
		// WTP, ray, just to be save e.g. if getMaxYieldCapacity is really 0 - START
		if (iMaxCapacity == 0)
		{
			iMaxCapacity = 300;
		}
		// WTP, ray, just to be save e.g. if getMaxYieldCapacity is really 0 - END

		// transport feeder - start - Nightinggale
		//int iMaintainLevel = pCity->getMaintainLevel(eYield);
		int iMaintainLevel = pCity->getAutoMaintainThreshold(eYield);
		// transport feeder - end - Nightinggale
		int iTotalStored = pCity->getTotalYieldStored();
		// R&R, ray, improvement
		if (iTotalStored > iMaxCapacity / 2)
		{
			int iCargoYields = 0;
			for (int iYield = 1; iYield < NUM_YIELD_TYPES; iYield++)//YIELD_FOOD
			{
				if ((pCity->getYieldStored((YieldTypes)iYield) > 0) && (GC.getYieldInfo(eYield).isCargo()))
					{iCargoYields++;}
			}
			if (iCargoYields < 1)
			{
				iCargoYields = 1;
			}
			iMaintainLevel /= iCargoYields;
		}
		FAssert(iMaxCapacity > 0);
		if (iAmount < 0) // Loading
		{
			int iSurplus = iStored - iMaintainLevel;
			if (iSurplus > 0)
			{
				iValue = std::min(iSurplus, -iAmount);

				iValue *= 50 + ((100 * iStored) / std::max(1, iMaxCapacity));
				int iMax = iMaxCapacity * 9 / 10;

				// WTP, ray, even if it should never happen, let us prevent iMax being 0 if iMaxCapacity for some reason is 1 above - START
				if (iMax == 0)
				{
					iMax = 270;
				}
				// WTP, ray, even if it should never happen, let us prevent iMax being 0 if iMaxCapacity for some reason is 1 above - END

				if (iTotalStored >= iMax)
				{
					iValue *= 125 + ((100 * iMax) / iMax);
					iValue /= 100;
				}
			}
		}
		else //Unloading
		{
			if (iAmount > 0)
			{
				iTotalStored += (pCity->AI_getTransitYield(eYield) * 75) / 100;
				iStored += (pCity->AI_getTransitYield(eYield) * 75) / 100;
				// Erik: Adding this since I suspect a bug may cause this variable to be negative
				FAssertMsg(iStored >= 0, "iStored expected to be >= 0");
			}

			iValue = iAmount * 100;
			int iMax = iMaxCapacity * 9 / 10;

			// WTP, ray, even if it should never happen, let us prevent iMax being 0 if iMaxCapacity for some reason is 1 above - START
			if (iMax == 0)
			{
				iMax = 270;
			}
			// WTP, ray, even if it should never happen, let us prevent iMax being 0 if iMaxCapacity for some reason is 1 above - END

			if (iTotalStored > iMax)
			{
				iValue *= 10;
				iValue /= 100 + ((100 * (iTotalStored - iMax)) / iMax);
			}
			else
			{
				iValue *=  std::max(10, 10 + (100 * (iMaxCapacity - pCity->getTotalYieldStored())) / std::max(1, 10 + iMaxCapacity));
				iValue /= 100;
			}

			if (iStored < iMaintainLevel && iMaintainLevel > 0)
			{
				iValue *= 125 + 75 * (iMaintainLevel - iStored) / iMaintainLevel;
				iValue /= 100;
			}

			/*
			int iProductionNeeded = 0;
			UnitTypes eUnit = pCity->getProductionUnit();
			if (eUnit != NO_UNIT)
			{
				iProductionNeeded = std::max(iProductionNeeded, pCity->getYieldProductionNeeded(eUnit, eYield));
			}
			BuildingTypes eBuilding = pCity->getProductionBuilding();
			if (eBuilding != NO_BUILDING)
			{
				iProductionNeeded = std::max(iProductionNeeded, pCity->getYieldProductionNeeded(eBuilding, eYield));
			}
			*/
			// production cache - start - Nightinggale
			int iProductionNeeded = pCity->getProductionNeeded(eYield);
			// production cache - end - Nightinggale

			// Erik: Production needed can be 0, so we have to deal with it to avoid dividing by zero
			if (iStored < iProductionNeeded && iProductionNeeded > 0)
			{
				iValue *= 150 + 100 * (iProductionNeeded - iStored) / iProductionNeeded;
				iValue /= 100;
				// transport feeder - start - Nightinggale
				iValue *= 2;
				// transport feeder - end - Nightinggale
			}

			// transport feeder - start - Nightinggale
			if (pCity->getImportsMaintain(eYield))
			{
				iValue *= 2;
			}
			// transport feeder - end - Nightinggale
		}
	}
	else if (target == kEurope)
	{
		if (iAmount < 0) //Loading
		{
			iValue = -iAmount;
		}
		else
		{
			iValue = iAmount;
		}
	}
	else
	{
		FAssertMsg(false, "Invalid Route Destination");
	}

	return iValue;
}

int CvPlayerAI::AI_countYieldWaiting()
{
	int iCount = 0;
	int iLoop;
	CvCity* pLoopCity;

	int iUnitSize = GC.getGameINLINE().getCargoYieldCapacity();
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		// Custom_House_Mod Start
		if (pLoopCity->isBestPortCity())
		// Custom_House_Mod End
		{
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
			{
				YieldTypes eLoopYield = (YieldTypes)iYield;
				int iTotal = pLoopCity->getYieldStored(eLoopYield);
				if (iTotal > 0)
				{
					if (pLoopCity->AI_shouldExportYield(eLoopYield))
					{
						iCount += (iTotal + iUnitSize / 2) / iUnitSize;
					}
				}
			}
		}
	}

	return iCount;
}

int CvPlayerAI::AI_highestYieldAdvantage(YieldTypes eYield)
{
	int iBestValue = 0;
	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		iBestValue = std::max(iBestValue, pLoopCity->AI_getYieldAdvantage(eYield));
	}
	return iBestValue;
}

//Big function to do everything.
void CvPlayerAI::AI_manageEconomy()
{
	if (getNumCities() == 0)
	{
		return;
	}

	bool bAtWar = (GET_TEAM(getTeam()).getAnyWarPlanCount() > 0);
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		for (int iI = 0; iI < NUM_YIELD_TYPES; iI++)
		{
			int iWeight = 100;
			YieldTypes eLoopYield = (YieldTypes)iI;

			if (isNative())
			{
				iWeight = 60 + GC.getGame().getSorenRandNum(40, "AI Native Yield Value Randomization 1");

				if (pLoopCity->getTeachUnitClass() != NO_UNITCLASS)
				{
					UnitTypes eUnit = (UnitTypes)GC.getUnitClassInfo(pLoopCity->getTeachUnitClass()).getDefaultUnitIndex();
					if (eUnit != NO_UNIT)
					{
						iWeight += GC.getUnitInfo(eUnit).getYieldModifier(eLoopYield);
					}
				}

				if (eLoopYield == YIELD_FOOD)
				{
					iWeight += bAtWar ? 20 : 0;
				}
				else if (eLoopYield == YIELD_ORE || eLoopYield == YIELD_COAL || eLoopYield ==YIELD_CHAR_COAL || eLoopYield == YIELD_PEAT)
				{
					iWeight *= 20 + GC.getGame().getSorenRandNum(80, "AI Native Yield Value Randomization 3");
					iWeight /= 100;
				}
				else if (eLoopYield == YIELD_HORSES)
				{
					iWeight += bAtWar ? 20 : 0;
				}

				int iTotalStored = countTotalYieldStored(eLoopYield);
				int iMaxStored = getNumCities() * GC.getGameINLINE().getCargoYieldCapacity();
				iMaxStored *= GC.getYieldInfo(eLoopYield).getNativeConsumptionPercent();

				int iModifier = 1 + std::max(10, 100 - (100 * iTotalStored) / iMaxStored);
				iWeight *= iModifier;
				iWeight /= 100;
			}

			int iEmphasize = 0;
			for (int i = 0; i < GC.getNumEmphasizeInfos(); ++i)
			{
				if (pLoopCity->AI_isEmphasize((EmphasizeTypes)i))
				{
					CvEmphasizeInfo& kEmphasize = GC.getEmphasizeInfo((EmphasizeTypes)i);
					int iValue = kEmphasize.getYieldChange(eLoopYield);
					if (iValue != 0)
					{
						iEmphasize = iValue;
						break;
					}
				}
			}

			iWeight *= 100 + 133 * std::max(0, iEmphasize);
			iWeight /= 100 + 166 * std::max(0, -iEmphasize);

			pLoopCity->AI_setYieldOutputWeight(eLoopYield, iWeight);
		}
	}

	//Calculate Comparative Advantage in producing various yields.

	//For averages.
	int aiBestYield[NUM_YIELD_TYPES];
	int aiWorstYield[NUM_YIELD_TYPES];

	for (int iYield = 0; iYield < NUM_YIELD_TYPES; iYield++)
	{
		aiBestYield[iYield] = 0;
		aiWorstYield[iYield] = MAX_INT;
	}

	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		for (int i = 0; i < GC.getNumProfessionInfos(); ++i)
		{
			CvProfessionInfo& kProfession = GC.getProfessionInfo((ProfessionTypes)i);

			if (kProfession.isCitizen())
			{
				// R&R, ray , MYCP partially based on code of Aymerick - START
				YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
				// R&R, ray , MYCP partially based on code of Aymerick - END
				if (eYield != NO_YIELD)
				{
					int iOutput = pLoopCity->getProfessionOutput((ProfessionTypes)i, NULL);
					pLoopCity->AI_setYieldAdvantage(eYield, iOutput);

					aiBestYield[eYield] = std::max(aiBestYield[eYield], iOutput);
					aiWorstYield[eYield] = std::min(aiWorstYield[eYield], iOutput);
				}
			}
		}
	}

	if (!isNative())
	{
		for (CvCity*pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
		{
			int iAdvantageCount = 0;
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
			{
				YieldTypes eYield = (YieldTypes)iYield;
				if (aiBestYield[eYield] > 0)
				{
					int iAdvantage = (100 * pLoopCity->AI_getYieldAdvantage(eYield)) / aiBestYield[eYield];
					if (aiBestYield[eYield] == aiWorstYield[eYield])
					{
						iAdvantage *= 99;
						iAdvantage /= 100;
					}
					if (iAdvantage == 100)
					{
						iAdvantageCount++;
					}
					pLoopCity->AI_setYieldAdvantage(eYield, iAdvantage);
				}
			}
			pLoopCity->AI_setTargetSize(std::max(pLoopCity->getHighestPopulation(), 2 + iAdvantageCount * 3));
		}
	}

}

CvPlot* CvPlayerAI::AI_getTerritoryCenter() const
{
	if (getNumCities() == 0)
	{
		return NULL;
	}

	CvCity* pLoopCity;
	int iLoop;

	int iTotalX = 0;
	int iTotalY = 0;
	int iTotalWeight = 0;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		int iWeight = 1 + 10 * pLoopCity->AI_getTargetSize();
		if (pLoopCity->area()->getCitiesPerPlayer(getID()) == 1)
		{
			iWeight /= 2;
		}
		iWeight = std::max(1, iWeight);

		iTotalX += pLoopCity->getX_INLINE() * iWeight;
		iTotalY += pLoopCity->getY_INLINE() * iWeight;
		iTotalWeight += iWeight;
	}

	if (iTotalWeight == 0)
	{
		return NULL;
	}

	iTotalX += iTotalWeight / 2;
	iTotalY += iTotalWeight / 2;

	return GC.getMap().plotINLINE(iTotalX / iTotalWeight, iTotalY / iTotalWeight);
}

int CvPlayerAI::AI_getTerritoryRadius() const
{
	return 10;
}

void CvPlayerAI::AI_createNatives()
{
	AI_createNativeCities();
	if (getNumCities() == 0)
	{
		return;
	}

	int iLoop;
	CvCity* pLoopCity;
	int iCount = 0;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{

		int iExtraPop = std::max(3, ((getNumCities() - iCount) * 3) / getNumCities());

		if (iCount == 0)
		{
			iExtraPop++;
		}

		// Require a certain minimum food surplus.
		while ((pLoopCity->AI_getFoodGatherable(iExtraPop, GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION) / 2)
			< GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION * iExtraPop)
		{
			iExtraPop--;

			if (iExtraPop <= 3)
			{
				iExtraPop = 3;
				break;
			}
		}

		pLoopCity->changePopulation(iExtraPop);
		pLoopCity->AI_setTargetSize(pLoopCity->getPopulation());

		iCount++;

		int iBraveCount = pLoopCity->getPopulation() + 2;
		for (int iI = 0; iI < iBraveCount; iI++)
		{
			UnitTypes eBrave = AI_bestUnit(UNITAI_DEFENSIVE);
			if (eBrave != NO_UNIT)
			{
				OOS_LOG_3("AI create natives", CvString(pLoopCity->getName()).c_str(), getTypeStr(eBrave));
				initUnit(eBrave, GC.getUnitInfo(eBrave).getDefaultProfession(), pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE());
			}
		}
	}

	for (int iPass = 0; iPass < 2; ++iPass)
	{
		//Now provide some starting yield stockpiles.
		AI_manageEconomy();
		int aiYields[NUM_YIELD_TYPES];
		for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
		{
			pLoopCity->AI_assignWorkingPlots();
			pLoopCity->calculateNetYields(aiYields);

			for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
			{
				YieldTypes eYield = (YieldTypes) iYield;
				if (GC.getYieldInfo(eYield).isCargo())
				{
					pLoopCity->changeYieldStored(eYield, (aiYields[eYield] * (50 + GC.getGameINLINE().getSorenRandNum(50, "AI starting yields"))) / 10);
				}
			}
		}
	}
}

void CvPlayerAI::AI_createNativeCities()
{
	while (true)
	{
		int iBestValue = 0;
		CvPlot* pBestPlot = NULL;
		for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
		{
			CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
			int iValue = AI_foundValueNative(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE(), false);
			if (iValue > iBestValue)
			{
				iBestValue = iValue;
				pBestPlot = pLoopPlot;
			}
		}
		if (pBestPlot == NULL)
		{
			break;
		}

		CvCity* pCity = pBestPlot->getPlotCity();
		if (pCity != NULL)
		{
			FAssertMsg(false, "City already exists!");
			break;
		}

		found(pBestPlot->coord());
		pCity = pBestPlot->getPlotCity();
		if (pCity == NULL)
		{
			FAssertMsg(false, "Cannot found city!");
			break;
		}
		pCity->setCulture(getID(), 1, true);
	}

	for(int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
	{
		CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
		int iCulture = 0;
		int iOursCount = 0;
		if (pLoopPlot->isCityRadius())
		{
			if (pLoopPlot->getOwnerINLINE() == getID())
			{
				iCulture += 90;//XMLize and adjust for game speed...
				if (pLoopPlot->getWorkingCity() != NULL)
				{
					iCulture += 10 * pLoopPlot->getWorkingCity()->getPopulation();
				}
			}
		}
		else
		{
			for (int iJ = 0; iJ < NUM_DIRECTION_TYPES; iJ++)
			{
				CvPlot* pDirectionPlot = plotDirection(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE(), (DirectionTypes)iJ);
				if (pDirectionPlot != NULL)
				{
					if (pDirectionPlot->isCityRadius() && pDirectionPlot->getOwnerINLINE() == getID())
					{
						iOursCount++;
					}
				}
			}

			int iRand = GC.getGame().getSorenRandNum(2, "AI plot");
			if ((iOursCount >= 3 + iRand) && (pLoopPlot->getOwnerINLINE() == getID() || !pLoopPlot->isOwned()))
			{
				iCulture += 40 + 10 * iOursCount;
			}
			else if (pLoopPlot->getOwnerINLINE() == getID())
			{
				pLoopPlot->setOwner(NO_PLAYER, false);
			}
		}

		int iBestYield = 0;
		int iTotalYield = 0;
		if (iCulture > 0)
		{
			for (int i = 0; i < NUM_YIELD_TYPES; i++)
			{
				YieldTypes eYield = (YieldTypes)i;
				iBestYield = std::max(iBestYield, pLoopPlot->getYield(eYield));
				iTotalYield += pLoopPlot->getYield(eYield);
			}

			iCulture += 10 * iBestYield + (5 * (iTotalYield - iBestYield));

			if (pLoopPlot->getFeatureType() != NO_FEATURE)
			{
				iCulture += pLoopPlot->getYield(YIELD_LUMBER) * 5;
			}

			iCulture *= 100 + GC.getGameINLINE().getSorenRandNum(40, "Native Plot Culture");
			iCulture /= 100;
			pLoopPlot->setCulture(getID(), iCulture, true);
		}
	}


	for(int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
	{
		CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);


		if (pLoopPlot->getOwnerINLINE() == getID())
		{
			int iOursCount = 0;
			for (int iJ = 0; iJ < NUM_DIRECTION_TYPES; iJ++)
			{
				CvPlot* pDirectionPlot = plotDirection(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE(), (DirectionTypes)iJ);
				if (pDirectionPlot != NULL)
				{
					if (pDirectionPlot->getOwnerINLINE() == getID())
					{

						if (pLoopPlot->getX_INLINE() == pDirectionPlot->getX_INLINE() || pLoopPlot->getY_INLINE() == pDirectionPlot->getY_INLINE())
						{
							iOursCount++;
						}
					}
				}
			}
			if (iOursCount <= 1)
			{
				pLoopPlot->setCulture(getID(), 0, true);
			}
		}
	}
}

bool CvPlayerAI::AI_isKing()
{
	return isEurope();
}

CvPlot* CvPlayerAI::AI_getImperialShipSpawnPlot()
{
	CvPlot* pBestPlot = NULL;
	int iBestValue = 0;

	std::deque<bool> zoneAllowable(GC.getNumEuropeInfos(), false);
	bool bNoneAllowable = true;
	for (int iI = 0; iI < MAX_PLAYERS; ++iI)
	{
		CvPlayer& kPlayer = GET_PLAYER((PlayerTypes)iI);
		if (kPlayer.isAlive() && (kPlayer.getParent() == getID()))
		{
			if (kPlayer.getStartingPlot() != NULL)
			{
				if (kPlayer.getStartingPlot()->getEurope() != NO_EUROPE)
				{
					zoneAllowable[kPlayer.getStartingPlot()->getEurope()] = true;
					bNoneAllowable = false;
				}
			}
		}
	}

	CvTeamAI& kTeam = GET_TEAM(getTeam());

	CvPlot* pTargetPlot = NULL;

	if (AI_isStrategy(STRATEGY_CONCENTRATED_ATTACK))
	{
		pTargetPlot = GC.getMap().plotByIndexINLINE(AI_getStrategyData(STRATEGY_CONCENTRATED_ATTACK));
	}

	for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
	{
		CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);

		EuropeTypes eEurope = pLoopPlot->getEurope();
		if (eEurope != NO_EUROPE)
		{
			if (bNoneAllowable || zoneAllowable[eEurope])
			{
				int iEnemyDistance = kTeam.AI_enemyCityDistance(pLoopPlot);
				int iValue = (bNoneAllowable || (!bNoneAllowable && iEnemyDistance == -1)) ? 100 : (10000 / (std::max(1, iEnemyDistance - 2)));

				int iLocation = 50;
				switch ((CardinalDirectionTypes)GC.getEuropeInfo(eEurope).getCardinalDirection())
				{
				case CARDINALDIRECTION_EAST:
				case CARDINALDIRECTION_WEST:
					iLocation = (100 * pLoopPlot->getY_INLINE() + 50) / GC.getMap().getGridHeightINLINE();
					break;
				case CARDINALDIRECTION_NORTH:
				case CARDINALDIRECTION_SOUTH:
					iLocation = (100 * pLoopPlot->getX_INLINE() + 50) / GC.getMap().getGridWidthINLINE();
					break;
				default:
					break;
				}

				if (AI_isStrategy(STRATEGY_DISTRIBUTED_ATTACK))
				{
					iValue /= 1 + AI_plotTargetMissionAIs(pLoopPlot, MISSIONAI_ASSAULT, NULL, 1);

					int iModifier = 2 * std::abs(iLocation - 50);

					if (iModifier > 95)
					{
						iModifier = 1;
					}
					else if (iModifier > 80)
					{
						iModifier *= (100 - iModifier);
						iModifier /= 100;
					}
					else
					{
						iModifier += 10;
					}

					iValue *= iModifier;
					iValue /= 100;
				}
				if (AI_isStrategy(STRATEGY_CONCENTRATED_ATTACK))
				{
					iValue /= 1 + AI_plotTargetMissionAIs(pLoopPlot, MISSIONAI_ASSAULT, NULL, 1);

					if (pTargetPlot == NULL)
					{
						int iModifier = 100 - 2 * std::abs(iLocation - 50);

						if (iModifier < 10)
						{
							iModifier = 1;
						}
						iValue *= iModifier;
						iValue /= 100;
					}
					else
					{
						iValue *= 10;
						iValue /= std::max(1, stepDistance(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE(), pTargetPlot->getX_INLINE(), pTargetPlot->getY_INLINE()) - 3);
					}
				}

				iValue *= 25 + GC.getGameINLINE().getSorenRandNum(75, "AI best imperial ship spawn plot");

				if (iValue > iBestValue)
				{
					iBestValue = iValue;
					pBestPlot = pLoopPlot;
				}
			}
		}
	}

	FAssert(pBestPlot != NULL);

	return pBestPlot;
}

void CvPlayerAI::AI_addUnitToMoveQueue(CvUnit* pUnit)
{
	if (std::find(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), pUnit->getID()) == m_unitPriorityHeap.end())
	{
		m_unitPriorityHeap.push_back(pUnit->getID());
		std::push_heap(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), CvShouldMoveBefore(getID()));
	}
	else
	{
		std::make_heap(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), CvShouldMoveBefore(getID()));
	}
}

void CvPlayerAI::AI_removeUnitFromMoveQueue(CvUnit* pUnit)
{
	std::vector<int>::iterator it;
	it = std::find(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), pUnit->getID());
	if (it != m_unitPriorityHeap.end())
	{
		m_unitPriorityHeap.erase(it);
		std::make_heap(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), CvShouldMoveBefore(getID()));
	}
}

void CvPlayerAI::AI_verifyMoveQueue()
{
	std::vector<int>::iterator it = std::partition(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), CvShouldUnitMove(getID()));
	m_unitPriorityHeap.erase(it, m_unitPriorityHeap.end());
	std::make_heap(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), CvShouldMoveBefore(getID()));
}

CvUnit* CvPlayerAI::AI_getNextMoveUnit()
{
	CvUnit* pUnit = getUnit(m_unitPriorityHeap.front());
	std::pop_heap(m_unitPriorityHeap.begin(), m_unitPriorityHeap.end(), CvShouldMoveBefore(getID()));
	m_unitPriorityHeap.pop_back();
	return pUnit;
}

int CvPlayerAI::AI_highestProfessionOutput(ProfessionTypes eProfession, const CvCity* pIgnoreCity)
{
	int iLoop;
	CvCity* pLoopCity;

	int iBestYield = 0;

	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity != pIgnoreCity)
		{
			iBestYield = std::max(iBestYield, pLoopCity->getProfessionOutput(eProfession, NULL));

			//Also consider buildings under construction.
			CLLNode<OrderData>* pOrderNode = pLoopCity->headOrderQueueNode();

			while (pOrderNode != NULL)
			{
				switch (pOrderNode->m_data.getType())
				{
				case ORDER_TRAIN:
				case ORDER_CONVINCE:
					break;

				case ORDER_CONSTRUCT:
					{
						const BuildingTypes eBuilding = pOrderNode->m_data.building();
						if (eBuilding != NO_BUILDING)
						{
							if (GC.getProfessionInfo(eProfession).getSpecialBuilding() == GC.getBuildingInfo(eBuilding).getSpecialBuildingType())
							{
								iBestYield = std::max(iBestYield, GC.getBuildingInfo(eBuilding).getProfessionOutput());
							}
						}
						break;
					}

				default:
					FAssertMsg(false, "pOrderNode->m_data.eOrderType failed to match a valid option");
					break;
				}
				pOrderNode = pLoopCity->nextOrderQueueNode(pOrderNode);
			}
		}
	}

	return iBestYield;
}

CvCity* CvPlayerAI::AI_bestCityForBuilding(BuildingTypes eBuilding)
{
	int iBestValue = 0;
	CvCity* pBestCity = NULL;

	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (!pLoopCity->isHasConceptualBuilding(eBuilding))
		{
			int iValue = pLoopCity->AI_buildingValue(eBuilding);
			if (iValue > iBestValue)
			{
				pBestCity = pLoopCity;
				iBestValue = iValue;
			}
		}
	}

	return pBestCity;
}

UnitTypes CvPlayerAI::AI_bestUnit(UnitAITypes eUnitAI, CvArea* pArea)
{
	FAssertMsg(eUnitAI != NO_UNITAI, "UnitAI is not assigned a valid value");

	int iBestValue = 0;
	UnitTypes eBestUnit = NO_UNIT;

	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		UnitTypes eLoopUnit = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eUnitClass);

		if (eLoopUnit != NO_UNIT)
		{
			if (eUnitAI == NO_UNITAI || GC.getUnitInfo(eLoopUnit).getDefaultUnitAIType() == eUnitAI)
			{

				int iValue = AI_unitValue(eLoopUnit, eUnitAI, pArea);

				if (iValue > 0)
				{
					iValue *= (GC.getGameINLINE().getSorenRandNum(40, "AI Best Unit") + 100);
					iValue /= 100;

					iValue *= (getNumCities() + 2);
					iValue /= (getUnitClassCountPlusMaking(eUnitClass) + getNumCities() + 2);

					FAssert((MAX_INT / 1000) > iValue);
					iValue *= 1000;

					iValue = std::max(1, iValue);

					if (iValue > iBestValue)
					{
						iBestValue = iValue;
						eBestUnit = eLoopUnit;
					}
				}
			}
		}
	}

	return eBestUnit;
}

int CvPlayerAI::AI_desiredCityCount()
{
	bool bDense = AI_isStrategy(STRATEGY_DENSE_CITY_SPACING);
	int iCount = 0;

	// WTP, Schmiddie, improved colonial AI expansion - START
	bool bColonial = is(CIV_CATEGORY_COLONIAL);
	// WTP, Schmiddie, improved colonial AI expansion - END

	int iStep = 4;

	// WTP: only count colonists in the colonies and on the map, not those waiting in Europe
	int iTotal = 0;
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		iTotal += pLoopCity->getPopulation();
	}
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getUnitInfo().isFound() && pLoopUnit->isOnMap())
		{
			++iTotal;
		}
	}
	if (AI_isStrategy(STRATEGY_DENSE_CITY_SPACING))
	{
		iTotal *= 133;
		iTotal /= 100;
	}
	// WTP: colonial AIs need about 57% more population per wanted colony (43% before, raised by 10%)
	if (bColonial)
	{
		iTotal *= 100;
		iTotal /= 157;
	}

	while (iTotal > 0)
	{
		iTotal -= iStep;

		// WTP, Schmiddie, improved colonial AI expansion - START
		if (bColonial)
		{
			iStep += 2 + std::max(0, iCount - 6);
		}
		else
		{
			iStep += 3 + std::max(0, iCount - 4);
		}
		// WTP, Schmiddie, improved colonial AI expansion - END

		iCount++;
	}

	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity->getPopulation() >= 20)
		{
			iCount++;
		}
	}

	return std::max(1, iCount);
}

int CvPlayerAI::AI_professionBasicValue(ProfessionTypes eProfession, UnitTypes eUnit, CvCity* pCity)
{
	CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
	// R&R, ray , MYCP partially based on code of Aymerick - START
	YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
	// R&R, ray , MYCP partially based on code of Aymerick - END
	if (eYield == NO_YIELD)
	{
		return 0;
	}

	CvUnitInfo& kUnit = GC.getUnitInfo(eUnit);
	int iBestValue = 0;

	if (kProfession.isCitizen())
	{
		if (!kProfession.isWorkPlot())
		{
			int iNewOutput = pCity->AI_professionBasicOutput(eProfession, eUnit, NULL);
			int iProfessionCount = 0;
			bool bDone = false;
			for (int i = 0; i < pCity->getPopulation(); ++i)
			{
				CvUnit* pLoopUnit = pCity->getPopulationUnitByIndex(i);
				if (pLoopUnit->getProfession() == eProfession)
				{
					int iOldOutput = pCity->AI_professionBasicOutput(eProfession, pLoopUnit->getUnitType(), NULL);

					if (iNewOutput > iOldOutput)
					{
						int iValue = AI_yieldValue(eYield, true, iNewOutput);
						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							break;
						}
					}
					else
					{
						iProfessionCount ++;
					}
				}
			}
			if (iBestValue == 0)
			{
				if (iProfessionCount < pCity->getNumProfessionBuildingSlots(eProfession))
				{
					iBestValue = AI_yieldValue(eYield, true, iNewOutput);
				}
			}
		}
		else
		{
			for (int i = 0; i < NUM_CITY_PLOTS; ++i)
			{
				CvPlot* pLoopPlot = plotCity(pCity->getX_INLINE(), pCity->getY_INLINE(), i);
				if ((pLoopPlot != NULL) && (pLoopPlot->getWorkingCity()==pCity))
				{
					int iNewOutput = pCity->AI_professionBasicOutput(eProfession, eUnit, pLoopPlot);
					int iOldOutput = 0;
					if (pLoopPlot->isBeingWorked())
					{
						CvUnit* pWorkingUnit = pCity->getUnitWorkingPlot(pLoopPlot);
						if (pWorkingUnit != NULL)
						{
							if ((pWorkingUnit->getProfession() == eProfession))
							{
								iOldOutput = pCity->AI_professionBasicOutput(eProfession, pWorkingUnit->getUnitType(), pLoopPlot);
								if (iNewOutput <= iOldOutput)
								{
									iNewOutput = 0;
								}
							}
							else
							{
								bool bOverride = false;
								if ((pLoopPlot->getBonusType() != NO_BONUS) && (GC.getBonusInfo(pLoopPlot->getBonusType()).getYieldChange(eYield) > 0))
								{
									bOverride = true;
								}
								if (!bOverride)
								{
									int iBestYield = AI_getBestPlotYield(eYield);
									if (pLoopPlot->getBonusType() == NO_BONUS)
									{
										if (pLoopPlot->calculateBestNatureYield(eYield, getTeam()) >= iBestYield)
										{
											bOverride = true;
										}
									}
								}
								if (!bOverride)
								{
									iNewOutput = 0;
								}
							}
						}
					}

					if (iNewOutput > iOldOutput)
					{
						int iValue = AI_yieldValue(eYield, true, iNewOutput);
						if (iValue > iBestValue)
						{
							iBestValue = iValue;
						}
					}
				}
			}
		}
	}

	return iBestValue;
}

int CvPlayerAI::AI_professionUpgradeValue(ProfessionTypes eProfession, UnitTypes eUnit)
{
	CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
	// R&R, ray , MYCP partially based on code of Aymerick - START
	YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
	// R&R, ray , MYCP partially based on code of Aymerick - END
	if (eYield == NO_YIELD)
	{
		return 0;
	}

	CvUnitInfo& kUnit = GC.getUnitInfo(eUnit);

	int iBestValue = 0;
	CvCity* pBestCity = NULL;

	int iLoop;
	CvCity* pLoopCity;

	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		for (int i = 0; i < pLoopCity->getPopulation(); ++i)
		{
			CvUnit* pLoopUnit = pLoopCity->getPopulationUnitByIndex(i);

			if (pLoopUnit->getProfession() == eProfession)
			{
				int iExistingYield = 0;
				int iNewYield = 0;
				int iExtraMultiplier = 0;
				if (kProfession.isWorkPlot())
				{
					CvPlot* pWorkedPlot = pLoopCity->getPlotWorkedByUnit(pLoopUnit);
					if (pWorkedPlot != NULL)
					{
						iExistingYield = pWorkedPlot->calculatePotentialYield(eYield, getID(), pWorkedPlot->getImprovementType(), false, pWorkedPlot->getRouteType(), pLoopUnit->getUnitType(), false);
						iNewYield = pWorkedPlot->calculatePotentialYield(eYield, getID(), pWorkedPlot->getImprovementType(), false, pWorkedPlot->getRouteType(), eUnit, false);
						if (pWorkedPlot->getBonusType() != NO_BONUS && GC.getBonusInfo(pWorkedPlot->getBonusType()).getYieldChange(eYield) > 0)
						{
							iExtraMultiplier += 100;
						}
					}
				}
				else
				{
					CvUnitInfo& kLoopUnit = GC.getUnitInfo(pLoopUnit->getUnitType());
					iExistingYield = pLoopCity->getProfessionOutput(eProfession, pLoopUnit);
					iNewYield = (iExistingYield * 100) / (100 + kLoopUnit.getYieldModifier(eYield));
					iNewYield -= kLoopUnit.getYieldChange(eYield);
					iNewYield += kUnit.getYieldChange(eYield);
					iNewYield = iNewYield * (100 + kUnit.getYieldModifier(eYield)) / 100;
				}

				if (iNewYield > iExistingYield)
				{
					int iValue = AI_yieldValue(eYield, true, iNewYield - iExistingYield);
					iValue *= 100 + iExtraMultiplier;
					iValue /= 100;
					if (iValue > iBestValue)
					{
						iBestValue = iValue;
						pBestCity = pLoopCity;
					}
				}
			}
		}
	}

	return iBestValue;
}

int CvPlayerAI::AI_professionValue(ProfessionTypes eProfession, UnitAITypes eUnitAI) const
{
	CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
	if (kProfession.isCitizen())
	{
		return 0;
	}

	int iValue = 0;
	switch (eUnitAI)
	{
		case UNITAI_UNKNOWN:
		case UNITAI_ANIMAL: // R&R, ray, Wild Animals
		case UNITAI_ANIMAL_SEA: // R&R, ray, Wild Animals
		case UNITAI_FLEEING: // R&R, ray, Fleeing
			break;

		case UNITAI_COLONIST:
			{
				if (GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession() == eProfession)
				{
					iValue += 100;
				}
			}
			break;

		case UNITAI_SETTLER:
			{
				if (kProfession.canFound())
				{
					iValue += 200; //WTP, ray, Settler Professsion - START
				}
			}
			break;

		case UNITAI_WORKER:
			{
				if (kProfession.getWorkRate() > 0)
				{
					iValue += kProfession.getWorkRate();
				}
			}
			break;

		case UNITAI_MISSIONARY:
			{
				if (kProfession.getMissionaryRate() > 0)
				{
					iValue += kProfession.getMissionaryRate();
				}
			}
			break;
		// WTP, ray, Native Trade Posts - START
		case UNITAI_TRADER:
			{
				if (kProfession.getNativeTradeRate() > 0)
				{
					iValue += kProfession.getNativeTradeRate();
				}
			}
			break;

		case UNITAI_SCOUT:
			{
				if (kProfession.isScout())
				{
					iValue += 100;
				}
				iValue += 20 * kProfession.getMovesChange();
			}
			break;

		case UNITAI_WAGON:
		case UNITAI_TREASURE:
		case UNITAI_YIELD:
		case UNITAI_GENERAL:
			break;

		case UNITAI_DEFENSIVE:
			{
				int iExtraCombatStrength = kProfession.getCombatChange() - GC.getProfessionInfo(GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession()).getCombatChange();
				// WTP, ray, Cannons to Professions - START
				int iExtraBombardStrength = kProfession.getBombardRateChangeProfession()- GC.getProfessionInfo(GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession()).getBombardRateChangeProfession();
				// WTP, ray, Cannons to Professions - END
				if (isNative())
				{
					iValue += 10;
				}

				if (!kProfession.isUnarmed() && iExtraCombatStrength > 0)
				{
					if (kProfession.isCityDefender())
					{
						iValue += iExtraCombatStrength * 25;
						iValue += iExtraBombardStrength * 25; // WTP, ray, Cannons to Professions - START
					}
				}
			}
			break;

		case UNITAI_OFFENSIVE:
			// TAC - AI purchases military units - koma13 - START
			//if (isNative())
			{
				iValue += 10;
				int iExtraCombatStrength = kProfession.getCombatChange() - GC.getProfessionInfo(GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession()).getCombatChange();
				// WTP, ray, Cannons to Professions - START
				int iExtraBombardStrength = kProfession.getBombardRateChangeProfession()- GC.getProfessionInfo(GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession()).getBombardRateChangeProfession();
				// WTP, ray, Cannons to Professions - END
				if (!kProfession.isUnarmed() && iExtraCombatStrength > 0)
				{
					iValue += iExtraCombatStrength * 15;
					//iValue += kProfession.getMovesChange() * 15;
					iValue += kProfession.getMovesChange() * 10;
					iValue += iExtraBombardStrength * 10; // WTP, ray, Cannons to Professions - START
				}
			}
			// TAC - AI purchases military units - koma13 - END

			break;
		case UNITAI_COUNTER:
			{
				int iExtraCombatStrength = kProfession.getCombatChange() - GC.getProfessionInfo(GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession()).getCombatChange();
				if (isNative())
				{
					iValue += 10;
				}

				if (isNative() || (!kProfession.isUnarmed() && iExtraCombatStrength > 0))
				{
					iValue += iExtraCombatStrength * 15;
					iValue *= 1 + kProfession.getMovesChange();

					// WTP, ray, Cannons to Professions - START
					// in Counter BombarStrength does not matter
				}
			}
			break;

		case UNITAI_WORKER_SEA: //TAC Whaling, ray
		case UNITAI_TRANSPORT_SEA:
		case UNITAI_TRANSPORT_COAST:
		case UNITAI_ASSAULT_SEA:
		case UNITAI_COMBAT_SEA:
		case UNITAI_PIRATE_SEA:
		case UNITAI_ESCORT_SEA: // TAC - AI Escort Sea - koma13
			break;
		default:
			FAssert(false);
			break;
	}
	return iValue;
}

// R&R, ray, code changes for Ideal Profession - START
// Heavily modified
// simply returns the first Expert to Profession that matches UnitType
ProfessionTypes CvPlayerAI::AI_idealProfessionForUnit(UnitTypes eUnitType) const
{
	const UnitClassTypes eUnitClassType = GC.getUnitInfo(eUnitType).getUnitClassType();
	ProfessionTypes eIdealProfession = NO_PROFESSION;

	for (ProfessionTypes eLoopProfession = FIRST_PROFESSION; eLoopProfession < NUM_PROFESSION_TYPES; ++eLoopProfession)
	{
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eLoopProfession);

		if (kProfession.isCitizen())
		{
			if (kProfession.LbD_getExpert() == eUnitClassType)
			{
				eIdealProfession = eLoopProfession;
				break;
			}
		}
	}
	return eIdealProfession;
}
// R&R, ray, code changes for Ideal Profession - END

ProfessionTypes CvPlayerAI::AI_idealProfessionForUnitAIType(UnitAITypes eUnitAI, CvCity* pCity)
{
	int iBestValue = 0;
	ProfessionTypes eBestProfession = NO_PROFESSION;

	for (int iI = 0; iI < GC.getNumProfessionInfos(); ++iI)
	{
		ProfessionTypes eLoopProfession = (ProfessionTypes)iI;
		CvProfessionInfo& kProfession = GC.getProfessionInfo(eLoopProfession);

		if (!(kProfession.isCitizen() || kProfession.isWorkPlot()))
		{
			//WTP, ray, Settler Professsion - START
			if (GC.getCivilizationInfo(getCivilizationType()).isValidProfession(eLoopProfession))
			{
				//we can only do this, if it is checking for a Unit in a City
				if (eUnitAI == UNITAI_SETTLER && kProfession.canFound() && pCity != NULL)
				{
					//we better check the size of the City
					//I have no idea, what the population check below in else is supposed to do
					if (pCity->getPopulation() > 1)
					{
						iBestValue = MAX_INT;
						eBestProfession = eLoopProfession;
						break; // do not loop through professions anymore, we already found the best Profession for SETTLER_AI
					}
				}

				else
				//WTP, ray, Settler Professsion - END
				{
					CvUnit* pUnit = NULL;
					if (pCity != NULL)
					{
						pUnit = pCity->getPopulationUnitByIndex(0);
					}
					if (pUnit == NULL || pUnit->canHaveProfession(eLoopProfession, true))
					{
						int iValue = AI_professionValue(eLoopProfession, eUnitAI);

						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							eBestProfession = eLoopProfession;
						}
					}
				}
			}
		}
	}

	return eBestProfession;
}

//100 means "An average use for Gold", so a higher multiplier here, means a higher priority on buying that unit type.
//Most notably a "0" means "Don't bother".
int CvPlayerAI::AI_unitAIValueMultipler(UnitAITypes eUnitAI)
{
	int iCurrentCount = AI_totalUnitAIs(eUnitAI);
	// WTP: warships carrying goods and colonists in peace are warships still
	if (eUnitAI == UNITAI_COMBAT_SEA)
	{
		iCurrentCount += AI_numWarshipsAsTransports();
	}
	int iCount = AI_totalUnitAIs(eUnitAI) + AI_getNumRetiredAIUnits(eUnitAI);
	// WTP: (also in the count the purchase value uses: the Swedes bought two more war caravels right after three of theirs
	// had become transports)
	if (eUnitAI == UNITAI_COMBAT_SEA)
	{
		iCount += AI_numWarshipsAsTransports();
	}
	// WTP, ray, Native Trade Posts - START
	// adjustment for UNITAI_TRADER
	int iPopulation = AI_getPopulation() + AI_getNumRetiredAIUnits(UNITAI_MISSIONARY) + AI_getNumRetiredAIUnits(UNITAI_TRADER);
	int iValue = 0;
	switch (eUnitAI)
	{
		case UNITAI_UNKNOWN:
			break;

		case UNITAI_COLONIST:
			{
				iValue = std::min(75, 20 + iPopulation);
				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iValue *= 75;
					iValue /= 100;
					if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
					{
						iValue *= 75;
						iValue /= 100;
					}
				}
			}
			break;

		case UNITAI_SETTLER:
			{
				if (!AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					int iDesiredCities = AI_desiredCityCount();
					if (iDesiredCities > (getNumCities() + iCount))
					{
						iValue = 100 + 150 * (iDesiredCities - (getNumCities() + iCount));
					}
				}
			}
			break;

		case UNITAI_WORKER:
			if (!AI_isStrategy(STRATEGY_REVOLUTION_DECLARING) && iCurrentCount < 15)
			{
				int iNeeded = AI_neededWorkers(NULL);

				if (iNeeded > iCount)
				{
					iValue = 100 + 20 * iNeeded + (50 * iNeeded) / (iCount + 1);
				}
			}
			break;

		case UNITAI_MISSIONARY:
			if (!AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
			{
				int iLowerPop = 12;
				int iPop = 15 + iCount * 5;
				int iModifier = 0;
				iModifier -= getNativeCombatModifier() * 4;
				iModifier += getMissionaryRateModifier();
				iModifier += 10 * GC.getLeaderHeadInfo(getLeaderType()).getNativeAttitude();
				iModifier += getMissionarySuccessPercent() - 50;
				if (iModifier != 0)
				{
					iLowerPop *= 100 + std::max(0, -iModifier);
					iLowerPop /= 100 + std::max(0, iModifier);
					iPop *= 100 + std::max(0, -iModifier);
					iPop /= 100 + std::max(0, iModifier);
				}

				iValue = (100 * std::max(0, iPopulation - (iLowerPop + iPop * iCount))) / iPop;
				if (iValue > 0)
				{
					iValue += 100;
				}
			}
			break;

		// WTP, ray, Native Trade Posts - START
		case UNITAI_TRADER:
			if (!AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
			{
				int iLowerPop = 12;
				int iPop = 15 + iCount * 5;
				int iModifier = 0;
				iModifier -= getNativeCombatModifier() * 4;
				iModifier += getNativeTradeModifier();
				iModifier += 10 * GC.getLeaderHeadInfo(getLeaderType()).getNativeAttitude();
				iModifier += getNativeTradePostSuccessPercent() - 50;
				if (iModifier != 0)
				{
					iLowerPop *= 100 + std::max(0, -iModifier);
					iLowerPop /= 100 + std::max(0, iModifier);
					iPop *= 100 + std::max(0, -iModifier);
					iPop /= 100 + std::max(0, iModifier);
				}

				iValue = (100 * std::max(0, iPopulation - (iLowerPop + iPop * iCount))) / iPop;
				if (iValue > 0)
				{
					iValue += 100;
				}
			}
			break;
		// WTP, ray, Native Trade Posts - END

		case UNITAI_SCOUT:
			if (!AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
			{
				if (iCount <= 1)
				{
					int iTotalUnexploredPlots = 0;

					CvArea* pLoopArea;
					int iLoop;
					for(pLoopArea = GC.getMap().firstArea(&iLoop); pLoopArea != NULL; pLoopArea = GC.getMap().nextArea(&iLoop))
					{
						if (!(pLoopArea->isWater()))
						{
							iTotalUnexploredPlots += pLoopArea->getNumRevealedTiles(getTeam());
						}
					}

					if (iTotalUnexploredPlots > 10)
					{
						if (iCount == 0)
						{
							iValue = 500;//scout is important.
						}
						else if (iCount == 1)
						{
							if ((iTotalUnexploredPlots > 500) && (iPopulation > 5))
							{
								iValue = 150;
							}
						}
					}
				}
			}
			break;

		case UNITAI_WAGON:
			{
				// Erik: The number of land transports should depend on the production
				// vs. consumption rate of all cargo yields as well as the number of cities
				const int iCityCount = getNumCities();
				const int iNeeded = iCityCount*iCityCount / (countNumCoastalCities() + 1);

				if (iCount < iNeeded)
				{
					iValue = 100 + (100 * (iNeeded - iCurrentCount));
				}
			}
			break;

		case UNITAI_TRANSPORT_COAST:
			{
				const int iNeeded = countNumCoastalCities() / 2;

				if (iCount < iNeeded)
				{
					iValue = 100 + (100 * (iNeeded - iCurrentCount));
				}
			}
			break;

		case UNITAI_TREASURE:
		case UNITAI_YIELD:
		case UNITAI_GENERAL:
		case UNITAI_ANIMAL: // R&R, ray adjustments to new UnitAIs
		case UNITAI_ANIMAL_SEA: // R&R, ray adjustments to new UnitAIs
		case UNITAI_FLEEING: // R&R, ray adjustments to new UnitAIs
			break;

		case UNITAI_DEFENSIVE:
			{
				if (isNative())
				{
					return 100;
				}

				bool bAtWar = GET_TEAM(getTeam()).getAnyWarPlanCount();

				// TAC - AI City Defense - koma13 - START
				/*
				int iLowerPop = bAtWar ? 8 : 15;
				int iPop = bAtWar ? (10 + 5 * iCount) : 17 + 6 * iCount;
				iValue = 110 * std::max(0, iPopulation - (iLowerPop + iPop * iCount)) / iPop;

				int iUndefended = 0;
				int iNeeded = AI_totalDefendersNeeded(&iUndefended);
				if (iUndefended > 0)
				{
					iValue += 100 + ((200 * iUndefended) / (1 + getNumCities()));
				}

				if (iCount < getNumCities())
				{
					iValue += 100 + ((200 * (getNumCities() - iCount)) / (1 + getNumCities()));
				}

				iValue += (200 * iNeeded) / (1 + getNumCities());

				if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
				{
					iValue += 3000 / (25 + iCount);
				}

				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iValue += 3000 / (25 + iCount);
				}

				if (getGold() > 5000)
				{
					iValue += 1;
					iValue *= 2;
				}
				*/

				int iUndefended = 0;
				int iNeeded = AI_totalDefendersNeeded(&iUndefended, NULL, true);

				if (iNeeded <= 0)
				{
					return 0;
				}

				if (iCount < getNumCities())
				{
					iValue += 100 + ((200 * (getNumCities() - iCount)) / (1 + getNumCities()));
				}

				iValue += ((bAtWar ? 100 : 50) * iNeeded) / (1 + getNumCities());

				if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
				{
					iValue += 3000 / (25 + iCount);
				}

				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iValue += 3000 / (25 + iCount);
				}
				// TAC - AI City Defense - koma13 - END
			}
			break;

		case UNITAI_OFFENSIVE:
			{
				bool bAtWar = GET_TEAM(getTeam()).getAnyWarPlanCount();
				int iLowerPop = bAtWar ? 9 : 15 ;
				int iPop = bAtWar ? 10 : 20;
				iValue = 100 * std::max(0, iPopulation - (iLowerPop + iPop * iCount)) / iPop;

				if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
				{
					iValue += 3000 / (40 + iCount);
				}
				else
				{
					if (iValue > 0)
					{
						iValue += 50 + getNativeCombatModifier() * 2;
					}
				}

				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iValue += 1500 / (40 + iCount);
				}

				// TAC - AI purchases military units - koma13 - START
				// WTP: only while planning or fighting a war, not as a permanent incentive
				if (!isEurope() && !isNative() && GET_TEAM(getTeam()).getAnyWarPlanCount() > 0)
				{
					iValue += 1500 / (40 + iCount);
				}
				// TAC - AI purchases military units - koma13 - END
			}
			break;

		case UNITAI_COUNTER:
			{
				// TAC - AI Counter Units - koma13 - START
				if (iCount >= getNumCities())
				{
					return 0;
				}
				// TAC - AI Counter Units - koma13 - END

				bool bAtWar = GET_TEAM(getTeam()).getAnyWarPlanCount();
				int iPop = bAtWar ? 12 : 20;
				int iLowerPop = bAtWar ? 8 : 14;
				iValue = 100 * std::max(0, iPopulation - (iLowerPop + iPop * iCount)) / iPop;
				if (iValue > 0)
				{
					iValue += 25;
				}

				if (AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
				{
					iValue += 3000 / (25 + iCount);
				}

				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iValue += 3000 / (25 + iCount);
				}

				if (getGold() > 5000)
				{
					iValue += 1;
					iValue *= 2;
				}
			}
			break;

		//TAC Whaling, ray
		case UNITAI_WORKER_SEA:
			if (!AI_isStrategy(STRATEGY_REVOLUTION))
			{
				int iNeeded = countNumCoastalCities();
				if (iCount < iNeeded)
				{
					iValue = 100 + 20 * iNeeded + (50 * iNeeded) / (iCount + 1);
				}
			}
			break;

		case UNITAI_TRANSPORT_SEA:
			if (!AI_isStrategy(STRATEGY_REVOLUTION))
			{
				// TAC - AI purchases military units - koma13 - START
				//if (iCount < 6)
				if (iCount < std::max(3, getNumCities()))
				{
					int iLowerPop = 5 - countNumCoastalCities();
					int iPop = 13 + 26 * iCount;

					iValue = 150 * std::max(0, iPopulation - (iLowerPop + iPop * iCount)) / iPop;

					//iValue += 25 * std::max(0, AI_countYieldWaiting() - 4 * iCount);
					iValue += 25 * std::max(0, AI_countYieldWaiting() - iCount);
					iValue += 25 * std::max(0, getNumEuropeUnits() - 4 * iCount);
				}
				// TAC - AI purchases military units - koma13 - END
			}
			break;

		case UNITAI_ASSAULT_SEA:
			{
				// See comment for the escort AI. We allow a single assault ship since they are sometimes useful
				// for reinforcement missions (ferrying defenders etc.)
				if (iCurrentCount == 0)
				{
					if (AI_prepareAssaultSea())	// TAC - AI Assault Sea - koma13
					{
						int iLowerPop = 5;
						int iPop = 15 + 50 * iCount;
						iValue = (160 * std::max(0, iPopulation - (iLowerPop + iPop * iCount))) / iPop;
					}
				}
			}
		case UNITAI_COMBAT_SEA:
			{
				// WTP: in a war with a colonial power twice as many warships (counted as half; natives have no ships)
				int iWarshipCount = iCount;
				const CvTeam& kTeam = GET_TEAM(getTeam());
				for (PlayerTypes ePlayer = FIRST_PLAYER; ePlayer < NUM_PLAYER_TYPES; ++ePlayer)
				{
					const CvPlayer& kOther = GET_PLAYER(ePlayer);
					if (kOther.isAlive() && kOther.getTeam() != getTeam() && !kOther.isNative() && !GC.getGameINLINE().isBarbarianPlayer(ePlayer)
						&& kTeam.isAtWar(kOther.getTeam()))
					{
						iWarshipCount = iCount / 2;
						break;
					}
				}
				int iLowerPop = 5;
				int iPop = 40 + 30 * iWarshipCount;
				if (AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
				{
					iPop /= 2;
				}
				iValue = (140 * std::max(0, iPopulation - (iLowerPop + iPop * iWarshipCount))) / iPop;
			}
			break;

		case UNITAI_PIRATE_SEA:
			{
				// Get combat ships before considering a privateer
				if (iCurrentCount < 2 && AI_totalUnitAIs(UNITAI_COMBAT_SEA) > 0)
				// TAC - AI purchases military units - koma13 - START
				{
					/*
					int iLowerPop = 5;
					int iPop = 16 + 6 * iCount;
					iValue = (140 * std::max(0, iPopulation - (iLowerPop + iPop * iCount))) / iPop;
					*/
					int iLowerPop = 10;
					int iPop = 16 + 6 * iCurrentCount;
					iValue = (140 * std::max(0, iPopulation - (iLowerPop + iPop * iCurrentCount))) / iPop;

					int iGameTurn = GC.getGameINLINE().getGameTurn();
					int iEndTurn = GC.getGameINLINE().getEstimateEndTurn();
					int iTrainPercent = GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAITrainPercent();
					int iTempValue = (iTrainPercent * iEndTurn * ((iCount * iCount) / 2)) / ((1 + iGameTurn) * 3);
					iValue = std::max(0, iValue - iTempValue);
				}
				// TAC - AI purchases military units - koma13 - END
			}
			break;

		// TAC - AI Escort Sea - koma13 - START
		case UNITAI_ESCORT_SEA:
			// Disabled for now, the colonial AI is currently not able to undertake intercontinental invasions
			// and hence there is no need for escort ships
			/*
			{
				if (iCount < AI_totalUnitAIs(UNITAI_ASSAULT_SEA))
				{
					int iLowerPop = 5;
					int iPop = 15 + 50 * iCount;
					iValue = (160 * std::max(0, iPopulation - (iLowerPop + iPop * iCount))) / iPop;
				}
			}
			*/
			break;
		// TAC - AI Escort Sea - koma13 - END

		default:
			FAssert(false);
			break;
	}

	iValue *= 100 + m_em_iUnitAIStrategyWeights.get(eUnitAI);
	iValue /= 100;

	return iValue;
}

bool CvPlayerAI::AI_isCityAcceptingYield(CvCity* pCity, YieldTypes eYield)
{
	for (CvIdVector<CvTradeRoute>::iterator it = m_tradeRoutes.begin(); it != m_tradeRoutes.end(); ++it)
	{
		CvTradeRoute* pTradeRoute = it->second;

		if (pTradeRoute->getYield() == eYield)
		{
			if (pTradeRoute->getDestinationCity() == pCity->getIDInfo())
			{
				return true;
			}
		}
	}

	return false;
}

// TAC - AI Economy - koma13 - START
bool CvPlayerAI::AI_isTradeRouteConnection(CvCity* pSourceCity, CvCity* pDestinationCity)
{
	FAssert(pSourceCity != NULL || pDestinationCity != NULL);

	for (CvIdVector<CvTradeRoute>::iterator it = m_tradeRoutes.begin(); it != m_tradeRoutes.end(); ++it)
	{
		CvTradeRoute* pTradeRoute = it->second;

		if (pTradeRoute->getSourceCity() == pSourceCity->getIDInfo())
		{
			if (pTradeRoute->getDestinationCity() == pDestinationCity->getIDInfo())
			{
				return true;
			}
		}
	}

	return false;
}
// TAC - AI Economy - koma13 - END
int CvPlayerAI::AI_professionSuitability(UnitTypes eUnit, ProfessionTypes eProfession) const
{
	if (eProfession == NO_PROFESSION)
	{
		return 0;
	}

	if (!GC.getCivilizationInfo(getCivilizationType()).isValidProfession(eProfession))
	{
		return 0;
	}


	CvUnitInfo& kUnit = GC.getUnitInfo(eUnit);

	if (kUnit.getDefaultProfession() == NO_PROFESSION)
	{
		return 0;
	}

	if (eProfession == GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession())
	{
		return 100;
	}

	CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
	int iValue = 100;

	int iPositiveYields = 0;
	int iNegativeYields = 0;

	int iProModifiers = 0;
	int iConModifiers = 0;

	const ProfessionTypes eIdealProfession = AI_idealProfessionForUnit(eUnit);

	if (kProfession.isWater() && kUnit.isWaterYieldChanges() || !kProfession.isWater() && kUnit.isLandYieldChanges())
	{

		for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
		{
			YieldTypes eLoopYield = (YieldTypes)iYield;


			int iModifier = kUnit.getYieldModifier(eLoopYield);
			// XXX account for kUnit.getYieldChange, kUnit.getBonusYieldChange
			int iYieldChange = kUnit.getYieldChange(eLoopYield) * 2 + kUnit.getBonusYieldChange(eLoopYield);
			iModifier += 10 * iYieldChange;

			if (iModifier != 0)
			{
				// R&R, ray , MYCP partially based on code of Aymerick - START
				if (kProfession.getYieldsProduced(0) == eLoopYield)
				// R&R, ray , MYCP partially based on code of Aymerick - END
				{
					//We produce enhanced yield for this profession.
					if (iModifier > 0)
					{
						iProModifiers += iModifier;
					}
					else //We produce reduced yield for this profession.
					{
						iConModifiers += iModifier;
					}
				}
				else
				{
					//We produce enhanced yield for ANOTHER profession.
					if (iModifier > 0)
					{
						iNegativeYields = std::max(iModifier, iNegativeYields);
					}
				}
			}
		}

		iProModifiers += iPositiveYields / 8;
		iConModifiers += iNegativeYields / 20;

	}

	if (!kProfession.isCitizen())
	{
		int iChange = kUnit.getYieldModifier(YIELD_FOOD) / 10 + kUnit.getYieldChange(YIELD_FOOD) * 3 + kUnit.getYieldChange(YIELD_FOOD) * 2;
		if (iChange > 0)
		{
			iConModifiers += iChange;
		}
	}

	if (kProfession.getMissionaryRate() > 0)
	{
		int iModifier = kUnit.getMissionaryRateModifier();
		if (iModifier > 0)
		{
			iProModifiers += iModifier;
		}
		else
		{
			iConModifiers += iModifier / 8;
		}
	}

	// WTP, ray, Native Trade Posts - START
	if (kProfession.getNativeTradeRate() > 0)
	{
		int iModifier = kUnit.getNativeTradeRateModifier();
		if (iModifier > 0)
		{
			iProModifiers += iModifier;
		}
		else
		{
			iConModifiers += iModifier / 8;
		}
	}
	// WTP, ray, Native Trade Posts - EMD

	if (kProfession.getWorkRate() > 0)
	{
		int iModifier = kUnit.getWorkRate();
		if (iModifier > 0)
		{
			iProModifiers += iModifier;
		}
		else
		{
			iConModifiers += iModifier / 8;
		}
	}

	if (kProfession.isScout())
	{
		if (kUnit.isNoBadGoodies())
		{
			iProModifiers += 100;
		}
		else
		{
			// If this is an expert unit, discourage it slightly
			if (eIdealProfession != NO_PROFESSION)
			{
				iConModifiers += 20;
			}
			else
			{
				iConModifiers += 10;
			}
		}
	}

	for (int i = 0; i < GC.getNumPromotionInfos(); ++i)
	{
		if (kUnit.getFreePromotions(i))
		{
			if (kProfession.isUnarmed())
			{
				iConModifiers += 25;
			}
			else
			{
				iProModifiers += 25;
			}
		}
	}

	if (eProfession != GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession())
	{
		if (kUnit.getDefaultProfession() == eProfession)
		{
			iProModifiers = std::max(100, iProModifiers);//Just in case.
		}
		else
		{
			if (hasContentsYieldEquipmentAmount(kUnit.getDefaultProfession())) // cache CvPlayer::getYieldEquipmentAmount - Nightinggale
			{
				for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
				{
					if (getYieldEquipmentAmount(kUnit.getDefaultProfession(), (YieldTypes) iYield) > 0)
					{
						if (getYieldEquipmentAmount(eProfession, (YieldTypes) iYield) == 0)
						{
							iConModifiers += 50;
						}
						break;
					}
				}
			}
		}
	}

	iValue *= 100 + iProModifiers;
	iValue /= 100 + iConModifiers;

	return iValue;
}

int CvPlayerAI::AI_professionSuitability(const CvUnit* pUnit, ProfessionTypes eProfession, const CvPlot* pPlot, UnitAITypes eUnitAI) const
{
	CvPlot* pCityPlot = NULL;
 	if (pPlot != NULL)
	{
		CvCity* pCity = pPlot->getPlotCity();
		if (pCity == NULL)
		{
			pCity = pPlot->getWorkingCity();
		}

		if (pCity != NULL)
		{
			pCityPlot = pCity->plot();
		}
	}

	if (!pUnit->canHaveProfession(eProfession, true, pCityPlot))
	{
		return 0;
	}

	int iValue = AI_professionSuitability(pUnit->getUnitType(), eProfession);

	if (eUnitAI != NO_UNITAI && pUnit != NULL)
	{
		int iPromotionCount = 0;
		for (int i = 0; i < GC.getNumPromotionInfos(); ++i)
		{
			PromotionTypes eLoopPromotion = (PromotionTypes)i;

			if (pUnit->isHasPromotion(eLoopPromotion))
			{
				iPromotionCount ++;
			}
		}
		iValue *= 100 + 5 * iPromotionCount;
		iValue /= 100;

		if (eUnitAI == UNITAI_OFFENSIVE)
		{
			iValue *= 100 + pUnit->cityAttackModifier();
			iValue /= 100;

			if (pUnit->isProfessionalMilitary())
			{
				iValue *= 200;
				iValue /= 100;
			}
		}
		else if (eUnitAI == UNITAI_DEFENSIVE)
		{
			iValue *= 100 + pUnit->cityDefenseModifier();
			iValue /= 100;

			if (pUnit->isProfessionalMilitary())
			{
				iValue *= 200;
				iValue /= 100;
			}
		}
	}

	if (pPlot == NULL)
	{
		return iValue;
	}

	CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
	CvUnitInfo& kUnit = GC.getUnitInfo(pUnit->getUnitType());

	CvCity* pCity = pPlot->getPlotCity();
	if (pCity == NULL)
	{
		pCity = pPlot->getWorkingCity();
	}

	if (pCity == NULL || pCity->getOwnerINLINE() != getID())
	{
		return iValue;
	}

	bool bMismatchedBonus = false;
	int iExtraValue = 0;
	if (kProfession.isCitizen())
	{
		// R&R, ray , MYCP partially based on code of Aymerick - START
		YieldTypes eYieldProducedType = (YieldTypes)kProfession.getYieldsProduced(0);
		// R&R, ray , MYCP partially based on code of Aymerick - END
		FAssert(eYieldProducedType != NO_YIELD);

		if (kProfession.isWorkPlot())
		{
			if (pPlot->getWorkingCity() == pCity)
			{
				if (kProfession.isWater() == pPlot->isWater())
				{
					if (pPlot->getBonusType() != NO_BONUS)
					{
						int iBonusYield = GC.getBonusInfo(pPlot->getBonusType()).getYieldChange(eYieldProducedType);
						if (iBonusYield > 0)
						{
							int iPlotYield = pPlot->calculateNatureYield(eYieldProducedType, getTeam());
							if (iPlotYield > 0)
							{
								int iExtraYield = kUnit.getYieldChange(eYieldProducedType);
								iExtraYield += kUnit.getBonusYieldChange(eYieldProducedType);

								iExtraValue += (100 * iExtraYield) / iPlotYield;
							}
						}
						else
						{
							bMismatchedBonus = true;
						}
					}
				}
			}
		}
		else
		{
			if (pCity != NULL)
			{
				int iModifier = kUnit.getYieldModifier(eYieldProducedType);
				iModifier += kUnit.getYieldChange(eYieldProducedType) * 10;

				iModifier *= pCity->AI_getYieldAdvantage(eYieldProducedType);
				iModifier /= 100;

				iExtraValue += iModifier;
			}
		}
	}

	iValue += iExtraValue;
	if (bMismatchedBonus)
	{
		iValue *= 95;
		iValue /= 100;
	}

	if (eUnitAI == UNITAI_OFFENSIVE || eUnitAI == UNITAI_DEFENSIVE || eUnitAI == UNITAI_COUNTER)
	{
		const CvUnitInfo& kUnitInfo = pUnit->getUnitInfo();

		// For combat units, we penalize the use of valuable units
		if (abs(kUnitInfo.getEuropeCost()) > 1000)
		{
			iValue *= 50;
			iValue /= 100;
		}

		// Erik: If the unit is generally capable of escaping, we prefer it for a combat units since
		// it will prevent it from escaping
		if (kUnitInfo.LbD_canEscape())
		{
			iValue *= 150;
			iValue /= 100;
		}
	}

	return iValue;
}

// Note: A swap should never fail so an assert\fail here indicates that the caller
// messed up!
void CvPlayerAI::AI_swapUnitJobs(CvUnit* pUnitA, CvUnit* pUnitB)
{
	FAssert(pUnitA->plot() == pUnitB->plot());

	UnitAITypes eUnitAI_A = pUnitA->AI_getUnitAIType();
	ProfessionTypes eProfession_A = pUnitA->getProfession();
	int iMovePriorityA = pUnitA->AI_getMovePriority();

	UnitAITypes eUnitAI_B = pUnitB->AI_getUnitAIType();
	ProfessionTypes eProfession_B = pUnitB->getProfession();
	int iMovePriorityB = pUnitB->AI_getMovePriority();

	CvProfessionInfo& kProfessionA = GC.getProfessionInfo(eProfession_A);
	CvProfessionInfo& kProfessionB = GC.getProfessionInfo(eProfession_B);

	CvCity* pCity = getPopulationUnitCity(pUnitA->getID());
	if (pCity == NULL)
	{
		FAssert(pUnitA->isOnMap_());
		pCity = pUnitA->plot()->getPlotCity();
	}
	FAssert(pCity != NULL);

	ProfessionTypes eDefaultProfession = GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession();

	//Ensure all units are added to city.
	if (pUnitA->isOnMap_())
	{
		pCity->addPopulationUnit(pUnitA, NO_PROFESSION);
	}
	else
	{
		pUnitA->setProfession(NO_PROFESSION);
	}

	if (pUnitB->isOnMap_())
	{
		pCity->addPopulationUnit(pUnitB, NO_PROFESSION);
	}
	else
	{
		pUnitB->setProfession(NO_PROFESSION);
	}

	if (kProfessionA.isCitizen())
	{
		pUnitB->setProfession(eProfession_A);
	}
	else
	{
		pCity->removePopulationUnit(CREATE_ASSERT_DATA, pUnitB, false, eProfession_A);
		pUnitB->AI_setUnitAIType(eUnitAI_A);
		pUnitB->AI_setMovePriority(iMovePriorityA);
	}

	if (kProfessionB.isCitizen())
	{
		pUnitA->setProfession(eProfession_B);
	}
	else
	{
		pCity->removePopulationUnit(CREATE_ASSERT_DATA, pUnitA, false, eProfession_B);
		pUnitA->AI_setMovePriority(iMovePriorityB);
		pUnitA->AI_setUnitAIType(eUnitAI_B);
	}
}

int CvPlayerAI::AI_sumAttackerStrength(CvPlot* pPlot, CvPlot* pAttackedPlot, int iRange, DomainTypes eDomainType, bool bCheckCanAttack, bool bCheckCanMove)
{
	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	int	strSum = 0;

	for (int iX = -iRange; iX <= iRange; ++iX)
	{
		for (int iY = -iRange; iY <= iRange; ++iY)
		{
			CvPlot* pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iX, iY);
			if (pLoopPlot != NULL)
			{
				pUnitNode = pLoopPlot->headUnitNode();

				while (pUnitNode != NULL)
				{
					pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

					if (pLoopUnit != NULL && pLoopUnit->getOwnerINLINE() == getID())
					{
						if (!pLoopUnit->isDead())
						{
							bool bCanAttack = pLoopUnit->canAttack();

							if (!bCheckCanAttack || bCanAttack)
							{
								if (!bCheckCanMove || pLoopUnit->canMove())
									if (!bCheckCanMove || pAttackedPlot == NULL || pLoopUnit->canMoveInto(*pAttackedPlot, /*bAttack*/ true, /*bDeclareWar*/ true))
										if (eDomainType == NO_DOMAIN || pLoopUnit->getDomainType() == eDomainType)
											strSum += pLoopUnit->currEffectiveStr(pAttackedPlot, pLoopUnit);
							}
						}
					}
				}
			}
		}
	}

	return strSum;
}

int CvPlayerAI::AI_sumEnemyStrength(CvPlot* pPlot, int iRange, bool bAttack, DomainTypes eDomainType)
{
	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	int	strSum = 0;

	for (int iX = -iRange; iX <= iRange; ++iX)
	{
		for (int iY = -iRange; iY <= iRange; ++iY)
		{
			CvPlot* pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iX, iY);
			if (pLoopPlot != NULL)
			{
				pUnitNode = pLoopPlot->headUnitNode();

				while (pUnitNode != NULL)
				{
					pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

					if (pLoopUnit != NULL && pLoopUnit->isEnemy(getTeam(), pLoopPlot))
					{
						if (!pLoopUnit->isDead())
						{
							if (eDomainType == NO_DOMAIN || pLoopUnit->getDomainType() == eDomainType)
							{
								strSum += pLoopUnit->currEffectiveStr(pLoopPlot, bAttack ? pLoopUnit : NULL);
							}
						}
					}
				}
			}
		}
	}

	return strSum;
}

int CvPlayerAI::AI_setUnitAIStatesRange(CvPlot* pPlot, int iRange, UnitAIStates eNewUnitAIState, UnitAIStates eValidUnitAIState, const std::vector<UnitAITypes>& validUnitAITypes)
{
	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	int iCount = 0;

	for (int iX = -iRange; iX <= iRange; ++iX)
	{
		for (int iY = -iRange; iY <= iRange; ++iY)
		{
			CvPlot* pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iX, iY);
			if (pLoopPlot != NULL)
			{
				pUnitNode = pLoopPlot->headUnitNode();

				while (pUnitNode != NULL)
				{
					pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

					if (pLoopUnit != NULL && (eValidUnitAIState == NO_UNITAI_STATE || pLoopUnit->AI_getUnitAIState() == eValidUnitAIState))
					{
						if (std::find(validUnitAITypes.begin(), validUnitAITypes.end(), pLoopUnit->AI_getUnitAIType()) == validUnitAITypes.end())
						{
							pLoopUnit->AI_setUnitAIState(eNewUnitAIState);
							iCount++;
						}
					}
				}
			}
		}
	}
	return iCount;
}

//
// read object from a stream
// used during load
//
void CvPlayerAI::read(FDataStreamBase* pStream)
{
	CvSavegameReaderBase readerbase(pStream);
	CvSavegameReader reader(readerbase);

	read(reader);

	/// post load function - start - Nightinggale
	if (m_eID == (MAX_PLAYERS - 1))
	{
		// Done loading the last player
		// Execute the post load code
		postLoadGameFixes();
	}
	/// post load function - end - Nightinggale
}


//
// save object to a stream
// used during save
//
void CvPlayerAI::write(FDataStreamBase* pStream)
{
	CvSavegameWriterBase writerbase(pStream);
	CvSavegameWriter writer(writerbase);
	write(writer);
	writerbase.WriteFile();
}


int CvPlayerAI::AI_eventValue(EventTypes eEvent, const EventTriggeredData& kTriggeredData)
{
	CvEventTriggerInfo& kTrigger = GC.getEventTriggerInfo(kTriggeredData.m_eTrigger);
	CvEventInfo& kEvent = GC.getEventInfo(eEvent);

	int iNumCities = getNumCities();
	CvCity* pCity = getCity(kTriggeredData.m_iCityId);
	CvPlot* pPlot = GC.getMap().plotINLINE(kTriggeredData.m_iPlotX, kTriggeredData.m_iPlotY);
	CvUnit* pUnit = getUnit(kTriggeredData.m_iUnitId);

	int aiYields[NUM_YIELD_TYPES];

	for (int iI = 0; iI < NUM_YIELD_TYPES; iI++)
	{
		aiYields[iI] = 0;
	}

	if (NO_PLAYER != kTriggeredData.m_eOtherPlayer)
	{
		if (kEvent.isDeclareWar())
		{
			switch (AI_getAttitude(kTriggeredData.m_eOtherPlayer))
			{
			case ATTITUDE_FURIOUS:
			case ATTITUDE_ANNOYED:
			case ATTITUDE_CAUTIOUS:
				if (GET_TEAM(getTeam()).getDefensivePower() < GET_TEAM(GET_PLAYER(kTriggeredData.m_eOtherPlayer).getTeam()).getPower())
				{
					return -MAX_INT + 1;
				}
				break;
			case ATTITUDE_PLEASED:
			case ATTITUDE_FRIENDLY:
				return -MAX_INT + 1;
				break;
			}
		}
	}

	//Proportional to #turns in the game...
	//(AI evaluation will generally assume proper game speed scaling!)
	int iGameSpeedPercent = 100;

	int iValue = GC.getGameINLINE().getSorenRandNum(kEvent.getAIValue(), "AI Event choice");
	iValue += (getEventCost(eEvent, kTriggeredData.m_eOtherPlayer, false) + getEventCost(eEvent, kTriggeredData.m_eOtherPlayer, true)) / 2;
	if (kEvent.getUnitClass() != NO_UNITCLASS)
	{
		UnitTypes eUnit = (UnitTypes)GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(kEvent.getUnitClass());
		if (eUnit != NO_UNIT)
		{
			//Although AI_unitValue compares well within units, the value is somewhat independent of cost
			int iUnitValue = 0;
			for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_YIELD_TYPES; ++eYield)
			{
				iUnitValue += GC.getUnitInfo(eUnit).getYieldCost(eYield);
			}
			if (iUnitValue > 0)
			{
				iUnitValue *= 2;
			}
			else
			{
				iUnitValue = 200;
			}

			iUnitValue *= GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getTrainPercent();
			iValue += kEvent.getNumUnits() * iUnitValue;
		}
	}

	if (kEvent.isDisbandUnit())
	{
		CvUnit* pUnit = getUnit(kTriggeredData.m_iUnitId);
		if (NULL != pUnit)
		{
			int iUnitValue = 0;
			for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_YIELD_TYPES; ++eYield)
			{
				iUnitValue += pUnit->getUnitInfo().getYieldCost(eYield);
			}
			if (iUnitValue > 0)
			{
				iUnitValue *= 2;
			}
			else
			{
				iUnitValue = 200;
			}

			iUnitValue *= GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getTrainPercent();
			iValue -= iUnitValue;
		}
	}

	if (kEvent.getBuildingClass() != NO_BUILDINGCLASS)
	{
		BuildingTypes eBuilding = GC.getCivilizationInfo(getCivilizationType()).getCivilizationBuildings((BuildingClassTypes)kEvent.getBuildingClass());
		if (eBuilding != NO_BUILDING)
		{
			if (pCity)
			{
				//iValue += kEvent.getBuildingChange() * pCity->AI_buildingValue(eBuilding);
				int iBuildingValue = 0;
				for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
				{
					iBuildingValue += GC.getBuildingInfo(eBuilding).getYieldCost(iYield);
				}
				if (iBuildingValue > 0)
				{
					iBuildingValue *= 2;
				}
				else if (iBuildingValue == -1)
				{
					iBuildingValue = 300;
				}

				iBuildingValue *= GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getConstructPercent();
				iValue += kEvent.getBuildingChange() * iBuildingValue;
			}
		}
	}

	{	//Yield and other changes
		if (kEvent.getNumBuildingYieldChanges() > 0)
		{
			for (int iBuildingClass = 0; iBuildingClass < GC.getNumBuildingClassInfos(); ++iBuildingClass)
			{
				for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
				{
					aiYields[iYield] += kEvent.getBuildingYieldChange(iBuildingClass, iYield);
				}
			}
		}
	}

	// Score every yield that was collected. The old city-effect path only
	// added food/bells/crosses, and the global path never added aiYields at
	// all, so events like EVENT_BOOK_MORMON_2 looked worthless to the AI.
	int iYieldTurnValue = 0;
	for (int iYield = 0; iYield < NUM_YIELD_TYPES; ++iYield)
	{
		if (aiYields[iYield] != 0)
		{
			int iWeight = 2;
			if (iYield == YIELD_FOOD)
			{
				iWeight = 5;
			}
			else if (iYield == YIELD_BELLS)
			{
				iWeight = 3;
			}
			else if (iYield == YIELD_CROSSES)
			{
				iWeight = 1;
			}
			iYieldTurnValue += aiYields[iYield] * iWeight;
		}
	}

	if (kEvent.isCityEffect())
	{
		int iCityPopulation = -1;
		if (NULL != pCity)
		{
			iCityPopulation = pCity->getPopulation();
		}

		if (-1 == iCityPopulation)
		{
			//What is going on here?
			iCityPopulation = 5;
		}

		iValue += (iYieldTurnValue * 20 * iGameSpeedPercent) / 100;

		iValue += kEvent.getFood();
		iValue += kEvent.getFoodPercent() / 4;
		iValue += kEvent.getPopulationChange() * 30;
		iValue -= kEvent.getRevoltTurns() * (12 + iCityPopulation * 16);
		iValue += kEvent.getCulture() / 2;
		iValue += kEvent.getHealth() / 2; // R&R, ray, change for Health in Events
	}
	else if (!kEvent.isOtherPlayerCityEffect())
	{
		iValue += (iNumCities * iYieldTurnValue * 20 * iGameSpeedPercent) / 100;

		iValue += (kEvent.getFood() * iNumCities);
		iValue += (kEvent.getFoodPercent() * iNumCities) / 4;
		iValue += (kEvent.getPopulationChange() * iNumCities * 40);
		iValue += iNumCities * kEvent.getCulture() / 2;
		iValue += iNumCities * kEvent.getHealth() / 2; // R&R, ray, change for Health in Events
	}

	if (NULL != pPlot)
	{
		if (kEvent.getImprovementChange() > 0)
		{
			iValue += (30 * iGameSpeedPercent) / 100;
		}
		else if (kEvent.getImprovementChange() < 0)
		{
			iValue -= (30 * iGameSpeedPercent) / 100;
		}

		if (kEvent.getRouteChange() > 0)
		{
			iValue += (10 * iGameSpeedPercent) / 100;
		}
		else if (kEvent.getRouteChange() < 0)
		{
			iValue -= (10 * iGameSpeedPercent) / 100;
		}

		for (int i = 0; i < NUM_YIELD_TYPES; ++i)
		{
			if (0 != kEvent.getPlotExtraYield(i))
			{
				if (pPlot->getWorkingCity() != NULL)
				{
					FAssertMsg(pPlot->getWorkingCity()->getOwnerINLINE() == getID(), "Event creates a boni for another player?");
					aiYields[i] += kEvent.getPlotExtraYield(i);
				}
				else
				{
					iValue += (20 * 8 * kEvent.getPlotExtraYield(i) * iGameSpeedPercent) / 100;
				}
			}
		}
	}

	if (NULL != pUnit)
	{
		iValue += (2 * pUnit->baseCombatStr() * kEvent.getUnitExperience() * GC.getGameSpeedInfo(GC.getGameINLINE().getGameSpeedType()).getTrainPercent()) / 100;

		iValue -= 10 * kEvent.getUnitImmobileTurns();
	}

	{
		int iPromotionValue = 0;

		for (int i = 0; i < GC.getNumUnitCombatInfos(); ++i)
		{
			if (NO_PROMOTION != kEvent.getUnitCombatPromotion(i))
			{
				int iLoop;
				for (CvUnit* pLoopUnit = firstUnit(&iLoop); NULL != pLoopUnit; pLoopUnit = nextUnit(&iLoop))
				{
					if (pLoopUnit->getUnitCombatType() == i)
					{
						if (!pLoopUnit->isHasPromotion((PromotionTypes)kEvent.getUnitCombatPromotion(i)))
						{
							iPromotionValue += 5 * pLoopUnit->baseCombatStr();
						}
					}
				}

				iPromotionValue += iNumCities * 50;
			}
		}

		iValue += (iPromotionValue * iGameSpeedPercent) / 100;
	}

	int iOtherPlayerAttitudeWeight = 0;
	if (kTriggeredData.m_eOtherPlayer != NO_PLAYER)
	{
		iOtherPlayerAttitudeWeight = AI_getAttitudeWeight(kTriggeredData.m_eOtherPlayer);
		iOtherPlayerAttitudeWeight += 10 - GC.getGame().getSorenRandNum(20, "AI event value attitude");
	}

	if (NO_PLAYER != kTriggeredData.m_eOtherPlayer)
	{
		CvPlayerAI& kOtherPlayer = GET_PLAYER(kTriggeredData.m_eOtherPlayer);

		int iDiploValue = 0;
		//if we like this player then value positive attitude, if however we really hate them then
		//actually value negative attitude.
		iDiploValue += ((iOtherPlayerAttitudeWeight + 50) * kEvent.getAttitudeModifier() * GET_PLAYER(kTriggeredData.m_eOtherPlayer).getPower()) / std::max(1, getPower());

		if (kEvent.getTheirEnemyAttitudeModifier() != 0)
		{
			//Oh wow this sure is mildly complicated.
			TeamTypes eWorstEnemy = GET_TEAM(GET_PLAYER(kTriggeredData.m_eOtherPlayer).getTeam()).AI_getWorstEnemy();

			if (NO_TEAM != eWorstEnemy && eWorstEnemy != getTeam())
			{
			int iThirdPartyAttitudeWeight = GET_TEAM(getTeam()).AI_getAttitudeWeight(eWorstEnemy);

			//If we like both teams, we want them to get along.
			//If we like otherPlayer but not enemy (or vice-verca), we don't want them to get along.
			//If we don't like either, we don't want them to get along.
			//Also just value stirring up trouble in general.

			int iThirdPartyDiploValue = 50 * kEvent.getTheirEnemyAttitudeModifier();
			iThirdPartyDiploValue *= (iThirdPartyAttitudeWeight - 10);
			iThirdPartyDiploValue *= (iOtherPlayerAttitudeWeight - 10);
			iThirdPartyDiploValue /= 10000;

			if ((iOtherPlayerAttitudeWeight) < 0 && (iThirdPartyAttitudeWeight < 0))
			{
				iThirdPartyDiploValue *= -1;
			}

			iThirdPartyDiploValue /= 2;

			iDiploValue += iThirdPartyDiploValue;
		}
		}

		iDiploValue *= iGameSpeedPercent;
		iDiploValue /= 100;

		if (GC.getGameINLINE().isOption(GAMEOPTION_AGGRESSIVE_AI))
		{
			//What is this "relationships" thing?
			iDiploValue /= 2;
		}

		if (kEvent.isGoldToPlayer())
		{
			//If the gold goes to another player instead of the void, then this is a positive
			//thing if we like the player, otherwise it's a negative thing.
			int iGiftValue = (getEventCost(eEvent, kTriggeredData.m_eOtherPlayer, false) + getEventCost(eEvent, kTriggeredData.m_eOtherPlayer, true)) / 2;
			iGiftValue *= -iOtherPlayerAttitudeWeight;
			iGiftValue /= 110;

			iValue += iGiftValue;
		}

		if (kEvent.isDeclareWar())
		{
			int iWarValue = (GET_TEAM(getTeam()).getDefensivePower() - GET_TEAM(GET_PLAYER(kTriggeredData.m_eOtherPlayer).getTeam()).getPower());// / std::max(1, GET_TEAM(getTeam()).getDefensivePower());
			iWarValue -= 30 * AI_getAttitudeVal(kTriggeredData.m_eOtherPlayer);
		}

		if (kEvent.getMaxPillage() > 0)
		{
			int iPillageValue = (40 * (kEvent.getMinPillage() + kEvent.getMaxPillage())) / 2;
			//If we hate them, this is good to do.
			iPillageValue *= 25 - iOtherPlayerAttitudeWeight;
			iPillageValue *= iGameSpeedPercent;
			iPillageValue /= 12500;
		}

		iValue += (iDiploValue * iGameSpeedPercent) / 100;
	}

	int iThisEventValue = iValue;
	//XXX THIS IS VULNERABLE TO NON-TRIVIAL RECURSIONS!
	//Event A effects Event B, Event B effects Event A
	for (int iEvent = 0; iEvent < GC.getNumEventInfos(); ++iEvent)
	{
		if (kEvent.getAdditionalEventChance(iEvent) > 0)
		{
			if (iEvent == eEvent)
			{
				//Infinite recursion is not our friend.
				//Fortunately we have the event value for this event - sans values of other events
				//disabled or cleared. Hopefully no events will be that complicated...
				//Double the value since it's recursive.
				iValue += (kEvent.getAdditionalEventChance(iEvent) * iThisEventValue) / 50;
			}
			else
			{
				iValue += (kEvent.getAdditionalEventChance(iEvent) * AI_eventValue((EventTypes)iEvent, kTriggeredData)) / 100;
			}
		}

		if (kEvent.getClearEventChance(iEvent) > 0)
		{
			if (iEvent == eEvent)
			{
				iValue -= (kEvent.getClearEventChance(iEvent) * iThisEventValue) / 50;
			}
			else
			{
				iValue -= (kEvent.getClearEventChance(iEvent) * AI_eventValue((EventTypes)iEvent, kTriggeredData)) / 100;
			}
		}
	}

	iValue *= 100 + GC.getGameINLINE().getSorenRandNum(20, "AI Event choice");
	iValue /= 100;

	return iValue;
}

EventTypes CvPlayerAI::AI_chooseEvent(int iTriggeredId)
{
	EventTriggeredData* pTriggeredData = getEventTriggered(iTriggeredId);
	if (NULL == pTriggeredData)
	{
		return NO_EVENT;
	}

	CvEventTriggerInfo& kTrigger = GC.getEventTriggerInfo(pTriggeredData->m_eTrigger);

	int iBestValue = -MAX_INT;
	EventTypes eBestEvent = NO_EVENT;

	const InfoArray<EventTypes>& Events = kTrigger.getEvents();
	for (int i = 0; i < Events.getLength(); i++)
	{
		int iValue = -MAX_INT;
		if (Events.getEvent(i) != NO_EVENT)
		{
			CvEventInfo& kEvent = GC.getEventInfo(Events.getEvent(i));
			if (canDoEvent(Events.getEvent(i), *pTriggeredData))
			{
				iValue = AI_eventValue(Events.getEvent(i), *pTriggeredData);
			}
		}

		if (iValue > iBestValue)
		{
			iBestValue = iValue;
			eBestEvent = Events.getEvent(i);
		}
	}

	return eBestEvent;
}

void CvPlayerAI::AI_doNativeArmy(TeamTypes eTeam)
{
	CvTeamAI& kTeam = GET_TEAM(getTeam());
	FAssert(eTeam != NO_TEAM && eTeam != getTeam());

	int iTotalUnitCount = getTotalPopulation();

	int iGameTurn = GC.getGameINLINE().getGameTurn();
	int iEndTurn = GC.getGameINLINE().getEstimateEndTurn();

	iEndTurn *= GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getNativePacifismPercent();
	iEndTurn /= 100;

	int iOffensivePercent = std::max(40, 30 * iGameTurn * 2 / iEndTurn);
	int iCounterPercent = std::max(25, 15 * iGameTurn * 3 / iEndTurn);

	if (kTeam.AI_getWarPlan(eTeam) == WARPLAN_TOTAL || kTeam.AI_getWarPlan(eTeam) == WARPLAN_PREPARING_TOTAL)
	{
		iOffensivePercent *= 3;
		iOffensivePercent /= 2;
	}
	else if (kTeam.AI_getWarPlan(eTeam) == WARPLAN_ATTACKED_RECENT)
	{
		iOffensivePercent += 3;
		iOffensivePercent /= 4;
	}
	else
	{
		iCounterPercent += (iOffensivePercent * 1 + 2) / 3;
		iOffensivePercent = (iOffensivePercent * 2 + 1) / 3;
	}

	//First convert units which are already on the map.
	int iLoop;
	CvUnit* pLoopUnit;
	for (pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		CvArea* pArea = pLoopUnit->area();
		AreaAITypes eAreaAI = pArea->getAreaAIType(getTeam());
		if (pLoopUnit->AI_getUnitAIType() == UNITAI_DEFENSIVE)
		{
			int iValue = 0;

			int iAreaPopulation = pArea->getPopulationPerPlayer(getID()) + pArea->getUnitsPerPlayer(getID());

			CvCity* pLoopCity = pLoopUnit->plot()->getPlotCity();
			if (pLoopCity != NULL && pLoopCity->getOwnerINLINE() != getID())
			{
				pLoopCity = NULL;
			}
			if (pLoopCity == NULL || pLoopCity->AI_isDefended())
			{
				int iOffenseValue = iOffensivePercent * (iOffensivePercent - 100 * pArea->getNumAIUnits(getID(), UNITAI_OFFENSIVE) / iAreaPopulation);
				int iCounterValue = iCounterPercent * (iCounterPercent - 100 * pArea->getNumAIUnits(getID(), UNITAI_COUNTER) / iAreaPopulation);

				if (iOffenseValue >= 0 && iOffenseValue >= iCounterValue)
				{
					pLoopUnit->AI_setUnitAIType(UNITAI_OFFENSIVE);
				}
				else if (iCounterValue > 0)
				{
					pLoopUnit->AI_setUnitAIType(UNITAI_COUNTER);
				}
			}
		}
		else if (eAreaAI == AREAAI_NEUTRAL)
		{
			pLoopUnit->AI_setUnitAIType(UNITAI_DEFENSIVE);
		}
	}

	int iBestValue = 0;
	CvCity* pBestCity = NULL;

	int iInfiniteLoop = 0;
	while (true)
	{
		int iBestValue = 0;
		UnitAITypes eBestUnitAI = NO_UNITAI;
		CvCity* pBestCity = NULL;


		CvCity* pLoopCity;
		for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
		{
			if (pLoopCity->getPopulation() > 1)
			{
				CvArea* pArea = pLoopCity->area();
				AreaAITypes eAreaAI = pArea->getAreaAIType(getTeam());

				if (eAreaAI != AREAAI_NEUTRAL)
				{
					int iAreaPopulation = pArea->getPopulationPerPlayer(getID()) + pArea->getUnitsPerPlayer(getID());
					int iOffenseValue = iOffensivePercent * (iOffensivePercent - 100 * pArea->getNumAIUnits(getID(), UNITAI_OFFENSIVE) / iAreaPopulation);
					int iCounterValue = iCounterPercent * (iCounterPercent - 100 * pArea->getNumAIUnits(getID(), UNITAI_COUNTER) / iAreaPopulation);

					if (iOffenseValue > 0 || iCounterValue > 0)
					{
						int iValue = (100 * pLoopCity->getPopulation()) / pLoopCity->getHighestPopulation();

						iValue /= (3 + kTeam.AI_enemyCityDistance(pLoopCity->plot()));

						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							pBestCity = pLoopCity;
							eBestUnitAI = (iOffenseValue > iCounterValue) ? UNITAI_OFFENSIVE : UNITAI_COUNTER;
						}
					}
				}
			}
		}

		if (pBestCity == NULL)
		{
			break;
		}
		if (iInfiniteLoop > 100)
		{
			FAssertMsg(false, "Infinite Loop in Native War Preperations");
			break;
		}
		CvUnit* pEjectUnit = pBestCity->AI_bestPopulationUnit(eBestUnitAI);
		if (pEjectUnit == NULL)
		{
			FAssertMsg(false, "Could not eject unit");
			break;
		}
	}
}

CvCity* CvPlayerAI::AI_getPrimaryCity()
{
	int iLoop;
	CvCity* pCity = firstCity(&iLoop);
	if (pCity != NULL)
	{
		return pCity;
	}
	return NULL;

}

int CvPlayerAI::AI_getOverpopulationPercent()
{
	int iCityPopulation = getTotalPopulation() - getNumUnits();

	if (iCityPopulation <= 0)
	{
		return 0;
	}

	int iTargetPop = (iCityPopulation + getNumCities()) * (100 + 100 / GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAITrainPercent());
	iTargetPop /= 100;

	return (100 * getNumUnits() / std::max(1, iTargetPop) - 100);
}

int CvPlayerAI::AI_countNumHomedUnits(CvCity* pCity, UnitAITypes eUnitAI, UnitAIStates eUnitAIState)
{
	int iCount = 0;

	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pCity == NULL || pLoopUnit->getHomeCity() == pCity)
		{
			if (eUnitAI == NO_UNITAI || pLoopUnit->AI_getUnitAIType() == eUnitAI)
			{
				if (eUnitAIState == NO_UNITAI_STATE || pLoopUnit->AI_getUnitAIState() == eUnitAIState)
				{
					iCount++;
				}
			}
		}
	}

	return iCount;
}

// R&R, ray, Natives raiding party - START
bool CvPlayerAI::AI_isPotentialRaidTarget(PlayerTypes ePlayer)
{
	if (!isNative())
	{
		return false;
	}

	if (GET_PLAYER(ePlayer).isNative())
	{
		return false;
	}

	if (AI_getAttitude(ePlayer) > GC.getLeaderHeadInfo(getPersonalityType()).getNativeRaidAttitudeThreshold())
	{
		return false;
	}

	return true;
}

bool CvPlayerAI::AI_hasPotentialRaidTarget()
{
	for (int iPlayer = 0; iPlayer < MAX_PLAYERS; ++iPlayer)
	{
		if (AI_isPotentialRaidTarget((PlayerTypes)iPlayer))
		{
			return true;
		}
	}

	return false;
}

int CvPlayerAI::AI_getNumPotentialRaids()
{
	int iCount = 0;

	if (isNative())
	{
		for (int iPlayer = 0; iPlayer < MAX_PLAYERS; ++iPlayer)
		{
			CvPlayer& kLoopPlayer = GET_PLAYER((PlayerTypes)iPlayer);
			if (AI_isPotentialRaidTarget(kLoopPlayer.getID()))
			{
				if (!kLoopPlayer.isNative())
				{
					int iAttitudeVal = AI_getAttitudeVal(kLoopPlayer.getID());
					iAttitudeVal += GC.getNATIVE_POTENTIAL_RAID_TARGET_THRESHOLD();

					if (iAttitudeVal < 0)
					{
						iCount += -(iAttitudeVal);
					}
				}
			}
		}
	}

	return iCount;
}
// R&R, ray, Natives raiding party - END

void CvPlayerAI::AI_doMilitaryStrategy()
{

	//Iterate over every enemy city.
	//If we are sieging that city then evaluate the strength of our units vs the strength of the local defense (blah blah blah)

	//If we are in a strong position : CHARGE
	//If we are waiting for reinforcements: CAMP
	//If we are weak and no reinforcements are coming: RETREAT

	std::vector<UnitAITypes> militaryUnitAIs;

	// Counter units do not make use of UnitAIStates
	//militaryUnitAIs.push_back(UNITAI_COUNTER);
	militaryUnitAIs.push_back(UNITAI_OFFENSIVE);

	CvTeamAI& kTeam = GET_TEAM(getTeam());
	for (int iPlayer = 0; iPlayer < MAX_PLAYERS; ++iPlayer)
	{
		CvPlayer& kLoopPlayer = GET_PLAYER((PlayerTypes)iPlayer);
		WarPlanTypes eWarPlan = kTeam.AI_getWarPlan(kLoopPlayer.getTeam());
		if (kLoopPlayer.isAlive() && (kLoopPlayer.getTeam() != getTeam()) && (eWarPlan != NO_WARPLAN))
		{
			bool bKnockKnock_WhoseThere_Monty_MontyWho_MontyAndHisHordeDieDieDie = false;
			int iLoop;
			CvCity* pLoopCity;
			for (pLoopCity = kLoopPlayer.firstCity(&iLoop); pLoopCity != NULL; pLoopCity = kLoopPlayer.nextCity(&iLoop))
			{
				if (pLoopCity->plot()->isVisible(getTeam(), false) || isNative())
				{
					int iOurStrength = AI_sumAttackerStrength(pLoopCity->plot(), pLoopCity->plot(), 3, DOMAIN_LAND);
					if ((iOurStrength > 0) && (GC.getGameINLINE().getSorenRandNum(100, "AI new wave") < 25))
					{
						//Only consider enemy strength for total war, otherwise dribble on in.
						int iEnemyStrength = (eWarPlan == WARPLAN_TOTAL) ? AI_sumEnemyStrength(pLoopCity->plot(), 1, false, DOMAIN_LAND) : 0;
						if ((iOurStrength * 100) > iEnemyStrength * 150)
						{
							AI_setUnitAIStatesRange(pLoopCity->plot(), 3, UNITAI_STATE_CHARGING, UNITAI_STATE_GROUPING, militaryUnitAIs);
							bKnockKnock_WhoseThere_Monty_MontyWho_MontyAndHisHordeDieDieDie = true;
						}
					}
				}
			}

			if (bKnockKnock_WhoseThere_Monty_MontyWho_MontyAndHisHordeDieDieDie)
			{
				if (!atWar(getTeam(), kLoopPlayer.getTeam()) && kTeam.AI_mayDeclareWarAsNative(kLoopPlayer.getTeam()))
				{
					WarPlanTypes eNewWarplan = WARPLAN_TOTAL;

					//Do the whole extortion thing
					kTeam.declareWar(kLoopPlayer.getTeam(), true, eNewWarplan);
				}
			}
		}
	}
	return;
}

void CvPlayerAI::AI_doSuppressRevolution()
{
	const HandicapTypes eHandicap = GC.getGameINLINE().getHandicapType();

	// First‐time init: set counters and reveal map
	if (AI_getLastWave() == -1)
	{
		AI_setLastWave(0);
		AI_setWaveIndex(0);
		for (int i = 0; i < GC.getMap().numPlotsINLINE(); ++i)
		{
			CvPlot* pPlot = GC.getMap().plotByIndexINLINE(i);
			if (pPlot)
			{
				pPlot->setRevealed(getTeam(), true, false, NO_TEAM);
			}
				}
			}

	// Early exit if all waves sent
	const int iNumWaves = GC.getHandicapInfo(eHandicap).getNumWaves();
	const int iWaveIndex = AI_getWaveIndex();
	if (iWaveIndex >= iNumWaves)
	{
		if (AI_isStrategy(STRATEGY_SMALL_WAVES))
		{
			AI_clearStrategy(STRATEGY_SMALL_WAVES);
		}
		return;
	}

	// Find our target colony that we're at war with
	bool bFound = false;
	PlayerTypes eColony = NO_PLAYER;
	for (int iPlayer = 0; iPlayer < MAX_PLAYERS; ++iPlayer)
	{
		const CvPlayer& kLoop = GET_PLAYER((PlayerTypes)iPlayer);
		if (kLoop.isAlive() &&
			GET_TEAM(getTeam()).isParentOf(kLoop.getTeam()) &&
			atWar(getTeam(), kLoop.getTeam()))
	{
			eColony = (PlayerTypes)iPlayer;
			bFound = true;
			break;
		}
	}
	if (!bFound) return;

	const CvPlayerAI& kColony = GET_PLAYER(eColony);

	// Pick strategy
	if (!AI_isAnyStrategy())
	{
		AI_setStrategy(STRATEGY_SMALL_WAVES);
		int iT = GC.getGameINLINE().getSorenRandNum(5, "Choose");
		if (iT < 3)
				AI_setStrategy(STRATEGY_CONCENTRATED_ATTACK);
		else
				AI_setStrategy(STRATEGY_DISTRIBUTED_ATTACK);
	}

	// Occasional swap between attack srategies
	if (GC.getGameINLINE().getSorenRandNum(100, "Swap") < 33)
	{
		if (AI_isStrategy(STRATEGY_CONCENTRATED_ATTACK) &&
			AI_getStrategyDuration(STRATEGY_CONCENTRATED_ATTACK) > 7)
		{
			AI_clearStrategy(STRATEGY_CONCENTRATED_ATTACK);
			AI_setStrategy(STRATEGY_DISTRIBUTED_ATTACK);
		}
		else if (AI_isStrategy(STRATEGY_DISTRIBUTED_ATTACK) &&
			AI_getStrategyDuration(STRATEGY_DISTRIBUTED_ATTACK) > 4)
		{
			AI_clearStrategy(STRATEGY_DISTRIBUTED_ATTACK);
			AI_setStrategy(STRATEGY_CONCENTRATED_ATTACK);
		}
	}

	// If concentrated, choose a target once
	if (AI_isStrategy(STRATEGY_CONCENTRATED_ATTACK) &&
		AI_getStrategyData(STRATEGY_CONCENTRATED_ATTACK) == -1)
	{
		int iBest = 0;
		CvPlot* pBest = NULL;
		int iIter = 0;
		for (const CvCity* pCity = kColony.firstCity(&iIter); pCity; pCity = kColony.nextCity(&iIter))
		{
			if (pCity->plot()->getNearestEurope() != NO_EUROPE)
			{
				int iVal = pCity->getHighestPopulation() * 50 + pCity->plot()->getCrumbs();
				iVal *= 25 + GC.getGameINLINE().getSorenRandNum(75, "Pick");
				if (iVal > iBest)
				{
					iBest = iVal;
					pBest = pCity->plot();
				}
			}
		}
		if (pBest)
		{
			AI_setStrategy(STRATEGY_CONCENTRATED_ATTACK,
				GC.getMap().plotNumINLINE(pBest->getX_INLINE(), pBest->getY_INLINE()));
		}
	}

	// Count ships & soldiers in Europe
	std::vector<CvUnit*> ships, soldiers;
	int iShipCount = 0, iSoldierCount = 0, iCargo = 0;
	int iLoop = 0;
	for (CvUnit* pU = firstUnit(&iLoop); pU; pU = nextUnit(&iLoop))
		{
		if (pU->getDomainType() == DOMAIN_SEA &&
			pU->getUnitTravelState() == UNIT_TRAVEL_STATE_IN_EUROPE)
			{
			ships.push_back(pU);
			++iShipCount;
			iCargo += pU->cargoSpace();
			}
	    }
	std::vector<int> shuffle(getNumEuropeUnits());
	for (int i = 0; i < getNumEuropeUnits(); ++i)
		shuffle[i] = i;
	GC.getGameINLINE().getSorenRand().shuffleArray(shuffle, NULL);
	for (int i = 0; i < getNumEuropeUnits(); ++i)
	{
		CvUnit* pU = getEuropeUnit(shuffle[i]);
		if (pU && pU->getDomainType() == DOMAIN_LAND)
		{
			soldiers.push_back(pU);
			++iSoldierCount;
		}
	}
	if (iShipCount == 0)
		return;

	// Delay logic: first wave fires immediately; later only after iWaveTurns
	const int iWaveTurns = GC.getHandicapInfo(eHandicap).getWaveTurns();
	const int iLast = AI_getLastWave();
	if (iLast > 0 && iLast < iWaveTurns)
		{
			return;
		}

	// Compute how many to send this wave
	int iShipsToLaunch = 0;
	if (AI_isStrategy(STRATEGY_SMALL_WAVES))
	{
		int pctSent = 0;
		for (int i = 0; i < iWaveIndex; ++i)
			pctSent += GC.getHandicapInfo(eHandicap).getWaves(i);

		if (iWaveIndex + 1 == iNumWaves || pctSent > 99)
		{
			iShipsToLaunch = iShipCount;
		}
		else
		{
			const int base = (iShipCount * 100) / std::max(1, 100 - pctSent);
			iShipsToLaunch = (base * GC.getHandicapInfo(eHandicap).getWaves(iWaveIndex)) / 100;
		}
	}
	iShipsToLaunch = std::min(iShipsToLaunch, iShipCount);

	int iSoldiersToLoad = 0;
	int iMaxCargo = (iShipsToLaunch * iCargo) / iShipCount;
	iSoldiersToLoad = std::min(iSoldierCount, iMaxCargo);

	// Load & dispatch
	int iLoaded = 0;
	for (int i = 0; i < iShipsToLaunch; ++i)
	{
		CvUnit* const pShip = ships[i];
		while (iLoaded < iSoldiersToLoad && !pShip->isFull())
	{
			loadUnitFromEurope(soldiers[iLoaded++], pShip);
	}
		CvPlot* const pTarget = AI_getImperialShipSpawnPlot();
		if (!pShip->atPlot(pTarget))
		{
			pShip->setXY(pTarget->getX_INLINE(), pTarget->getY_INLINE(), false, false, false);
		}
		pShip->crossOcean(UNIT_TRAVEL_STATE_FROM_EUROPE);
	}

	// Advance to next wave & reset counter
	AI_setWaveIndex(std::min(iWaveIndex + 1, iNumWaves));
	AI_setLastWave(0);
}

void CvPlayerAI::AI_doUnitAIWeights()
{
	if ((GC.getGame().getGameTurn() == 2) || (GC.getGameINLINE().getSorenRandNum(50, "AI do Unit AI Weight Calculations") == 0))
	for (int i = 0; i < NUM_UNITAI_TYPES; ++i)
	{
		int iWeight = 90 + GC.getGameINLINE().getSorenRandNum(20, "AI Unit AI Weights");

		m_em_iUnitAIStrategyWeights.set((UnitAITypes)i, iWeight);
	}
}

void CvPlayerAI::AI_doEmotions()
{
	CvMap& kMap = GC.getMap();

	std::vector<short> const &distanceMap = *AI_getDistanceMap();

	int iGreedValue = 0;
	int iAnxietyValue = 0;
	int iAngerValue = 0;
	int iEnvyValue = 0;

	for (int i = 0; i < kMap.numPlotsINLINE(); ++i)
	{
		CvPlot* pLoopPlot = kMap.plotByIndexINLINE(i);
		if (distanceMap[i] != -1)
		{
			if (!pLoopPlot->isOwned())
			{
				if (pLoopPlot->getBonusType() != NO_BONUS)
				{
					if (pLoopPlot->isWater())
					{
						iGreedValue += 1;
					}
					else
					{
						iGreedValue += 2;
					}
				}
			}
			if (pLoopPlot->isVisible(getTeam(), false))
			{
				int iEnemyUnits = 0;
				int iFriendlyUnits = 0;
				int iNeutralColonialUnits = 0;
				int iTreasureUnits = 0;


				CLLNode<IDInfo>* pUnitNode;
				CvUnit* pLoopUnit;
				pUnitNode = pLoopPlot->headUnitNode();

				while (pUnitNode != NULL)
				{
					pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

					if (pLoopUnit == NULL)
					{
						continue;
					}

					if (pLoopUnit->getTeam() == getTeam())
					{
						iFriendlyUnits++;
					}
					else if (pLoopUnit->isEnemy(getTeam(), pLoopPlot))
					{
						iEnemyUnits++;
					}
					else
					{
						if (!pLoopUnit->isNative())
						{
							iNeutralColonialUnits++;
						}
					}

					if (pLoopUnit->getTeam() != getTeam())
					{
						if ((pLoopUnit->getUnitInfo().isTreasure()) && pLoopUnit->getYieldStored() > 0)
						{
							iEnvyValue += pLoopUnit->getYieldStored() / 40;
						}
					}

					if (pLoopPlot->getOwnerINLINE() == getID())
					{
						iAnxietyValue += 5 * iEnemyUnits;
						if (isNative())
						{
							iAngerValue += 3 * iNeutralColonialUnits;
						}
					}
					else
					{
						if (isNative())
						{
							iAngerValue += iNeutralColonialUnits;
						}
						iAngerValue += 1 * iEnemyUnits;
						iAnxietyValue += 1 * iEnemyUnits;
					}
				}
			}
		}
	}
	iGreedValue /= 2;
	AI_changeEmotion(EMOTION_GREED, iGreedValue);
	AI_changeEmotion(EMOTION_ANXIETY, iAnxietyValue);
	AI_changeEmotion(EMOTION_ANGER, iAngerValue);

	//Anxiety
	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (!pLoopCity->AI_isDefended())
		{
			AI_changeEmotion(EMOTION_ANXIETY, pLoopCity->getPopulation());
		}
	}
}

void CvPlayerAI::AI_doStrategy()
{
	int iGameTurn = GC.getGameINLINE().getGameTurn();
	CvTeamAI& kTeam = GET_TEAM(getTeam());

	int iPercent = 10000 / GC.getGame().AI_adjustedTurn(100);;

	if (getParent() != NO_PLAYER)
	{
		if (iGameTurn == 1)
		{
			AI_setStrategy(STRATEGY_CASH_FOCUS);
			//AI_setStrategy(STRATEGY_SELL_TO_NATIVES);	// TAC - AI Economy - koma13
		}

		if (iGameTurn == GC.getGameINLINE().AI_adjustedTurn(10))
		{
			//Set initial strategies.
			if (GC.getGame().getSorenRandNum(100, "AI Fast Bells") < 30)
			{
				AI_setStrategy(STRATEGY_FAST_BELLS);
			}

			// TAC - AI City Sites - koma13 - START
			/*
			if (GC.getGameINLINE().getSorenRandNum(100, "AI Dense Spacing") < 50)
			{
				AI_setStrategy(STRATEGY_DENSE_CITY_SPACING);
			}
			*/
			// TAC - AI City Sites - koma13 - END
		}

		if (!AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
		{
			if (!AI_isStrategy(STRATEGY_FAST_BELLS) && GC.getGameINLINE().getSorenRandNum(10000, "AI Fast Bells") < (2 * iPercent))
			{
				AI_setStrategy(STRATEGY_FAST_BELLS);
			}

			if (iGameTurn > GC.getGameINLINE().AI_adjustedTurn(35))
			{
				if (AI_isStrategy(STRATEGY_CASH_FOCUS) && GC.getGameINLINE().getSorenRandNum(10000, "AI Cash Focus") < (3 * iPercent))
				{
					AI_clearStrategy(STRATEGY_CASH_FOCUS);
				}
			}

			// TAC - AI Economy - koma13 - START
			/*
			if (!AI_isStrategy(STRATEGY_SELL_TO_NATIVES) && GC.getGameINLINE().getSorenRandNum(10000, "AI Sell to Natives") < (6 * iPercent))
			{
				AI_setStrategy(STRATEGY_SELL_TO_NATIVES);
			}
			*/
			// TAC - AI Economy - koma13 - END

			// TAC - AI City Sites - koma13 - START
			/*
			if (GC.getGameINLINE().getSorenRandNum(10000, "AI toggle spacing strategy") < (4 * iPercent))
			{
				if (AI_isStrategy(STRATEGY_DENSE_CITY_SPACING))
				{
					AI_clearStrategy(STRATEGY_DENSE_CITY_SPACING);
				}
				else
				{
					AI_setStrategy(STRATEGY_DENSE_CITY_SPACING);
				}
			}
			*/
			// TAC - AI City Sites - koma13 - END
		}

		// TAC - AI Military Buildup - koma13 - START
		if (AI_isStrategy(STRATEGY_MILITARY_BUILDUP))
		{
			if ((AI_bestBuildupUnitAI() == NO_UNITAI) || !canTradeWithEurope())
			{
				AI_clearStrategy(STRATEGY_MILITARY_BUILDUP);
			}
		}
		// TAC - AI Military Buildup - koma13 - END

		if (iGameTurn > GC.getGameINLINE().AI_adjustedTurn(100))
		{

			int iProb = 3 * iPercent;
			int iRebelPercent = GET_TEAM(getTeam()).getRebelPercent();
			iProb *= (100 + iRebelPercent + ((200 * iGameTurn) / GC.getGameINLINE().getEstimateEndTurn()));
			iProb /= 100;

			// TAC - AI Revolution - koma13 - START
			int iRebelsNeeded = GC.getAI_REBELS_FOR_REVOLUTION();

			if (iRebelPercent < iRebelsNeeded)
			{
				iProb = 0;
			}

			if (!AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
			{
				if (GC.getGameINLINE().getSorenRandNum(10000, "AI Start Revolution") < iProb)
				{
					AI_setStrategy(STRATEGY_REVOLUTION_PREPARING);
					AI_clearStrategy(STRATEGY_FAST_BELLS);
				}
			}
			/*
			else if (!AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
			{
				if ((iRebelPercent > 70) &&  (AI_getStrategyDuration(STRATEGY_REVOLUTION_PREPARING) > GC.getGameINLINE().AI_adjustedTurn(20)))
				{
					if (GC.getGameINLINE().getSorenRandNum(10000, "AI Start Revolution") < iProb * 6)
					{
						AI_setStrategy(STRATEGY_REVOLUTION_DECLARING);
					}
				}
			}
			else
			{
				FAssert(AI_isStrategy(STRATEGY_REVOLUTION_DECLARING));
				if (AI_getStrategyDuration(STRATEGY_REVOLUTION_DECLARING) > GC.getGameINLINE().AI_adjustedTurn(20))
				{
					int iValue = iRebelPercent + 100 * AI_getStrategyDuration(STRATEGY_REVOLUTION_DECLARING) / GC.getGameINLINE().AI_adjustedTurn(50);

					if (iValue > 125)
					{
						if (AI_totalDefendersNeeded(NULL) - AI_totalUnitAIs(UNITAI_OFFENSIVE) - AI_totalUnitAIs(UNITAI_COUNTER) - getNumCities() <= 0)
						{
							if (kTeam.canDoRevolution())
							{
								kTeam.doRevolution();
								AI_setStrategy(STRATEGY_REVOLUTION);
							}
						}
					}
				}
			}
			*/

			else if (!AI_isStrategy(STRATEGY_REVOLUTION_DECLARING))
			{
				if ((iRebelPercent > iRebelsNeeded) &&  (AI_getStrategyDuration(STRATEGY_REVOLUTION_PREPARING) > GC.getGameINLINE().AI_adjustedTurn(20)))
				{
					if (getEuropeMilitary() < (NBMOD_GetColonialMilitaryValue() * AI_getColonialMilitaryModifier()) / 100)
					{
						AI_setStrategy(STRATEGY_REVOLUTION_DECLARING);
						//AI_clearStrategy(STRATEGY_GET_A_SHIP);	// TAC - AI Purchasing military units - koma13
					}
				}
			}
			else
			{
				FAssert(AI_isStrategy(STRATEGY_REVOLUTION_DECLARING));
				if (AI_getStrategyDuration(STRATEGY_REVOLUTION_DECLARING) > GC.getGameINLINE().AI_adjustedTurn(20))
				{
					int iValue = iRebelPercent + 100 * AI_getStrategyDuration(STRATEGY_REVOLUTION_DECLARING) / GC.getGameINLINE().AI_adjustedTurn(50);

					if (iValue > 100)
					{
						// We have to take into account the determined trait, promotions, bonus vs. the king, ability to
						// keep producing troops / weapons etc. The AI can probably declare with a significantly smaller army than the king and still win.
						if (getEuropeMilitary() < (NBMOD_GetColonialMilitaryValue() * AI_getColonialMilitaryModifier()) / 100)
						{
							// Erik: This is wrong, if we have any plans, just cancel them and declare instead.
							// it's too late to try to conquer someone. If we're are war with indians we can
							// just choose a civic that gives instant peace.
							//bool bAtWar = (GET_TEAM(getTeam()).getAnyWarPlanCount() > 0);
							//if (!bAtWar)
							{
								if (kTeam.canDoRevolution())
								{
									kTeam.doRevolution();
									AI_setStrategy(STRATEGY_REVOLUTION);
								}
							}
						}
					}
				}
			}
			// TAC - AI Revolution - koma13 - END
		}
	}

	if (isNative())
	{
		if (AI_isStrategy(STRATEGY_DIE_FIGHTING))
		{
			if (AI_getNumAIUnits(UNITAI_OFFENSIVE) == 0)
			{
				AI_clearStrategy(STRATEGY_DIE_FIGHTING);
			}
		}
		else if (kTeam.getAnyWarPlanCount() > 0)
		{
			for (int i = 0; i < MAX_PLAYERS; ++i)
			{
				PlayerTypes eLoopPlayer = (PlayerTypes)i;
				if (GET_PLAYER(eLoopPlayer).isAlive())
				{
					TeamTypes eLoopTeam = GET_PLAYER(eLoopPlayer).getTeam();
					if (kTeam.isAtWar((TeamTypes)i))
					{
						WarPlanTypes eWarPlan = kTeam.AI_getWarPlan(eLoopTeam);
						int iDuration = kTeam.AI_getWarPlanStateCounter(eLoopTeam);

						bool bRazedCity = AI_getMemoryAttitude(eLoopPlayer, MEMORY_RAZED_CITY) > 0;

						if (eWarPlan == WARPLAN_ATTACKED || eWarPlan == WARPLAN_TOTAL)
						{
							AI_setStrategy(STRATEGY_DIE_FIGHTING);
						}
					}
				}
			}
		}
	}
}

int CvPlayerAI::AI_countDeadlockedBonuses(CvPlot* pPlot)
{
    CvPlot* pLoopPlot;
    CvPlot* pLoopPlot2;
    int iDX, iDY;
    int iI;

    int iMinRange = GC.getMIN_CITY_RANGE();
    int iRange = iMinRange * 2;
    int iCount = 0;

    for (iDX = -(iRange); iDX <= iRange; iDX++)
    {
        for (iDY = -(iRange); iDY <= iRange; iDY++)
        {
            if (plotDistance(iDX, iDY, 0, 0) > CITY_PLOTS_RADIUS)
            {
                pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);

                if (pLoopPlot != NULL)
                {
                    if (pLoopPlot->getBonusType() != NO_BONUS)
                    {
                        if (!pLoopPlot->isCityRadius() && ((pLoopPlot->area() == pPlot->area()) || pLoopPlot->isWater()))
                        {
                            bool bCanFound = false;
                            bool bNeverFound = true;
                            //potentially blockable resource
                            //look for a city site within a city radius
                            for (iI = 0; iI < NUM_CITY_PLOTS; iI++)
                            {
                                pLoopPlot2 = plotCity(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE(), iI);
                                if (pLoopPlot2 != NULL)
                                {
                                    //canFound usually returns very quickly
                                    if (canFound(pLoopPlot2->coord(), false))
                                    {
                                        bNeverFound = false;
                                        if (stepDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pLoopPlot2->getX_INLINE(), pLoopPlot2->getY_INLINE()) > iMinRange)
                                        {
                                            bCanFound = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (!bNeverFound && !bCanFound)
                            {
                                iCount++;
                            }
                        }
                    }
                }
            }
        }
    }

    return iCount;
}

int CvPlayerAI::AI_getOurPlotStrength(CvPlot* pPlot, int iRange, bool bDefensiveBonuses, bool bTestMoves) const
{
	PROFILE_FUNC();

	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	CvPlot* pLoopPlot;
	int iValue;
	int iDistance;
	int iDX, iDY;

	iValue = 0;

	for (iDX = -(iRange); iDX <= iRange; iDX++)
	{
		for (iDY = -(iRange); iDY <= iRange; iDY++)
		{
			pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);

			if (pLoopPlot != NULL)
			{
				if (pLoopPlot->area() == pPlot->area())
				{
				    iDistance = stepDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
					pUnitNode = pLoopPlot->headUnitNode();

					while (pUnitNode != NULL)
					{
						pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

						if (pLoopUnit != NULL && pLoopUnit->getOwnerINLINE() == getID())
						{
							if ((bDefensiveBonuses && pLoopUnit->canDefend()) || pLoopUnit->canAttack())
							{
								if (!(pLoopUnit->isInvisible(getTeam(), false)))
								{
									if (pLoopUnit->atPlot(pPlot) || pLoopUnit->canMoveInto(*pPlot) || pLoopUnit->canMoveInto(*pPlot, /*bAttack*/ true))
									{
										if (!bTestMoves)
										{
											iValue += pLoopUnit->currEffectiveStr((bDefensiveBonuses ? pPlot : NULL), NULL);
										}
										else
										{
											if (pLoopUnit->baseMoves() >= iDistance)
											{
												iValue += pLoopUnit->currEffectiveStr((bDefensiveBonuses ? pPlot : NULL), NULL);
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}


	return iValue;
}

int CvPlayerAI::AI_getEnemyPlotStrength(CvPlot* pPlot, int iRange, bool bDefensiveBonuses, bool bTestMoves)
{
	PROFILE_FUNC();

	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	CvPlot* pLoopPlot;
	int iValue;
	int iDistance;
	int iDX, iDY;

	iValue = 0;

	for (iDX = -(iRange); iDX <= iRange; iDX++)
	{
		for (iDY = -(iRange); iDY <= iRange; iDY++)
		{
			pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);

			if (pLoopPlot != NULL)
			{
				if (pLoopPlot->area() == pPlot->area())
				{
				    iDistance = stepDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
					pUnitNode = pLoopPlot->headUnitNode();

					while (pUnitNode != NULL)
					{
						pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

						if (pLoopUnit != NULL && atWar(pLoopUnit->getTeam(), getTeam()))
						{
							if ((bDefensiveBonuses && pLoopUnit->canDefend()) || pLoopUnit->canAttack())
							{
								if (!(pLoopUnit->isInvisible(getTeam(), false)))
								{
								    if (pPlot->isValidDomainForAction(*pLoopUnit))
								    {
                                        if (!bTestMoves)
                                        {
                                            iValue += pLoopUnit->currEffectiveStr((bDefensiveBonuses ? pPlot : NULL), NULL);
                                        }
                                        else
                                        {
                                            int iDangerRange = pLoopUnit->baseMoves();
                                            iDangerRange += ((pLoopPlot->isValidRoute(pLoopUnit)) ? 1 : 0);
                                            if (iDangerRange >= iDistance)
                                            {
                                                iValue += pLoopUnit->currEffectiveStr((bDefensiveBonuses ? pPlot : NULL), NULL);
                                            }
                                        }
								    }
								}
							}
						}
					}
				}
			}
		}
	}


	return iValue;

}

int CvPlayerAI::AI_goldToUpgradeAllUnits(int iExpThreshold)
{
	if (m_iUpgradeUnitsCacheTurn == GC.getGameINLINE().getGameTurn() && m_iUpgradeUnitsCachedExpThreshold == iExpThreshold)
	{
		return m_iUpgradeUnitsCachedGold;
	}

	int iTotalGold = 0;

	CvCivilizationInfo& kCivilizationInfo = GC.getCivilizationInfo(getCivilizationType());

	// cache the value for each unit type
	std::vector<int> aiUnitUpgradePrice(GC.getNumUnitInfos(), 0);	// initializes to zeros

	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		// if experience is below threshold, skip this unit
		if (pLoopUnit->getExperience() < iExpThreshold)
		{
			continue;
		}

		UnitTypes eUnitType = pLoopUnit->getUnitType();

		// check cached value for this unit type
		int iCachedUnitGold = aiUnitUpgradePrice[eUnitType];
		if (iCachedUnitGold != 0)
		{
			// if positive, add it to the sum
			if (iCachedUnitGold > 0)
			{
				iTotalGold += iCachedUnitGold;
			}

			// either way, done with this unit
			continue;
		}

		int iUnitGold = 0;
		int iUnitUpgradePossibilities = 0;

		UnitAITypes eUnitAIType = pLoopUnit->AI_getUnitAIType();
		CvArea* pUnitArea = pLoopUnit->area();
		int iUnitValue = AI_unitValue(eUnitType, eUnitAIType, pUnitArea);

		for (UnitClassTypes eUpgradeUnitClassType = FIRST_UNITCLASS; eUpgradeUnitClassType < NUM_UNITCLASS_TYPES; ++eUpgradeUnitClassType)
		{
			UnitTypes eUpgradeUnitType = (UnitTypes)(kCivilizationInfo.getCivilizationUnits(eUpgradeUnitClassType));

			if (NO_UNIT != eUpgradeUnitType)
			{
				// is it better?
				int iUpgradeValue = AI_unitValue(eUpgradeUnitType, eUnitAIType, pUnitArea);
				if (iUpgradeValue > iUnitValue)
				{
					// is this a valid upgrade?
					if (pLoopUnit->upgradeAvailable(eUnitType, eUpgradeUnitClassType))
					{
						// can we actually make this upgrade?
						bool bCanUpgrade = false;
						CvCity* pCapitalCity = getPrimaryCity();
						if (pCapitalCity != NULL && pCapitalCity->canTrain(eUpgradeUnitType))
						{
							bCanUpgrade = true;
						}
						else
						{
							CvCity* pCloseCity = GC.getMap().findCity(pLoopUnit->getX_INLINE(), pLoopUnit->getY_INLINE(), getID(), NO_TEAM, true, (pLoopUnit->getDomainType() == DOMAIN_SEA));
							if (pCloseCity != NULL && pCloseCity->canTrain(eUpgradeUnitType))
							{
								bCanUpgrade = true;
							}
						}

						if (bCanUpgrade)
						{
							iUnitGold += pLoopUnit->upgradePrice(eUpgradeUnitType);
							iUnitUpgradePossibilities++;
						}
					}
				}
			}
		}

		// if we found any, find average and add to total
		if (iUnitUpgradePossibilities > 0)
		{
			iUnitGold /= iUnitUpgradePossibilities;

			// add to cache
			aiUnitUpgradePrice[eUnitType] = iUnitGold;

			// add to sum
			iTotalGold += iUnitGold;
		}
		else
		{
			// add to cache, dont upgrade to this type
			aiUnitUpgradePrice[eUnitType] = -1;
		}
	}

	m_iUpgradeUnitsCacheTurn = GC.getGameINLINE().getGameTurn();
	m_iUpgradeUnitsCachedExpThreshold = iExpThreshold;
	m_iUpgradeUnitsCachedGold = iTotalGold;

	return iTotalGold;
}

int CvPlayerAI::AI_goldTradeValuePercent()
{
	return 100;
}

int CvPlayerAI::AI_playerCloseness(PlayerTypes eIndex, int iMaxDistance)
{
	PROFILE_FUNC();
	CvCity* pLoopCity;
	int iLoop;
	int iValue;

	FAssert(GET_PLAYER(eIndex).isAlive());
	FAssert(eIndex != getID());

	iValue = 0;
	// WTP: sum up how close our own cities are to eIndex (the loop used to run over eIndex's cities, which
	// measured how close eIndex's cities are to eIndex itself)
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		iValue += pLoopCity->AI_playerCloseness(eIndex, iMaxDistance);
	}

	return iValue;
}

int CvPlayerAI::AI_targetValidity(PlayerTypes ePlayer)
{
	FAssert(ePlayer != NO_PLAYER);

	CvPlayerAI& kPlayer = GET_PLAYER(ePlayer);

	if (!kPlayer.isAlive())
	{
		return 0;
	}
	if (kPlayer.getTeam() == getTeam())
	{
		return 0;
	}

	int iAggressionRange = 10;
	if (isNative())
	{
		iAggressionRange = 5;
	}

	int iValidTargetCount = 0;
	int iTotalValue = 0;

	if (!kPlayer.isAlive())
	{
		return 0;
	}

	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = kPlayer.firstCity(&iLoop); pLoopCity != NULL; pLoopCity = kPlayer.nextCity(&iLoop))
	{
		bool bLandTarget = (pLoopCity->area()->getCitiesPerPlayer(getID()) > 0);

		if (bLandTarget || !isNative())
		{
			int iDistance = AI_cityDistance(pLoopCity->plot());
			int iAdjustedRange = iAggressionRange;
			if (!bLandTarget)
			{
				iAdjustedRange *= 2;
			}

			if (iDistance <= iAdjustedRange)
			{
				iTotalValue += 100 - (100 * (iDistance - 1)) / iAdjustedRange;
				iValidTargetCount++;
			}
		}
	}

	int iValue = (2 * iTotalValue) / (1 + kPlayer.getNumCities() + iValidTargetCount);

	// TAC - AI Assault Sea - koma13 - START
	if (iValue == 0 && !isNative())
	{
		iValue += AI_totalUnitAIs(UNITAI_ASSAULT_SEA);
	}
	// TAC - AI Assault Sea - koma13 - END

	return iValue;
}

// TAC - AI City Defense - koma13 - START
//int CvPlayerAI::AI_totalDefendersNeeded(int* piUndefendedCityCount)
int CvPlayerAI::AI_totalDefendersNeeded(int* piUndefendedCityCount, CvArea* pArea, bool bIncludeStacks)
{
	PROFILE_FUNC();
	CvCity* pLoopCity;
	int iLoop;
	int iValue;

	int iTotalNeeded = 0;
	int iUndefendedCount = 0;

	iValue = 0;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pArea == NULL || pArea == pLoopCity->area())
		{
			int iHave = pLoopCity->AI_numDefenders(true, true);
			int iNeeded = pLoopCity->AI_neededDefenders();

			if (iNeeded > 0)
			{
				if (iHave == 0)
				{
					iUndefendedCount++;
				}
				iTotalNeeded += std::max(0, iNeeded - iHave);
			}
		}
	}

	if (bIncludeStacks)
	{
		for(CvSelectionGroup* pLoopSelectionGroup = firstSelectionGroup(&iLoop); pLoopSelectionGroup; pLoopSelectionGroup = nextSelectionGroup(&iLoop))
		{
			if (pLoopSelectionGroup->getNumUnits() > 1)
			{
				if (pLoopSelectionGroup->getHeadUnitAI() == UNITAI_OFFENSIVE)
				{
					if (pArea == NULL || pArea == pLoopSelectionGroup->area())
					{
						int iStackDefenders = pLoopSelectionGroup->countNumUnitAIType(UNITAI_DEFENSIVE) + AI_unitTargetMissionAIs(pLoopSelectionGroup->getHeadUnit(), MISSIONAI_GROUP, NULL, UNITAI_DEFENSIVE);
						if (iStackDefenders < 3)
						{
							iTotalNeeded += std::max(0, 3 - iStackDefenders);
						}
					}
				}
			}
		}
	}
	// TAC - AI City Defense - koma13 - END

	if (piUndefendedCityCount != NULL)
	{
		*piUndefendedCityCount = iUndefendedCount;
	}

	return iTotalNeeded;
}


int CvPlayerAI::AI_getTotalAreaCityThreat(CvArea* pArea)
{
	PROFILE_FUNC();
	CvCity* pLoopCity;
	int iLoop;
	int iValue;

	iValue = 0;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity->getArea() == pArea->getID())
		{
			iValue += pLoopCity->AI_cityThreat();
		}
	}
	return iValue;
}

int CvPlayerAI::AI_countNumAreaHostileUnits(CvArea* pArea, bool bPlayer, bool bTeam, bool bNeutral, bool bHostile)
{
	PROFILE_FUNC();
	CvPlot* pLoopPlot;
	int iCount;
	int iI;

	iCount = 0;

	for (iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
	{
		pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
		if ((pLoopPlot->area() == pArea) && pLoopPlot->isVisible(getTeam(), false) &&
			((bPlayer && pLoopPlot->getOwnerINLINE() == getID()) || (bTeam && pLoopPlot->getTeam() == getTeam())
				|| (bNeutral && !pLoopPlot->isOwned()) || (bHostile && pLoopPlot->isOwned() && GET_TEAM(getTeam()).isAtWar(pLoopPlot->getTeam()))))
			{
			iCount += pLoopPlot->plotCount(PUF_isEnemy, getID(), false, NO_PLAYER, NO_TEAM, PUF_isVisible, getID());
		}
	}
	return iCount;
}

//this doesn't include the minimal one or two garrison units in each city.
int CvPlayerAI::AI_getTotalFloatingDefendersNeeded(CvArea* pArea)
{
	PROFILE_FUNC();
	int iDefenders;
	int iCurrentEra = getCurrentEra();
	int iAreaCities = pArea->getCitiesPerPlayer(getID());

	iCurrentEra = std::max(0, iCurrentEra - GC.getGame().getStartEra() / 2);

	iDefenders = 1 + ((iCurrentEra + ((GC.getGameINLINE().getMaxCityElimination() > 0) ? 3 : 2)) * iAreaCities);
	iDefenders /= 3;
	iDefenders += pArea->getPopulationPerPlayer(getID()) / 7;

	if (pArea->getAreaAIType(getTeam()) == AREAAI_DEFENSIVE)
	{
		iDefenders *= 2;
	}
	else if ((pArea->getAreaAIType(getTeam()) == AREAAI_OFFENSIVE) || (pArea->getAreaAIType(getTeam()) == AREAAI_MASSING))
	{
		iDefenders *= 2;
		iDefenders /= 3;
	}

	if (AI_getTotalAreaCityThreat(pArea) == 0)
	{
		iDefenders /= 2;
	}

	if (!GC.getGameINLINE().isOption(GAMEOPTION_AGGRESSIVE_AI))
	{
		iDefenders *= 2;
		iDefenders /= 3;
	}

	iDefenders *= 60;
	iDefenders /= std::max(30, (GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAITrainPercent() - 20));

	if (getPrimaryCity() != NULL)
	{
		if (getPrimaryCity()->area() != pArea)
		{
			//Defend offshore islands only lightly.
			iDefenders = std::min(iDefenders, iAreaCities * iAreaCities - 1);
		}
	}
	// Super Forts begin *AI_defense* - Build a few extra floating defenders for occupying forts
	iDefenders += iAreaCities / 2;
	// Super Forts end
	return iDefenders;
}

int CvPlayerAI::AI_getTotalFloatingDefenders(CvArea* pArea)
{
	PROFILE_FUNC();
	int iCount = 0;

	iCount += AI_totalAreaUnitAIs(pArea, UNITAI_DEFENSIVE);
	iCount += AI_totalAreaUnitAIs(pArea, UNITAI_OFFENSIVE);
	iCount += AI_totalAreaUnitAIs(pArea, UNITAI_COUNTER);
	return iCount / 2;
}

RouteTypes CvPlayerAI::AI_bestAdvancedStartRoute(CvPlot* pPlot, int* piYieldValue)
{
	RouteTypes eBestRoute = NO_ROUTE;
	int iBestValue = -1;
    for (int iI = 0; iI < GC.getNumRouteInfos(); iI++)
    {
        RouteTypes eRoute = (RouteTypes)iI;

		int iValue = 0;
		int iCost = getAdvancedStartRouteCost(eRoute, true, pPlot);

		if (iCost >= 0)
		{
			iValue += GC.getRouteInfo(eRoute).getValue();

			if (iValue > 0)
			{
				int iYieldValue = 0;
				if (pPlot->getImprovementType() != NO_IMPROVEMENT)
				{
					iYieldValue += ((GC.getImprovementInfo(pPlot->getImprovementType()).getRouteYieldChanges(eRoute, YIELD_FOOD)) * 100);
				}
				iValue *= 1000;
				iValue /= (1 + iCost);

				if (iValue > iBestValue)
				{
					iBestValue = iValue;
					eBestRoute = eRoute;
					if (piYieldValue != NULL)
					{
						*piYieldValue = iYieldValue;
					}
				}
			}
		}
	}
	return eBestRoute;
}

UnitTypes CvPlayerAI::AI_bestAdvancedStartUnitAI(CvPlot* pPlot, UnitAITypes eUnitAI)
{
	UnitTypes eLoopUnit;
	UnitTypes eBestUnit;
	int iValue;
	int iBestValue;
	int iJ, iK;

	FAssertMsg(eUnitAI != NO_UNITAI, "UnitAI is not assigned a valid value");

	iBestValue = 0;
	eBestUnit = NO_UNIT;

	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		eLoopUnit = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eUnitClass);

		if (eLoopUnit != NO_UNIT)
		{
			if (GC.getUnitInfo(eLoopUnit).getDefaultUnitAIType() == eUnitAI)
			{
				int iUnitCost = getAdvancedStartUnitCost(eLoopUnit, true, pPlot);
				if (iUnitCost >= 0)
				{
					iValue = AI_unitValue(eLoopUnit, eUnitAI, pPlot->area());

					if (iValue > 0)
					{
						//free promotions. slow?
						//only 1 promotion per source is counted (ie protective isn't counted twice)
						int iPromotionValue = 0;

						//special to the unit
						for (iJ = 0; iJ < GC.getNumPromotionInfos(); iJ++)
						{
							if (GC.getUnitInfo(eLoopUnit).getFreePromotions(iJ))
							{
								iPromotionValue += 15;
								break;
							}
						}

						for (iK = 0; iK < GC.getNumPromotionInfos(); iK++)
						{
							if (isFreePromotion((UnitCombatTypes)GC.getUnitInfo(eLoopUnit).getUnitCombatType(), (PromotionTypes)iK))
							{
								iPromotionValue += 15;
								break;
							}

							if (isFreePromotion(GC.getUnitInfo(eLoopUnit).getUnitClassType(), (PromotionTypes)iK))
							{
								iPromotionValue += 15;
								break;
							}
						}

						//traits
						for (iJ = 0; iJ < GC.getNumTraitInfos(); iJ++)
						{
							if (hasTrait((TraitTypes)iJ))
							{
								for (iK = 0; iK < GC.getNumPromotionInfos(); iK++)
								{
									if (GC.getTraitInfo((TraitTypes) iJ).isFreePromotion(iK))
									{
										if ((GC.getUnitInfo(eLoopUnit).getUnitCombatType() != NO_UNITCOMBAT) && GC.getTraitInfo((TraitTypes) iJ).isFreePromotionUnitCombat(GC.getUnitInfo(eLoopUnit).getUnitCombatType()))
										{
											iPromotionValue += 15;
											break;
										}
									}
								}
							}
						}

						iValue *= (iPromotionValue + 100);
						iValue /= 100;

						iValue *= (GC.getGameINLINE().getSorenRandNum(40, "AI Best Advanced Start Unit") + 100);
						iValue /= 100;

						iValue *= (getNumCities() + 2);
						iValue /= (getUnitClassCountPlusMaking(eUnitClass) + getNumCities() + 2);

						FAssert((MAX_INT / 1000) > iValue);
						iValue *= 1000;

						iValue /= 1 + iUnitCost;

						iValue = std::max(1, iValue);

						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							eBestUnit = eLoopUnit;
						}
					}
				}
			}
		}
	}

	return eBestUnit;
}

CvPlot* CvPlayerAI::AI_advancedStartFindCapitalPlot()
{
	CvPlot* pBestPlot = NULL;
	int iBestValue = -1;

	for (int iPlayer = 0; iPlayer < MAX_PLAYERS; iPlayer++)
	{
		CvPlayer& kPlayer = GET_PLAYER((PlayerTypes)iPlayer);
		if (kPlayer.isAlive())
		{
			if (kPlayer.getTeam() == getTeam())
			{
				CvPlot* pLoopPlot = kPlayer.getStartingPlot();
				if (pLoopPlot != NULL)
				{
					if (getAdvancedStartCityCost(true, pLoopPlot) > 0)
					{
					int iX = pLoopPlot->getX_INLINE();
					int iY = pLoopPlot->getY_INLINE();

						int iValue = 1000;
						if (iPlayer == getID())
						{
							iValue += 1000;
						}
						else
						{
							iValue += GC.getGame().getSorenRandNum(100, "AI Advanced Start Choose Team Start");
						}
						CvCity * pNearestCity = GC.getMap().findCity(iX, iY, NO_PLAYER, getTeam());
						if (NULL != pNearestCity)
						{
							FAssert(pNearestCity->getTeam() == getTeam());
							int iDistance = stepDistance(iX, iY, pNearestCity->getX_INLINE(), pNearestCity->getY_INLINE());
							if (iDistance < 10)
							{
								iValue /= (10 - iDistance);
							}
						}

						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							pBestPlot = pLoopPlot;
						}
					}
				}
				else
				{
					FAssertMsg(false, "StartingPlot for a live player is NULL!");
				}
			}
		}
	}

	if (pBestPlot != NULL)
	{
		return pBestPlot;
	}

	FAssertMsg(false, "AS: Failed to find a starting plot for a player");

	//Execution should almost never reach here.

	//Update found values just in case - particulary important for simultaneous turns.
	AI_updateFoundValues();

	pBestPlot = NULL;
	iBestValue = -1;

	if (NULL != getStartingPlot())
	{
		for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
		{
			CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
			if (pLoopPlot->getArea() == getStartingPlot()->getArea())
			{
				int iValue = pLoopPlot->getFoundValue(getID());
				if (iValue > 0)
				{
					if (getAdvancedStartCityCost(true, pLoopPlot) > 0)
					{
						if (iValue > iBestValue)
						{
							iBestValue = iValue;
							pBestPlot = pLoopPlot;
						}
					}
				}
			}
		}
	}

	if (pBestPlot != NULL)
	{
		return pBestPlot;
	}

	//Commence panic.
	FAssertMsg(false, "Failed to find an advanced start starting plot");
	return NULL;
}


bool CvPlayerAI::AI_advancedStartPlaceExploreUnits(bool bLand)
{
	CvPlot* pBestExplorePlot = NULL;
	int iBestExploreValue = 0;
	UnitTypes eBestUnitType = NO_UNIT;

	UnitAITypes eUnitAI = NO_UNITAI;
	if (bLand)
	{
		eUnitAI = UNITAI_SCOUT;
	}
	else if (!bLand)
	{
		return false;
	}

	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		CvPlot* pLoopPlot = pLoopCity->plot();
		CvArea* pLoopArea = bLand ? pLoopCity->area() : pLoopPlot->waterArea();

		if (pLoopArea != NULL)
			{
			int iValue = std::max(0, pLoopArea->getNumUnrevealedTiles(getTeam()) - 10) * 10;
			iValue += std::max(0, pLoopArea->getNumTiles() - 50);

				if (iValue > 0)
				{
					int iOtherPlotCount = 0;
					int iGoodyCount = 0;
					int iExplorerCount = 0;
				int iAreaId = pLoopArea->getID();

				int iRange = 4;
					for (int iX = -iRange; iX <= iRange; iX++)
					{
						for (int iY = -iRange; iY <= iRange; iY++)
						{
							CvPlot* pLoopPlot2 = plotXY(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE(), iX, iY);
						if (NULL != pLoopPlot2)
							{
								iExplorerCount += pLoopPlot2->plotCount(PUF_isUnitAIType, eUnitAI, -1, NO_PLAYER, getTeam());
							if (pLoopPlot2->getArea() == iAreaId)
							{
								if (pLoopPlot2->isGoody())
								{
									iGoodyCount++;
								}
								if (pLoopPlot2->getTeam() != getTeam())
								{
									iOtherPlotCount++;
								}
							}
						}
					}
				}

					iValue -= 300 * iExplorerCount;
					iValue += 200 * iGoodyCount;
					iValue += 10 * iOtherPlotCount;
					if (iValue > iBestExploreValue)
					{
						UnitTypes eUnit = AI_bestAdvancedStartUnitAI(pLoopPlot, eUnitAI);
						if (eUnit != NO_UNIT)
						{
							eBestUnitType = eUnit;
							iBestExploreValue = iValue;
							pBestExplorePlot = pLoopPlot;
						}
					}
				}
			}
		}

	if (pBestExplorePlot != NULL)
	{
		doAdvancedStartAction(ADVANCEDSTARTACTION_UNIT, pBestExplorePlot->coord(), eBestUnitType, true);
		return true;
	}
	return false;
}

void CvPlayerAI::AI_advancedStartRevealRadius(CvPlot* pPlot, int iRadius)
{
	for (int iRange = 1; iRange <=iRadius; iRange++)
	{
		for (int iX = -iRange; iX <= iRange; iX++)
		{
			for (int iY = -iRange; iY <= iRange; iY++)
			{
				if (plotDistance(0, 0, iX, iY) <= iRadius)
				{
					CvPlot* pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iX, iY);

					if (NULL != pLoopPlot)
					{
						if (getAdvancedStartVisibilityCost(true, pLoopPlot) > 0)
						{
							doAdvancedStartAction(ADVANCEDSTARTACTION_VISIBILITY, pLoopPlot->coord(), -1, true);
						}
					}
				}
			}
		}
	}
}

bool CvPlayerAI::AI_advancedStartPlaceCity(CvPlot* pPlot)
{
	if (isNative())
	{
		doAdvancedStartAction(ADVANCEDSTARTACTION_CITY, pPlot->coord(), -1, true);
		return true;
	}
	//If there is already a city, then improve it.
	CvCity* pCity = pPlot->getPlotCity();
	if (pCity == NULL)
	{
		doAdvancedStartAction(ADVANCEDSTARTACTION_CITY, pPlot->coord(), -1, true);

		pCity = pPlot->getPlotCity();
		if ((pCity == NULL) || (pCity->getOwnerINLINE() != getID()))
		{
			//this should never happen since the cost for a city should be 0 if
			//the city can't be placed.
			//(It can happen if another player has placed a city in the fog)
			FAssertMsg(false, "ADVANCEDSTARTACTION_CITY failed in unexpected way");
			return false;
		}
	}

	/*
	if (pCity->getCultureLevel() <= 1)
	{
		doAdvancedStartAction(ADVANCEDSTARTACTION_CULTURE, pPlot->getX(), pPlot->getY(), -1, true);
	}
	*/

	//to account for culture expansion.
	pCity->AI_updateBestBuild();

	int iPlotsImproved = 0;
	for (int iI = 0; iI < NUM_CITY_PLOTS; iI++)
	{
		if (iI != CITY_HOME_PLOT)
		{
			CvPlot* pLoopPlot = plotCity(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iI);
			if ((pLoopPlot != NULL) && (pLoopPlot->getWorkingCity() == pCity))
			{
				if (pLoopPlot->getImprovementType() != NO_IMPROVEMENT)
				{
					iPlotsImproved++;
				}
			}
		}
	}

	int iTargetPopulation = (getCurrentEra() / 2 + 3);

	while (iPlotsImproved < iTargetPopulation)
	{
		CvPlot* pBestPlot = NULL;
		ImprovementTypes eBestImprovement = NO_IMPROVEMENT;
		int iBestValue = 0;
		for (int iI = 0; iI < NUM_CITY_PLOTS; iI++)
		{
			int iValue = pCity->AI_getBestBuildValue(iI);
			if (iValue > iBestValue)
			{
				BuildTypes eBuild = pCity->AI_getBestBuild(iI);
				if (eBuild != NO_BUILD)
				{
					ImprovementTypes eImprovement = (ImprovementTypes)GC.getBuildInfo(eBuild).getImprovement();
					if (eImprovement != NO_IMPROVEMENT)
					{
						CvPlot* pLoopPlot = plotCity(pCity->getX_INLINE(), pCity->getY_INLINE(), iI);
						if ((pLoopPlot != NULL) && (pLoopPlot->getImprovementType() != eImprovement))
						{
							eBestImprovement = eImprovement;
							pBestPlot = pLoopPlot;
							iBestValue = iValue;
						}
					}
				}
			}
		}

		if (iBestValue > 0)
		{

			FAssert(pBestPlot != NULL);
			doAdvancedStartAction(ADVANCEDSTARTACTION_IMPROVEMENT, pBestPlot->coord(), eBestImprovement, true);
			iPlotsImproved++;
			if (pCity->getPopulation() < iPlotsImproved)
			{
				doAdvancedStartAction(ADVANCEDSTARTACTION_POP, pBestPlot->coord(), -1, true);
			}
		}
		else
		{
			break;
		}
	}


	while (iPlotsImproved > pCity->getPopulation())
	{
		int iPopCost = getAdvancedStartPopCost(true, pCity);
		if (iPopCost <= 0 || iPopCost > getAdvancedStartPoints())
		{
			break;
		}
		doAdvancedStartAction(ADVANCEDSTARTACTION_POP, pPlot->coord(), -1, true);
	}

	while (iTargetPopulation > pCity->getPopulation())
	{
		int iPopCost = getAdvancedStartPopCost(true, pCity);
		if (iPopCost <= 0 || iPopCost > getAdvancedStartPoints())
		{
			break;
		}
		doAdvancedStartAction(ADVANCEDSTARTACTION_POP, pPlot->coord(), -1, true);
	}

	pCity->AI_updateAssignWork();

	return true;
}




//Returns false if we have no more points.
bool CvPlayerAI::AI_advancedStartDoRoute(CvPlot* pFromPlot, CvPlot* pToPlot)
{
	FAssert(pFromPlot != NULL);
	FAssert(pToPlot != NULL);

	FAStarNode* pNode;
	gDLL->getFAStarIFace()->ForceReset(&GC.getStepFinder());
	if (gDLL->getFAStarIFace()->GeneratePath(&GC.getStepFinder(), pFromPlot->getX_INLINE(), pFromPlot->getY_INLINE(), pToPlot->getX_INLINE(), pToPlot->getY_INLINE(), false, 0, true))
	{
		pNode = gDLL->getFAStarIFace()->GetLastNode(&GC.getStepFinder());
		if (pNode != NULL)
		{
			if (pNode->m_iData1 > (1 + stepDistance(pFromPlot->getX(), pFromPlot->getY(), pToPlot->getX(), pToPlot->getY())))
			{
				//Don't build convulted paths.
				return true;
			}
		}

		while (pNode != NULL)
		{
			CvPlot* pPlot = GC.getMap().plotSoren(pNode->m_iX, pNode->m_iY);
			RouteTypes eRoute = AI_bestAdvancedStartRoute(pPlot);
			if (eRoute != NO_ROUTE)
			{
				if (getAdvancedStartRouteCost(eRoute, true, pPlot) > getAdvancedStartPoints())
				{
					return false;
				}
				doAdvancedStartAction(ADVANCEDSTARTACTION_ROUTE, pPlot->coord(), eRoute, true);
			}
			pNode = pNode->m_pParent;
		}
	}
	return true;
}
void CvPlayerAI::AI_advancedStartRouteTerritory()
{
	CvPlot* pLoopPlot;
	int iI;

	for (iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
	{
		pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
		if ((pLoopPlot != NULL) && (pLoopPlot->getOwnerINLINE() == getID()) && (pLoopPlot->getRouteType() == NO_ROUTE))
		{
			if (pLoopPlot->getImprovementType() != NO_IMPROVEMENT)
			{
				if (pLoopPlot->getRouteType() == NO_ROUTE)
				{
					int iRouteYieldValue = 0;
					RouteTypes eRoute = (AI_bestAdvancedStartRoute(pLoopPlot, &iRouteYieldValue));
					if (eRoute != NO_ROUTE && iRouteYieldValue > 0)
					{
						doAdvancedStartAction(ADVANCEDSTARTACTION_ROUTE, pLoopPlot->coord(), eRoute, true);
					}
				}
			}
		}
	}
}


void CvPlayerAI::AI_doAdvancedStart(bool bNoExit)
{
	if (NULL == getStartingPlot())
	{
		FAssert(false);
		return;
	}

	int iTargetCityCount = GC.getWorldInfo(GC.getMap().getWorldSize()).getTargetNumCities();

	iTargetCityCount = 1 + iTargetCityCount + GC.getGameINLINE().getSorenRandNum(2 * iTargetCityCount, "AI Native Civilization Size");
	iTargetCityCount /= 2;

	int iLoop;
	CvCity* pLoopCity;
	int iStartingPoints = getAdvancedStartPoints();
	int iRevealPoints;
	int iMilitaryPoints;
	int iCityPoints;

	bool bIsNative = isNative();

	if (bIsNative)
	{
		AI_createNatives();
		if (bNoExit)
		{
			return;
		}
		else
		{
			doAdvancedStartAction(ADVANCEDSTARTACTION_EXIT, Coordinates(-1, -1), -1, true);
		}
	}

	if (isNative())
	{
		iRevealPoints = (iStartingPoints * 20) / 100;
		iMilitaryPoints = (iStartingPoints * 40) / 100;
		iCityPoints = iStartingPoints - (iMilitaryPoints + iRevealPoints);
	}
	else
	{
		iRevealPoints = (iStartingPoints * 10) / 100;
		iMilitaryPoints = (iStartingPoints * (isHuman() ? 17 : 20)) / 100;
		iCityPoints = iStartingPoints - (iMilitaryPoints + iRevealPoints);
	}

	if (!bIsNative)
	{

		if (getPrimaryCity() != NULL)
		{
			AI_advancedStartPlaceCity(getPrimaryCity()->plot());
		}
		else
		{
			for (int iPass = 0; iPass < 2 && NULL == getPrimaryCity(); ++iPass)
			{
				CvPlot* pBestCapitalPlot = AI_advancedStartFindCapitalPlot();

				if (pBestCapitalPlot != NULL)
				{
					if (!AI_advancedStartPlaceCity(pBestCapitalPlot))
					{
						FAssertMsg(false, "AS AI: Unexpected failure placing capital");
					}
					break;
				}
				else
				{
					//If this point is reached, the advanced start system is broken.
					//Find a new starting plot for this player
					setStartingPlot(findStartingPlot(false), true);
					//Redo Starting visibility
					CvPlot* pStartingPlot = getStartingPlot();
					if (NULL != pStartingPlot)
					{
						for (int iPlotLoop = 0; iPlotLoop < GC.getMap().numPlotsINLINE(); ++iPlotLoop)
						{
							CvPlot* const pPlot = GC.getMap().plotByIndexINLINE(iPlotLoop);

							if (plotDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pStartingPlot->getX_INLINE(), pStartingPlot->getY_INLINE()) <= GC.getDefineINT("ADVANCED_START_SIGHT_RANGE"))
							{
								pPlot->setRevealed(getTeam(), true, false, NO_TEAM);
							}
						}
					}
				}
			}

			if (getPrimaryCity() == NULL)
			{
				if (!bNoExit)
				{
					doAdvancedStartAction(ADVANCEDSTARTACTION_EXIT, Coordinates(-1, -1), -1, true);
				}
				return;
			}
		}

		iCityPoints -= (iStartingPoints - getAdvancedStartPoints());

		int iLastPointsTotal = getAdvancedStartPoints();

		for (int iPass = 0; iPass < 6; iPass++)
		{
			for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
			{
				CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
				if (pLoopPlot->isRevealed(getTeam(), false))
				{
					if (pLoopPlot->getBonusType() != NO_BONUS)
					{
						AI_advancedStartRevealRadius(pLoopPlot, CITY_PLOTS_RADIUS);
					}
					else
					{
						for (int iJ = 0; iJ < NUM_CARDINALDIRECTION_TYPES; iJ++)
						{
							CvPlot* pLoopPlot2 = plotCardinalDirection(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE(), (CardinalDirectionTypes)iJ);
							if ((pLoopPlot2 != NULL) && (getAdvancedStartVisibilityCost(true, pLoopPlot2) > 0))
							{
								//Mildly maphackery but any smart human can see the terrain type of a tile.
								pLoopPlot2->getTerrainType();
								int iFoodYield = GC.getTerrainInfo(pLoopPlot2->getTerrainType()).getYield(YIELD_FOOD);
								if (pLoopPlot2->getFeatureType() != NO_FEATURE)
								{
									iFoodYield += GC.getFeatureInfo(pLoopPlot2->getFeatureType()).getYieldChange(YIELD_FOOD);
								}
								if (iFoodYield >= 2 || pLoopPlot2->isHills() || pLoopPlot2->isPeak() || pLoopPlot2->isRiver())
								{
									doAdvancedStartAction(ADVANCEDSTARTACTION_VISIBILITY, pLoopPlot2->coord(), -1, true);
								}
							}
						}
					}
				}
				if ((iLastPointsTotal - getAdvancedStartPoints()) > iRevealPoints)
				{
					break;
				}
			}
		}

		iLastPointsTotal = getAdvancedStartPoints();
		iCityPoints = std::min(iCityPoints, iLastPointsTotal);
		int iArea = -1; //getStartingPlot()->getArea();
		bool bDonePlacingCities = false;
		for (int iPass = 0; iPass < 100; ++iPass)
		{
			int iBestFoundValue = 0;
			CvPlot* pBestFoundPlot = NULL;
			AI_updateFoundValues(false);
			for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
			{
				CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
				if (plotDistance(getStartingPlot()->getX_INLINE(), getStartingPlot()->getY_INLINE(), pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE()) < 19)
				{
					int iFoundValue = pLoopPlot->getFoundValue(getID());
					if (isNative())
					{
						iFoundValue = iFoundValue + GC.getGameINLINE().getSorenRandNum(iFoundValue * 2, "AI place native city");
					}
					if (pLoopPlot->getFoundValue(getID()) > iBestFoundValue)
					{
						if (getAdvancedStartCityCost(true, pLoopPlot) > 0)
						{
							pBestFoundPlot = pLoopPlot;
							iBestFoundValue = pLoopPlot->getFoundValue(getID());
						}
					}
				}
			}

			if (isNative())
			{
				if (getNumCities() >= iTargetCityCount)
				{
					bDonePlacingCities = true;
				}
			}
			else if (iBestFoundValue < ((getNumCities() == 0) ? 1 : (500 + 250 * getNumCities())))
			{
				bDonePlacingCities = true;
			}

			if (pBestFoundPlot == NULL)
			{
				bDonePlacingCities = true;
			}

			if (!bDonePlacingCities)
			{
				int iCost = getAdvancedStartCityCost(true, pBestFoundPlot);
				if (iCost > getAdvancedStartPoints())
				{
					bDonePlacingCities = true;
				}// at 500pts, we have 200, we spend 100.
				else if (((iLastPointsTotal - getAdvancedStartPoints()) + iCost) > iCityPoints)
				{
					bDonePlacingCities = true;
				}
			}

			if (!bDonePlacingCities)
			{
				if (!AI_advancedStartPlaceCity(pBestFoundPlot))
				{
					FAssertMsg(false, "AS AI: Failed to place city (non-capital)");
					bDonePlacingCities = true;
				}
			}

			if (bDonePlacingCities)
			{
				break;
			}
		}


		//Land
		AI_advancedStartPlaceExploreUnits(true);
		if (getCurrentEra() > 2)
		{
			//Sea
			AI_advancedStartPlaceExploreUnits(false);
		}

		if (!isNative())
		{
			AI_advancedStartRouteTerritory();
		}

		bool bDoneBuildings = (iLastPointsTotal - getAdvancedStartPoints()) > iCityPoints;
		for (int iPass = 0; iPass < 10 && !bDoneBuildings; ++iPass)
		{
			for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
			{
				BuildingTypes eBuilding = pLoopCity->AI_bestAdvancedStartBuilding(iPass);
				if (eBuilding != NO_BUILDING)
				{
					bDoneBuildings = (iLastPointsTotal - (getAdvancedStartPoints() - getAdvancedStartBuildingCost(eBuilding, true, pLoopCity))) > iCityPoints;
					if (!bDoneBuildings)
					{
						doAdvancedStartAction(ADVANCEDSTARTACTION_BUILDING, pLoopCity->coord(), eBuilding, true);
					}
					else
					{
						//continue there might be cheaper buildings in other cities we can afford
					}
				}
			}
		}
	}

	//Units
	std::vector<UnitAITypes> aeUnitAITypes;
	if (!isNative())
	{
		aeUnitAITypes.push_back(UNITAI_COLONIST);
	}
	else
	{
		aeUnitAITypes.push_back(UNITAI_DEFENSIVE);
	}


	bool bDone = false;
	for (int iPass = 0; iPass < 6; ++iPass)
	{
		for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
		{

			{
				if (iPass > 0)
			{
					if (getAdvancedStartPopCost(true, pLoopCity) > getAdvancedStartPoints())
					{
						bDone = true;
						break;
					}
					doAdvancedStartAction(ADVANCEDSTARTACTION_POP, pLoopCity->coord(), -1, true);
				}
				CvPlot* pUnitPlot = pLoopCity->plot();
				//Token defender
				UnitTypes eBestUnit = AI_bestAdvancedStartUnitAI(pUnitPlot, aeUnitAITypes[iPass % aeUnitAITypes.size()]);
				if (eBestUnit != NO_UNIT)
				{

					if (getAdvancedStartUnitCost(eBestUnit, true, pUnitPlot) > getAdvancedStartPoints())
					{
						bDone = true;
						break;
					}
					doAdvancedStartAction(ADVANCEDSTARTACTION_UNIT, pUnitPlot->coord(), eBestUnit, true);
				}
			}
		}
	}

	if (!bNoExit)
	{
		doAdvancedStartAction(ADVANCEDSTARTACTION_EXIT, Coordinates(-1, -1), -1, true);
	}

}

int CvPlayerAI::AI_getMinFoundValue()
{
	return 600;
}

int CvPlayerAI::AI_bestAreaUnitAIValue(UnitAITypes eUnitAI, CvArea* pArea, UnitTypes* peBestUnitType)
{

	CvCity* pCity = NULL;

	if (pArea != NULL)
	{
	if (getPrimaryCity() != NULL)
	{
		if (pArea->isWater())
		{
			if (getPrimaryCity()->plot()->isAdjacentToArea(pArea))
			{
				pCity = getPrimaryCity();
			}
		}
		else
		{
			if (getPrimaryCity()->getArea() == pArea->getID())
			{
				pCity = getPrimaryCity();
			}
		}
	}

	if (NULL == pCity)
	{
		CvCity* pLoopCity;
		int iLoop;
		for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
		{
			if (pArea->isWater())
			{
				if (pLoopCity->plot()->isAdjacentToArea(pArea))
				{
					pCity = pLoopCity;
					break;
				}
			}
			else
			{
				if (pLoopCity->getArea() == pArea->getID())
				{
					pCity = pLoopCity;
					break;
				}
			}
		}
	}
	}

	return AI_bestCityUnitAIValue(eUnitAI, pCity, peBestUnitType);

}

int CvPlayerAI::AI_bestCityUnitAIValue(UnitAITypes eUnitAI, CvCity* pCity, UnitTypes* peBestUnitType)
{
	UnitTypes eLoopUnit;
	int iValue;
	int iBestValue;

	FAssertMsg(eUnitAI != NO_UNITAI, "UnitAI is not assigned a valid value");

	iBestValue = 0;

	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		eLoopUnit = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eUnitClass);

		if (eLoopUnit != NO_UNIT)
		{
			if (!isHuman() || (GC.getUnitInfo(eLoopUnit).getDefaultUnitAIType() == eUnitAI))
			{
				if (NULL == pCity ? canTrain(eLoopUnit) : pCity->canTrain(eLoopUnit))
				{
					iValue = AI_unitValue(eLoopUnit, eUnitAI, (pCity == NULL) ? NULL : pCity->area());
					if (iValue > iBestValue)
					{
						iBestValue = iValue;
						if (peBestUnitType != NULL)
						{
							*peBestUnitType = eLoopUnit;
						}
					}
				}
			}
		}
	}

	return iBestValue;
}

int CvPlayerAI::AI_calculateTotalBombard(DomainTypes eDomain)
{
	int iTotalBombard = 0;

	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		UnitTypes eLoopUnit = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eUnitClass);
		if (eLoopUnit != NO_UNIT)
		{
			if (GC.getUnitInfo(eLoopUnit).getDomainType() == eDomain)
			{
				int iBombardRate = GC.getUnitInfo(eLoopUnit).getBombardRate();

				if (iBombardRate > 0)
				{
					iTotalBombard += iBombardRate * getUnitClassCount(eUnitClass);
				}
			}
		}
	}

	return iTotalBombard;
}

int CvPlayerAI::AI_getUnitClassWeight(UnitClassTypes eUnitClass)
{
	return m_em_iUnitClassWeights.get(eUnitClass) / 100;
}

int CvPlayerAI::AI_getUnitCombatWeight(UnitCombatTypes eUnitCombat)
{
	return m_em_iUnitCombatWeights.get(eUnitCombat) / 100;
}

void CvPlayerAI::AI_doEnemyUnitData()
{
	std::vector<int> aiUnitCounts(GC.getNumUnitInfos(), 0);

	std::vector<int> aiDomainSums(NUM_DOMAIN_TYPES, 0);

	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	int iI;

	int iOldTotal = 0;
	int iNewTotal = 0;


	for (iI = 0; iI < GC.getMap().numPlotsINLINE(); iI++)
	{

		CvPlot* pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
		int iAdjacentAttackers = -1;
		if (pLoopPlot->isVisible(getTeam(), false))
		{
			pUnitNode = pLoopPlot->headUnitNode();

			while (pUnitNode != NULL)
			{
				pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

				if (pLoopUnit != NULL && pLoopUnit->canFight())
				{
					int iUnitValue = 1;
					if (atWar(getTeam(), pLoopUnit->getTeam()))
					{
						iUnitValue += 10;

						if ((pLoopPlot->getOwnerINLINE() == getID()))
						{
							iUnitValue += 15;
						}
						else if (atWar(getTeam(), pLoopPlot->getTeam()))
						{
							if (iAdjacentAttackers == -1)
							{
								iAdjacentAttackers = GET_PLAYER(pLoopPlot->getOwnerINLINE()).AI_adjacentPotentialAttackers(pLoopPlot);
							}
							if (iAdjacentAttackers > 0)
							{
								iUnitValue += 15;
							}
						}
					}
					else if (pLoopUnit->getOwnerINLINE() != getID())
					{
						iUnitValue += pLoopUnit->canAttack() ? 4 : 1;
						if (pLoopPlot->getCulture(getID()) > 0)
						{
							iUnitValue += pLoopUnit->canAttack() ? 4 : 1;
						}
					}

					if (m_em_iUnitClassWeights.get(pLoopUnit->getUnitClassType()) == 0)
					{
						iUnitValue *= 4;
					}

					iUnitValue *= pLoopUnit->baseCombatStr();
					aiUnitCounts[pLoopUnit->getUnitType()] += iUnitValue;
					aiDomainSums[pLoopUnit->getDomainType()] += iUnitValue;
					iNewTotal += iUnitValue;
				}
			}
		}
	}

	if (iNewTotal == 0)
	{
		//This should rarely happen.
		return;
	}

	//Decay
	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		m_em_iUnitClassWeights.add(eUnitClass, -100);
		m_em_iUnitClassWeights.set(eUnitClass, (m_em_iUnitClassWeights.get(eUnitClass)*3)/4);
		m_em_iUnitClassWeights.set(eUnitClass, std::max(0, m_em_iUnitClassWeights.get(eUnitClass)));
	}

	for (UnitTypes eLoopUnit = FIRST_UNIT; eLoopUnit < NUM_UNIT_TYPES; ++eLoopUnit)
	{
		if (aiUnitCounts[eLoopUnit] > 0)
		{
			aiUnitCounts[eLoopUnit] = 0;
			FAssert(aiDomainSums[GC.getUnitInfo(eLoopUnit).getDomainType()] > 0);
			m_em_iUnitClassWeights.add(GC.getUnitInfo(eLoopUnit).getUnitClassType(), (5000 * aiUnitCounts[eLoopUnit]) / std::max(1, aiDomainSums[GC.getUnitInfo(eLoopUnit).getDomainType()]));
		}
	}


	m_em_iUnitCombatWeights.reset();


	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		if (m_em_iUnitClassWeights.get(eUnitClass) > 0)
		{
			UnitTypes eUnit = (UnitTypes)GC.getUnitClassInfo(eUnitClass).getDefaultUnitIndex();
			m_em_iUnitCombatWeights.add((UnitCombatTypes)GC.getUnitInfo(eUnit).getUnitCombatType(), m_em_iUnitClassWeights.get(eUnitClass));

		}
	}

	for (iI = 0; iI < GC.getNumUnitCombatInfos(); iI++)
	{
		if (m_em_iUnitCombatWeights.get((UnitCombatTypes)iI) > 25)
		{
			m_em_iUnitCombatWeights.add((UnitCombatTypes)iI, 2500);
		}
		else if (m_em_iUnitCombatWeights.get((UnitCombatTypes)iI) > 0)
		{
			m_em_iUnitCombatWeights.add((UnitCombatTypes)iI, 1000);
		}
	}
}

int CvPlayerAI::AI_calculateUnitAIViability(UnitAITypes eUnitAI, DomainTypes eDomain)
{
	int iBestUnitAIStrength = 0;
	int iBestOtherStrength = 0;

	for (int iI = 0; iI < GC.getNumUnitClassInfos(); iI++)
	{
		UnitTypes eLoopUnit = (UnitTypes)GC.getUnitClassInfo((UnitClassTypes)iI).getDefaultUnitIndex();
		CvUnitInfo& kUnitInfo = GC.getUnitInfo((UnitTypes)iI);
		if (kUnitInfo.getDomainType() == eDomain)
		{
			if (m_em_iUnitClassWeights.get((UnitClassTypes)iI) > 0)
			{
				if (kUnitInfo.getUnitAIType(eUnitAI))
				{
					iBestUnitAIStrength = std::max(iBestUnitAIStrength, kUnitInfo.getCombat());
				}

				iBestOtherStrength = std::max(iBestOtherStrength, kUnitInfo.getCombat());
			}
		}
	}

	return (100 * iBestUnitAIStrength) / std::max(1, iBestOtherStrength);
}

int CvPlayerAI::AI_getAttitudeWeight(PlayerTypes ePlayer)
{
	int iAttitudeWeight = 0;
	switch (AI_getAttitude(ePlayer))
	{
	case ATTITUDE_FURIOUS:
		iAttitudeWeight = -100;
		break;
	case ATTITUDE_ANNOYED:
		iAttitudeWeight = -50;
		break;
	case ATTITUDE_CAUTIOUS:
		iAttitudeWeight = 0;
		break;
	case ATTITUDE_PLEASED:
		iAttitudeWeight = 50;
		break;
	case ATTITUDE_FRIENDLY:
		iAttitudeWeight = 100;
		break;
	}

	return iAttitudeWeight;
}

//vetiarvind, super forts - begin (mark as const)
int CvPlayerAI::AI_getPlotCanalValue(CvPlot* pPlot) const
{
	PROFILE_FUNC();

	FAssert(pPlot != NULL);

	// Super Forts begin *canal*

	if(pPlot == NULL) return 0;//R&R mod, vetiarvind, super forts merge, add null check just in case

	int iCanalValue = pPlot->getCanalValue();

	if (iCanalValue > 0)
	{

		if (pPlot->isOwned())
		{
			if (pPlot->getTeam() != getTeam())
			{
				return 0;
			}
			if (pPlot->isCityRadius())
			{
				CvCity* pWorkingCity = pPlot->getWorkingCity();
				if (pWorkingCity != NULL)
				{
					if (pWorkingCity->AI_getBestBuild(pWorkingCity->getCityPlotIndex(pPlot)) != NO_BUILD)
					{
						return 0;
					}
					if (pPlot->getImprovementType() != NO_IMPROVEMENT)
					{
						CvImprovementInfo &kImprovementInfo = GC.getImprovementInfo(pPlot->getImprovementType());
						// WTP, ray, Canal - START
						// also needed to put new Canal Improvement in here
						if (!kImprovementInfo.isActsAsCity() && !kImprovementInfo.isCanal())
						{
							return 0;
						}
						// Decrease value when within radius of a city
						iCanalValue -= 5;
					}
				}
			}
		}

		for (int iI = 0; iI < NUM_DIRECTION_TYPES; iI++)
		{
			CvPlot* pLoopPlot = plotDirection(pPlot->getX_INLINE(), pPlot->getY_INLINE(), (DirectionTypes)iI);
			if (pLoopPlot != NULL)
			{
				/* original
				if (pLoopPlot->isCity(true))
				{
					return 0;
				}*/
				if (pLoopPlot->isCity(true) && (pLoopPlot->getCanalValue() > 0))
				{
					// Decrease value when adjacent to a city or fort with a canal value
					iCanalValue -= 10;
				}
			}
		}

		//super forts - begin
		iCanalValue *= 10;

		int iDefenseModifier = pPlot->defenseModifier(getTeam(), false);
		iCanalValue += iDefenseModifier; // Favor plots with higher defense
		//super forts - end
	}

	return std::max(0,iCanalValue);
	/* original
	CvArea* pSecondWaterArea = pPlot->secondWaterArea();
	if (pSecondWaterArea == NULL)
	{
		return 0;
	}
	return 10 * std::min(0, pSecondWaterArea->getNumTiles() - 2); //vetiarvind - i think this should be max though
	*/


}
// super forts end

// Super Forts begin *choke*
int CvPlayerAI::AI_getPlotChokeValue(CvPlot* pPlot) const
{
	PROFILE_FUNC();

	FAssert(pPlot != NULL);

	if(pPlot == NULL) return 0;//R&R mod, vetiarvind, super forts merge, add null check just in case

	int iChokeValue = pPlot->getChokeValue();

	if (iChokeValue > 0)
	{
		if (pPlot->isOwned())
		{
			if (pPlot->getTeam() != getTeam())
			{
				return 0;
			}
			if (pPlot->isCityRadius())
			{
				CvCity* pWorkingCity = pPlot->getWorkingCity();
				if (pWorkingCity != NULL)
				{
					// Left in this part from the original code. Might be needed to avoid workers from getting stuck in a loop?
					if (pWorkingCity->AI_getBestBuild(pWorkingCity->getCityPlotIndex(pPlot)) != NO_BUILD)
					{
						return 0;
					}
					if (pPlot->getImprovementType() != NO_IMPROVEMENT)
					{
						CvImprovementInfo &kImprovementInfo = GC.getImprovementInfo(pPlot->getImprovementType());
						if (!kImprovementInfo.isActsAsCity())
						{
							return 0;
						}
					}
					// Decrease value when within radius of a city
					iChokeValue -= 5;
				}
			}
		}

		for (int iI = 0; iI < NUM_DIRECTION_TYPES; iI++)
		{
			CvPlot* pLoopPlot = plotDirection(pPlot->getX_INLINE(), pPlot->getY_INLINE(), (DirectionTypes)iI);
			if (pLoopPlot != NULL)
			{
				if (pLoopPlot->isCity(true) && (pLoopPlot->getChokeValue() > 0))
				{
					// Decrease value when adjacent to a city or fort with a choke value
					iChokeValue -= 10;
				}
			}
		}

		iChokeValue *= 10;
		// Favor plots with higher defense
		int iDefenseModifier = pPlot->defenseModifier(getTeam(), false);
		iChokeValue += iDefenseModifier;
	}

	return std::max(0,iChokeValue);
}
// Super Forts end

void CvPlayerAI::AI_diplomaticHissyFit(PlayerTypes ePlayer, int iAttitudeChange)
{
	if (ePlayer == NO_PLAYER)
	{
		FAssert(false);
		return;
	}
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	if (iAttitudeChange >= 0)
	{
		return;
	}

	if (atWar(getTeam(), kPlayer.getTeam()))
	{
		return;
	}

	if (!kPlayer.isHuman())
	{
		return;
	}

	if (GET_TEAM(getTeam()).AI_performNoWarRolls(kPlayer.getTeam()))
	{
		return;
	}

	if (!GET_TEAM(getTeam()).AI_mayDeclareWarAsNative(kPlayer.getTeam()))
	{
		return;
	}

	//Out of 1000, so 100 is a 10% chance.
	//Note this could be modified by all sorts of things. Difficulty level might be a good one.
	int iProbability = -iAttitudeChange * 100;

	iProbability /= (GET_TEAM(getTeam()).getNumMembers() * GET_TEAM(kPlayer.getTeam()).getNumMembers());

	if (iProbability < GC.getGameINLINE().getSorenRandNum(1000, "AI Diplomatic Hissy Fit"))
	{
		return;
	}

	GET_TEAM(getTeam()).declareWar(kPlayer.getTeam(), true, WARPLAN_EXTORTION);

}

UnitTypes CvPlayerAI::AI_nextBuyUnit(UnitAITypes* peUnitAI, int* piValue)
{
	if (peUnitAI != NULL)
	{
		*peUnitAI = m_eNextBuyUnitAI;
	}
	if (piValue != NULL)
	{
		*piValue = m_iNextBuyUnitValue;
	}
	return m_eNextBuyUnit;
}

UnitTypes CvPlayerAI::AI_nextBuyProfessionUnit(ProfessionTypes* peProfession, UnitAITypes* peUnitAI, int* piValue)
{
	if (peProfession != NULL)
	{
		*peProfession = m_eNextBuyProfession;
	}
	if (peUnitAI != NULL)
	{
		*peUnitAI = m_eNextBuyProfessionAI;
	}
	if (peUnitAI != NULL)
	{
		*piValue = m_iNextBuyProfessionValue;
	}
	return m_eNextBuyProfessionUnit;
}

namespace
{
	const UnitAITypes validUnitAITypes[] = {UNITAI_DEFENSIVE , UNITAI_OFFENSIVE , UNITAI_COUNTER, UNITAI_TRANSPORT_SEA, UNITAI_COMBAT_SEA, UNITAI_PIRATE_SEA};
}

// TAC - AI purchases military units - koma13 - START
//void CvPlayerAI::AI_updateNextBuyUnit()
void CvPlayerAI::AI_updateNextBuyUnit(bool bPriceLimit)
// TAC - AI purchases military units - koma13 - END
{
	PROFILE_FUNC();
	int iBestValue = 0;
	UnitTypes eBestUnit = NO_UNIT;
	UnitAITypes eBestUnitAI = NO_UNITAI;

	// WTP: in peace only buy defenders that are missing, keeping the same gold reserve as civilian purchases;
	// while more colonies are wanted only for undefended colonies (the gold is saved for settlers);
	// warships only from 3 colonies and turn 45 on
	const bool bWar = GET_TEAM(getTeam()).getAnyWarPlanCount() > 0;
	int iUndefended = 0;
	const int iDefendersMissing = AI_totalDefendersNeeded(&iUndefended);
	const bool bExpanding = AI_desiredCityCount() > getNumCities() + AI_totalUnitAIs(UNITAI_SETTLER);
	const bool bDefendersMissing = bWar || (bExpanding ? iUndefended > 0 : iDefendersMissing > 0);
	static const UnitClassTypes eTownGuardClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_VETERAN_TOWN_GUARD");
	static const UnitClassTypes eLightArtilleryClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_LIGHT_ARTILLERY");
	const bool bWarshipsAllowed = getNumCities() >= 3 && GC.getGameINLINE().getGameTurn() >= 45;

	const int len = sizeof(validUnitAITypes) / sizeof(validUnitAITypes[0]);
	for (int iUnitAIidx = 0; iUnitAIidx < len; ++iUnitAIidx)
	{
		const UnitAITypes eLoopUnitAI = validUnitAITypes[iUnitAIidx];

		if ((eLoopUnitAI == UNITAI_COMBAT_SEA || eLoopUnitAI == UNITAI_PIRATE_SEA) && !bWarshipsAllowed)
			continue;
		if (!bWar && (eLoopUnitAI == UNITAI_OFFENSIVE || eLoopUnitAI == UNITAI_COUNTER))
			continue;
		// WTP: in war the land units are bought as an even mix (AI_doWarMilitaryPurchase)
		if (bWar && (eLoopUnitAI == UNITAI_DEFENSIVE || eLoopUnitAI == UNITAI_OFFENSIVE || eLoopUnitAI == UNITAI_COUNTER))
			continue;
		if (eLoopUnitAI == UNITAI_DEFENSIVE && !bDefendersMissing)
			continue;

		int iMultiplier = AI_unitAIValueMultipler(eLoopUnitAI);
		if (iMultiplier <= 0)
			continue;

		// TAC - AI purchases military units - koma13 - END
		// TAC - AI Assault Sea - koma13 - START
		if (eLoopUnitAI == UNITAI_ASSAULT_SEA || eLoopUnitAI == UNITAI_ESCORT_SEA)
		{
			if (!AI_prepareAssaultSea())
				continue;
		}
		// TAC - AI Assault Sea - koma13 - END

		for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
		{
			UnitTypes eLoopUnit = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eUnitClass);
			if (eLoopUnit != NO_UNIT)
			{
				// Only consider units that default to this unit ai
				const CvUnitInfo& kUnitInfo = GC.getUnitInfo(eLoopUnit);
				if (kUnitInfo.getDefaultUnitAIType() != eLoopUnitAI)
					continue;
				// WTP: in peace colonial militia (bought separately, see AI_doMilitiaPurchase) instead of town guards and light artillery
				if (!bWar && (eUnitClass == eTownGuardClass || eUnitClass == eLightArtilleryClass))
					continue;

				//if (kUnitInfo.getDefaultProfession() == NO_PROFESSION || kUnitInfo.getDefaultUnitAIType() == UNITAI_DEFENSIVE || kUnitInfo.getDefaultUnitAIType() == UNITAI_COUNTER)
				{
					const int iPrice = getEuropeUnitBuyPrice(eLoopUnit);
					// TAC - AI purchases military units - koma13 - START
					//if (iPrice > 0)// && !kUnitInfo.getUnitAIType(eLoopUnitAI))
					int iInitialPrice = getEuropeUnitBuyPrice(eLoopUnit, false);
					//int iMaxPrice = (iInitialPrice * GC.getDefineINT("AI_EUROPE_PRICE_LIMIT_PERCENT")) / 100;
					//if ((iPrice > 0) && ((iPrice < iMaxPrice) || !bPriceLimit)) // && (iPrice <= getGold()))
					if ((iPrice > 0)  && (iPrice <= getGold()) && (bWar || AI_canAffordCivilianPurchase(iPrice)))
					{
						if (kUnitInfo.getDefaultUnitAIType() == UNITAI_COMBAT_SEA)
						{
							//if (kUnitInfo.isHiddenNationality())
							// TOOD: This veto does not belong here!
							if (kUnitInfo.isHiddenNationality() || (kUnitInfo.getDefaultUnitAIType() == UNITAI_TRANSPORT_SEA))
							{
								iMultiplier = 0;
							}
						}

						const int iGoldValue = AI_unitGoldValue(eLoopUnit, eLoopUnitAI, NULL);
						int iValue = (100*(iMultiplier * iGoldValue)) / iPrice;
						if (getEuropeUnitBuyPrice(eLoopUnit) > getGold())
						{
							if (iValue < 100)
							{
								iValue = 0;
							}
						}

						if (iValue > iBestValue && !kUnitInfo.getNotUnitAIType(eLoopUnitAI))
						{
							iBestValue = iValue;
							eBestUnit = eLoopUnit;
							eBestUnitAI = eLoopUnitAI;
						}
						// TAC - AI purchases military units - koma13 - END
					}
				}
			}
		}
	}

#if 0
	// TAC - AI Military Buildup - koma13 - START
	if (bPriceLimit && (eBestUnit != NO_UNIT))
	{
		if (getEuropeUnitBuyPrice(eBestUnit) > getGold())
		{
			if (!AI_isStrategy(STRATEGY_MILITARY_BUILDUP))
			{
				AI_setStrategy(STRATEGY_MILITARY_BUILDUP);
			}
		}
	}
	// TAC - AI Military Buildup - koma13 - END
#endif

	m_eNextBuyUnit = eBestUnit;
	m_eNextBuyUnitAI = eBestUnitAI;
	m_iNextBuyUnitValue = iBestValue;
	if (gPlayerLogLevel >= 1 && eBestUnit != NO_UNIT) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) next military buy %S unitAI %d value %d price %d gold %d",
		getID(), getCivilizationDescription(0), GC.getUnitInfo(eBestUnit).getDescription(), (int)eBestUnitAI, iBestValue, getEuropeUnitBuyPrice(eBestUnit), getGold());
}

/*
int CvPlayerAI::AI_highestNextBuyValue()
{
	return std::max(m_iNextBuyUnitValue, m_iNextBuyProfessionValue);
}
*/

void CvPlayerAI::AI_updateNextBuyProfession()
{
	PROFILE_FUNC();
	int iBestValue = 0;
	UnitTypes eBestProfessionUnit = NO_UNIT;
	ProfessionTypes eBestProfession = NO_PROFESSION;
	UnitAITypes eBestUnitAI = NO_UNITAI;

	// TAC - AI Economy - koma13 - START
	if (!AI_isStrategy(STRATEGY_REVOLUTION_PREPARING))
	// TAC - AI Economy - koma13 - END
	{
		ProfessionTypes eDefaultProfession = GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession();
		int iColMultiplier = AI_unitAIValueMultipler(UNITAI_COLONIST);
		//Professions which work in cities.
		for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
		{
			UnitTypes eLoopUnit = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eUnitClass);
			if (eLoopUnit != NO_UNIT)
			{
				CvUnitInfo& kUnitInfo = GC.getUnitInfo(eLoopUnit);
				int iPrice = getEuropeUnitBuyPrice(eLoopUnit);
				if (iPrice > 0)
				{
					if (kUnitInfo.getDefaultProfession() != NO_PROFESSION)
					{
						int iValue = 0;
						UnitAITypes eUnitAI = NO_UNITAI;
						//if (kUnitInfo.getDefaultProfession() == eDefaultProfession)
						{
							ProfessionTypes eProfession = AI_idealProfessionForUnit(eLoopUnit);
							if (eProfession != NO_PROFESSION)
							{
								// TAC - AI Economy - koma13 - START
								// R&R, ray , MYCP partially based on code of Aymerick - START
								YieldTypes eYieldConsumed = (YieldTypes)GC.getProfessionInfo(eProfession).getYieldsConsumed(0);
								YieldTypes eYieldProduced = (YieldTypes)GC.getProfessionInfo(eProfession).getYieldsProduced(0);
								// R&R, ray , MYCP partially based on code of Aymerick - END
								if ((eYieldConsumed != NO_YIELD) && (eYieldProduced != NO_YIELD) && GC.getYieldInfo(eYieldProduced).isCargo())
								// TAC - AI Economy - koma13 - END
								{
									int iValue = 50 + 3 * AI_professionUpgradeValue(eProfession, eLoopUnit);
									iValue *= iColMultiplier;
									iValue /= 100;
									int iExisting = getUnitClassCountPlusMaking(eUnitClass);
									if (iExisting < 3)
									{
										iValue *= 100 + (5 + getTotalPopulation()) * kUnitInfo.getYieldModifier(YIELD_LUMBER) / (5 * (1 + iExisting));
										iValue /= 100;
										iValue *= 100 + (5 + getTotalPopulation()) * (44 * kUnitInfo.getYieldChange(YIELD_FOOD) + 34 * kUnitInfo.getBonusYieldChange(YIELD_FOOD)) / (5 * (1 + iExisting)) ;
										iValue /= 100;
										if (AI_isStrategy(STRATEGY_FAST_BELLS))
										{
											iValue *= 100 + kUnitInfo.getYieldModifier(YIELD_BELLS) / (2 + iExisting);
											iValue /= 100;
										}
									}
									for (int i = 0; i < NUM_YIELD_TYPES; ++i)
									{
										YieldTypes eLoopYield = (YieldTypes)i;
										int iModifier = kUnitInfo.getYieldModifier(eLoopYield);
										if (iModifier > 0)
										{
											if (AI_highestYieldAdvantage(eLoopYield) == 100)
											{
												if (!AI_isYieldFinalProduct(eLoopYield))
												{
													iModifier /= 4;
												}
												iValue *= 100 + iModifier;
												iValue /= 100;
											}
										}
									}
									if (iExisting < 4)
									{
										iValue *= 2;
										iValue /= 2 + iExisting;
										if (iValue > iBestValue)
										{
											iBestValue = iValue;
											eBestProfession = eDefaultProfession;
											eBestProfessionUnit = eLoopUnit;
											eBestUnitAI = UNITAI_COLONIST;
										}
									}
								}
							}
						}
					}
				}
			}
		}
		// TAC - AI Economy - koma13 - START
		/*
		for (int iUnitAI = 0; iUnitAI < NUM_UNITAI_TYPES; ++iUnitAI)
		{
			UnitAITypes eLoopUnitAI = (UnitAITypes) iUnitAI;
			if (eLoopUnitAI != UNITAI_COLONIST)
			{
				int iMultiplier = AI_unitAIValueMultipler(eLoopUnitAI);
				if (iMultiplier > 0)
				{
					ProfessionTypes eProfession = AI_idealProfessionForUnitAIType(eLoopUnitAI);
					if (eProfession != NO_PROFESSION)
					{
						UnitTypes eBestUnit = NO_UNIT;
						for (int iI = 0; iI < GC.getNumUnitClassInfos(); iI++)
						{
							UnitTypes eLoopUnit = ((UnitTypes)(GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(iI)));
							if (eLoopUnit != NO_UNIT)
							{
								CvUnitInfo& kUnitInfo = GC.getUnitInfo(eLoopUnit);
								int iPrice = getEuropeUnitBuyPrice(eLoopUnit);
								if (iPrice > 0)
								{
									int iValue = AI_professionSuitability(eLoopUnit, eProfession);
									if (iValue >= 100)
									{
										iValue *= 2 * iMultiplier;
										iValue /= 3 * 100;
										if (iValue > iBestValue)
										{
											iBestValue = iValue;
											eBestProfession = eProfession;
											eBestProfessionUnit = eLoopUnit;
											eBestUnitAI = eLoopUnitAI;
										}
									}
								}
							}
						}
						if (eBestUnit == NO_UNIT)
						{
							//Special Case.
							int iValue = iMultiplier;
							if (iValue > iBestValue)
							{
								iBestValue = iValue;
								eBestProfession = eProfession;
								eBestProfessionUnit = NO_UNIT;
								eBestUnitAI = eLoopUnitAI;
							}
						}
					}
				}
			}
		}
		*/
		// TAC - AI Economy - koma13 - END
	}

	m_eNextBuyProfession = eBestProfession;
	m_eNextBuyProfessionUnit = eBestProfessionUnit;
	m_eNextBuyProfessionAI = eBestUnitAI;
	m_iNextBuyProfessionValue = iBestValue;
}

void CvPlayerAI::AI_invalidateCloseBordersAttitudeCache()
{
	m_em_iCloseBordersAttitudeCache.setAll(MAX_INT);
	m_em_iStolenPlotsAttitudeCache.setAll(MAX_INT);
}


EmotionTypes CvPlayerAI::AI_strongestEmotion()
{
	int iBestValue = 0;
	EmotionTypes eBestEmotion = NO_EMOTION;

	for (int i = 0; i < NUM_EMOTION_TYPES; ++i)
	{
		if (m_em_iEmotions.get((EmotionTypes)i) > iBestValue)
		{
			iBestValue = m_em_iEmotions.get((EmotionTypes)i);
			eBestEmotion = (EmotionTypes)i;
		}
	}

	return eBestEmotion;
}

int CvPlayerAI::AI_emotionWeight(EmotionTypes eEmotion)
{
	EmotionTypes eBestEmotion = AI_strongestEmotion();
	if (eBestEmotion == NO_EMOTION)
	{
		return 0;
	}
	return (100 * m_em_iEmotions.get(eEmotion)) / (m_em_iEmotions.get(eBestEmotion));
}

int CvPlayerAI::AI_getEmotion(EmotionTypes eEmotion)
{
	FAssert(eEmotion > NO_EMOTION);
	FAssert(eEmotion < NUM_EMOTION_TYPES);
	return m_em_iEmotions.get(eEmotion);
}

void CvPlayerAI::AI_setEmotion(EmotionTypes eEmotion, int iNewValue)
{
	FAssert(eEmotion > NO_EMOTION);
	FAssert(eEmotion < NUM_EMOTION_TYPES);
	m_em_iEmotions.set(eEmotion, iNewValue);
}

void CvPlayerAI::AI_changeEmotion(EmotionTypes eEmotion, int iChange)
{
	FAssert(eEmotion > NO_EMOTION);
	FAssert(eEmotion < NUM_EMOTION_TYPES);
	m_em_iEmotions.add(eEmotion, iChange);
}

bool CvPlayerAI::AI_isAnyStrategy() const
{
	for (int i = 0; i < NUM_STRATEGY_TYPES; ++i)
	{
		if (AI_isStrategy((StrategyTypes)i))
		{
			return true;
		}
	}

	return false;
}

bool CvPlayerAI::AI_isStrategy(StrategyTypes eStrategy) const
{
	FAssert(eStrategy > NO_STRATEGY);
	FAssert(eStrategy < NUM_STRATEGY_TYPES);
	return (m_em_iStrategyStartedTurn.get(eStrategy) != -1);
}

int CvPlayerAI::AI_getStrategyDuration(StrategyTypes eStrategy) const
{
	FAssert(eStrategy > NO_STRATEGY);
	FAssert(eStrategy < NUM_STRATEGY_TYPES);
	if (!AI_isStrategy(eStrategy))
	{
		return -1;
	}

	return (GC.getGameINLINE().getGameTurn() - m_em_iStrategyStartedTurn.get(eStrategy));
}

int CvPlayerAI::AI_getStrategyData(StrategyTypes eStrategy)
{
	FAssert(eStrategy > NO_STRATEGY);
	FAssert(eStrategy < NUM_STRATEGY_TYPES);
	return m_em_iStrategyData.get(eStrategy);
}

void CvPlayerAI::AI_setStrategy(StrategyTypes eStrategy, int iData)
{
	FAssert(eStrategy > NO_STRATEGY);
	FAssert(eStrategy < NUM_STRATEGY_TYPES);
	m_em_iStrategyStartedTurn.set(eStrategy, GC.getGameINLINE().getGameTurn());
	m_em_iStrategyData.set(eStrategy, iData);
}

void CvPlayerAI::AI_clearStrategy(StrategyTypes eStrategy)
{
	FAssert(eStrategy > NO_STRATEGY);
	FAssert(eStrategy < NUM_STRATEGY_TYPES);
	m_em_iStrategyStartedTurn.set(eStrategy, -1);
	m_em_iStrategyData.set(eStrategy, -1);
}
// TAC - AI Military Buildup - koma13 - START
UnitAITypes CvPlayerAI::AI_bestBuildupUnitAI()
{
	UnitTypes eBuyUnit;
	UnitAITypes eBuyUnitAI;
	int iBuyUnitValue;

	eBuyUnit = AI_nextBuyUnit(&eBuyUnitAI, &iBuyUnitValue);

	if (eBuyUnit != NO_UNIT)
	{
		return eBuyUnitAI;
	}

	return NO_UNITAI;
}
// TAC - AI Military Buildup - koma13 - END

// TAC - AI Improved Naval AI - koma13 - START
bool CvPlayerAI::AI_isPathDanger(const CvSelectionGroup* pGroup, const CvPlot* pFromPlot, const CvPlot* pToPlot, int iRange) const
{
	CvPlot* pPlot = pGroup->plot();

	int iMovesLeft;
	int iStart;

	if (iRange == -1)
	{
		iRange = GC.getAI_TRANSPORT_DANGER_RANGE();
	}

	iMovesLeft = (pGroup->getHeadUnit()->movesLeft() / GLOBAL_DEFINE_MOVE_DENOMINATOR);
	iStart = (pPlot == pToPlot) ? iMovesLeft : 0;
	//iPathLength = (pPlot == pFromPlot) ? pGroup->getPathLength() - iMovesLeft : pGroup->getPathLength();
	// fix: avoids NULL crash when iMovesLeft is negative - Nightinggale
	// the issue is that iPathLength could become longer than getPathLength() resulting in NULL plots being used without checking for NULL
	int iPathLength = pGroup->getPathLength();
	if (pPlot == pFromPlot && iMovesLeft > 0)
	{
		iPathLength -= iMovesLeft;
	}
	// NULL crash end

	// R&R, ray, commented out unnecessary Asserts, probably used for testing feature
	// FAssert(iPathLength > 0);
	// FAssert(iStart < iPathLength);

	for (int i = iStart; i < iPathLength; ++i)
	{
		CvPlot* pLoopPlot = pGroup->getPathPlotByIndex(i);

		if (AI_getWaterDanger(pLoopPlot, iRange, true, true, true) > 0)
		{
			return true;
		}
	}

	return false;
}
// TAC - AI Improved Naval AI - koma13 - END

int CvPlayerAI::AI_cityDistance(CvPlot* pPlot)
{
	FAssert(pPlot != NULL);

	if (m_iDistanceMapDistance == -1)
	{
		AI_getDistanceMap();
	}

	return m_distanceMap[GC.getMap().plotNumINLINE(pPlot->getX_INLINE(), pPlot->getY_INLINE())];
}

//There's no need to save this (it is very fast to generate anyway)
std::vector<short>* CvPlayerAI::AI_getDistanceMap()
{
	if (m_iDistanceMapDistance != -1)
	{
		return &m_distanceMap;
	}

	int iMaxRange = MAX_SHORT;
	CvMap& kMap = GC.getMap();

	std::deque<int>plotQueue;
	m_distanceMap.resize(kMap.numPlotsINLINE());

	for (int i = 0; i < kMap.numPlotsINLINE(); ++i)
	{
		CvPlot* pLoopPlot = kMap.plotByIndexINLINE(i);

		if (pLoopPlot->isCity() && (pLoopPlot->getOwnerINLINE() == getID()))
		{
			plotQueue.push_back(i);
			m_distanceMap[i] = 0;
		}
		else
		{
			m_distanceMap[i] = iMaxRange;
		}
	}

	int iVisits = 0;
	while (!plotQueue.empty())
	{
		iVisits++;
		int iPlot = plotQueue.front();
		CvPlot* pPlot = kMap.plotByIndexINLINE(iPlot);
		plotQueue.pop_front();

		int iDistance = m_distanceMap[iPlot];
		iDistance += 1;

		if (iDistance < iMaxRange)
		{
			for (int iDirection = 0; iDirection < NUM_DIRECTION_TYPES; iDirection++)
			{
				CvPlot* pDirectionPlot = plotDirection(pPlot->getX_INLINE(), pPlot->getY_INLINE(), (DirectionTypes)iDirection);
				if (pDirectionPlot != NULL)
				{
					if ((pDirectionPlot->isWater() && pPlot->isWater())
						|| (!pDirectionPlot->isWater() && !pPlot->isWater())
							|| (pDirectionPlot->isWater() && (pPlot->isCity() && (pPlot->getOwnerINLINE() == getID()))))
					{

						int iPlotNum = kMap.plotNumINLINE(pDirectionPlot->getX_INLINE(), pDirectionPlot->getY_INLINE());
						if (iDistance < m_distanceMap[iPlotNum])
						{
							m_distanceMap[iPlotNum] = iDistance;
							plotQueue.push_back(iPlotNum);
						}
					}
				}
			}
		}
	}
	m_iDistanceMapDistance = iMaxRange;
	return &m_distanceMap;
}

void CvPlayerAI::AI_invalidateDistanceMap()
{
	m_iDistanceMapDistance = -1;
}

void CvPlayerAI::AI_updateBestYieldPlots()
{
	EnumMap<YieldTypes, int> m_em_iBestWorkedYield;
	EnumMap<YieldTypes, int> m_em_iBestUnworkedYield;

	m_em_iBestWorkedYieldPlots.reset();
	m_em_iBestUnworkedYieldPlots.reset();

	CvMap& kMap = GC.getMap();
	for (int i = 0; i < kMap.numPlotsINLINE(); ++i)
	{
		const CvPlot* const pLoopPlot = kMap.plotByIndexINLINE(i);

		if (pLoopPlot->isCityRadius() && (pLoopPlot->getOwnerINLINE() == getID()))
		{
			for (int iYield = 0; iYield < NUM_YIELD_TYPES; iYield++)
			{
				int iPlotYield = std::max(pLoopPlot->calculateNatureYield((YieldTypes)iYield, getTeam(), false), pLoopPlot->calculateNatureYield((YieldTypes)iYield, getTeam(), true));
				if (iPlotYield > 0)
				{
					if (pLoopPlot->isBeingWorked() && pLoopPlot->getYield((YieldTypes)iYield) > 0)
					{
						if (iPlotYield > m_em_iBestWorkedYield.get((YieldTypes)iYield))
						{
							m_em_iBestWorkedYield.set((YieldTypes)iYield, iPlotYield);
							m_em_iBestWorkedYieldPlots.set((YieldTypes)iYield, i);
						}
					}
					else
					{
						if (iPlotYield > m_em_iBestUnworkedYield.get((YieldTypes)iYield))
						{
							m_em_iBestUnworkedYield.set((YieldTypes)iYield, iPlotYield);
							m_em_iBestUnworkedYieldPlots.set((YieldTypes)iYield, i);
						}
					}
				}
			}
		}
	}
}

CvPlot* CvPlayerAI::AI_getBestWorkedYieldPlot(YieldTypes eYield)
{
	FAssertMsg(eYield > NO_YIELD, "Index out of bounds");
	FAssertMsg(eYield < NUM_YIELD_TYPES, "Index out of bounds");

	//Automatically returns NULL, if -1.
	return GC.getMap().plotByIndexINLINE(m_em_iBestWorkedYieldPlots.get(eYield));
}

CvPlot* CvPlayerAI::AI_getBestUnworkedYieldPlot(YieldTypes eYield)
{
	FAssertMsg(eYield > NO_YIELD, "Index out of bounds");
	FAssertMsg(eYield < NUM_YIELD_TYPES, "Index out of bounds");

	//Automatically returns NULL, if -1.
	return GC.getMap().plotByIndexINLINE(m_em_iBestUnworkedYieldPlots.get(eYield));
}

int CvPlayerAI::AI_getBestPlotYield(YieldTypes eYield)
{
	CvPlot* pPlot = AI_getBestWorkedYieldPlot(eYield);
	if (pPlot == NULL)
	{
		pPlot = AI_getBestUnworkedYieldPlot(eYield);
	}
	if (pPlot == NULL)
	{
		return 0;
	}
	return pPlot->calculateBestNatureYield(eYield, getTeam());
}

void CvPlayerAI::AI_changeTotalIncome(int iChange)
{
	m_iTotalIncome += iChange;
}

int CvPlayerAI::AI_getTotalIncome()
{
	return m_iTotalIncome;
}

void CvPlayerAI::AI_changeHurrySpending(int iChange)
{
	m_iHurrySpending += iChange;
}

int CvPlayerAI::AI_getHurrySpending()
{
	return m_iHurrySpending;
}

// TAC - AI More Immigrants - koma13 - START
void CvPlayerAI::AI_changeImmigrantSpending(int iChange)
{
	m_iImmigrantSpending += iChange;
}

int CvPlayerAI::AI_getImmigrantSpending()
{
	return m_iImmigrantSpending;
}
// TAC - AI More Immigrants - koma13 - END

// TAC - AI Economy - koma13 - START
void CvPlayerAI::AI_changeEuropeYieldSpending(int iChange)
{
	m_iEuropeYieldSpending += iChange;
}

int CvPlayerAI::AI_getEuropeYieldSpending()
{
	return m_iEuropeYieldSpending;
}
// TAC - AI Economy - koma13 - END

// TAC - AI Improved Naval AI - koma13 - START
// Erik: Repurposed to return true for UnitAITypes types that should
// avoid enemies (unless protected) during pathfinding
bool CvPlayerAI::AI_needsProtection(UnitAITypes eUnitAI) const
{
	if (!isHuman())
	{
		switch (eUnitAI)
		{
			case UNITAI_COLONIST:
			case UNITAI_SETTLER:
			case UNITAI_WORKER:
			case UNITAI_MISSIONARY:
			case UNITAI_TRADER:
			case UNITAI_WAGON:
			case UNITAI_TREASURE:
			case UNITAI_GENERAL:
			case UNITAI_WORKER_SEA:
			case UNITAI_TRANSPORT_SEA:
			case UNITAI_TRANSPORT_COAST:
				return true;
				break;

			default:
				break;
		}
	}

	return false;
}
// TAC - AI Improved Naval AI - koma13 - END
int CvPlayerAI::AI_getPopulation()
{
	int iTotal = 0;

	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		iTotal += pLoopCity->getPopulation();
	}

	return iTotal;
}

bool CvPlayerAI::AI_shouldAttackAdjacentCity(CvPlot* pPlot)
{
	FAssert(pPlot != NULL);

	for (int i = 0; i < NUM_DIRECTION_TYPES; ++i)
	{
		CvPlot* pLoopPlot = plotDirection(pPlot->getX_INLINE(), pPlot->getY_INLINE(), (DirectionTypes)i);
		if (pLoopPlot != NULL)
		{
			if (atWar(getTeam(), pLoopPlot->getTeam()))
			{
				CvCity* pPlotCity = pLoopPlot->getPlotCity();
				if (pPlotCity != NULL)
				{
					if (!pPlotCity->isBombarded())
					{
						return true;
					}

					if (((100 * pPlotCity->getDefenseDamage()) / std::max(1, GC.getMAX_CITY_DEFENSE_DAMAGE())) > 90)
					{
						return true;
					}
				}
			}
		}
	}
	return false;
}

int CvPlayerAI::AI_getNumProfessionUnits(ProfessionTypes eProfession)
{
	int iCount = 0;

	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		for (int i = 0; i < pLoopCity->getPopulation(); ++i)
		{
			CvUnit* pLoopUnit = pLoopCity->getPopulationUnitByIndex(i);

			if (pLoopUnit->getProfession() == eProfession)
			{
				iCount ++;
			}
		}
	}

	return iCount;
}

int CvPlayerAI::AI_countNumCityUnits(UnitTypes eUnit)
{
	int iCount = 0;

	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		for (int i = 0; i < pLoopCity->getPopulation(); ++i)
		{
			CvUnit* pLoopUnit = pLoopCity->getPopulationUnitByIndex(i);

			if (pLoopUnit->getUnitType() == eUnit)
			{
				iCount ++;
			}
		}
	}

	return iCount;
}

int CvPlayerAI::AI_getNumCityUnitsNeeded(UnitTypes eUnit)
{
	int iCount = 0;

	// R&R, ray, code changes for Ideal Profession - START
	//ProfessionTypes eIdealProfession = AI_idealProfessionForUnit(eUnit);

	ProfessionTypes eIdealProfession = NO_PROFESSION; // only need one ideal profession, since we just check slots in building for same specialist
	const UnitClassTypes eUnitClassType = GC.getUnitInfo(eUnit).getUnitClassType();

	for (ProfessionTypes eLoopProfession = FIRST_PROFESSION; eLoopProfession < NUM_PROFESSION_TYPES; ++eLoopProfession)
	{
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eLoopProfession);

		if (kProfession.isCitizen())
		{
			if ((UnitClassTypes)kProfession.LbD_getExpert() == eUnitClassType)
			{
				if(kProfession.isWorkPlot()) // only specialists for building professions
				{
					break;
				}
				else
				{
					eIdealProfession = eLoopProfession;
					break;
				}
			}
		}
	}

	if (eIdealProfession == NO_PROFESSION)
	{
		return 0;
	}
	// R&R, ray , MYCP partially based on code of Aymerick - START
	YieldTypes eYieldProducedType = (YieldTypes)GC.getProfessionInfo(eIdealProfession).getYieldsProduced(0);
	// R&R, ray , MYCP partially based on code of Aymerick - END

	// R&R, ray, code changes for Ideal Profession - END

	int iLoop;
	CvCity* pLoopCity;
	for (pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity->AI_getYieldAdvantage(eYieldProducedType) == 100)
		{
			iCount += pLoopCity->getNumProfessionBuildingSlots(eIdealProfession);
		}
	}
	return iCount;
}

int CvPlayerAI::AI_countPromotions(PromotionTypes ePromotion, CvPlot* pPlot, int iRange, int* piUnitCount)
{
	CLLNode<IDInfo>* pUnitNode;
	CvUnit* pLoopUnit;
	int iCount = 0;
	int iUnitCount = 0;

	for (int iX = -iRange; iX <= iRange; ++iX)
	{
		for (int iY = -iRange; iY <= iRange; ++iY)
		{
			CvPlot* pLoopPlot = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iX, iY);
			if (pLoopPlot != NULL)
			{
				pUnitNode = pLoopPlot->headUnitNode();

				while (pUnitNode != NULL)
				{
					pLoopUnit = pLoopPlot->getUnitNodeLoop(pUnitNode);

					if (pLoopUnit != NULL && pLoopUnit->getOwnerINLINE() == getID())
					{
						if (pLoopUnit->isHasPromotion(ePromotion))
						{
							iCount++;
						}

						iUnitCount++;
					}
				}
			}
		}
	}

	if (piUnitCount != NULL)
	{
		*piUnitCount = iUnitCount;
	}
	return iCount;
}

// WTP: gold rules for civilian purchases: before turn 15 keep 300 gold, afterwards only buy with at least
// 1000 gold in the treasury (still keeping 300)
bool CvPlayerAI::AI_canAffordCivilianPurchase(int iCost) const
{
	if (iCost <= 0 || AI_isEarlyPhaseActive())
	{
		return false;
	}
	if (GC.getGameINLINE().getElapsedGameTurns() >= 15 && getGold() < 1000)
	{
		return false;
	}
	// WTP: while saving for a scout, a transport, the militia or the galleon that picks up the treasures
	if (AI_isSavingGold())
	{
		return false;
	}
	return getGold() - iCost >= 300;
}

// WTP: buy a unit in Europe, give it a unit AI and if needed equip it for a profession (bought there as well)
bool CvPlayerAI::AI_buyCivilianUnit(UnitTypes eUnit, UnitAITypes eUnitAI, ProfessionTypes eProfession)
{
	if (eUnit == NO_UNIT || !AI_canAffordCivilianPurchase(getEuropeUnitBuyPrice(eUnit)))
	{
		if (gPlayerLogLevel >= 1 && eUnit != NO_UNIT) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   cannot afford %S (%d gold, have %d)",
			GC.getUnitInfo(eUnit).getDescription(), getEuropeUnitBuyPrice(eUnit), getGold());
		return false;
	}
	CvUnit* pUnit = buyEuropeUnit(eUnit, 100);
	if (pUnit == NULL)
	{
		return false;
	}
	if (eProfession != NO_PROFESSION && pUnit->getProfession() != eProfession && pUnit->canHaveProfession(eProfession, false))
	{
		int iEquipCost = 0;
		for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
		{
			const int iMissing = pUnit->getProfessionChangeYieldRequired(eProfession, eYield);
			if (iMissing > 0)
			{
				iEquipCost += iMissing * GET_PLAYER(getParent()).getYieldSellPrice(eYield);
			}
		}
		if (getGold() >= iEquipCost)
		{
			changeProfessionEurope(pUnit->getID(), eProfession);
		}
	}
	pUnit->AI_setUnitAIType(eUnitAI);
	if (gPlayerLogLevel >= 1)
	{
		logBBAI("  Player %S buys civilian unit %S (unitAI %d)", getCivilizationDescription(), pUnit->getName().GetCString(), (int)eUnitAI);
	}
	return true;
}

// WTP: hurry the cheapest immigrant on the docks, if that is cheaper than buying a colonist
CvUnit* CvPlayerAI::AI_hurryCheapImmigrant(bool bUnconditional, bool bNonExpertOnly)
{
	const UnitTypes eColonist = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits((UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_COLONIST"));
	const int iColonistPrice = (eColonist != NO_UNIT) ? getEuropeUnitBuyPrice(eColonist) : MAX_INT;

	int iBestIndex = -1;
	int iBestCost = MAX_INT;
	for (unsigned int iIndex = 0; iIndex < getNumUnitsOnDock(); ++iIndex)
	{
		if (getDocksNextUnit(iIndex) != NO_UNIT && canHurry(HURRY_IMMIGRANT, iIndex)
			&& (!bNonExpertOnly || (AI_idealProfessionForUnit(getDocksNextUnit(iIndex)) == NO_PROFESSION
				&& GC.getUnitInfo(getDocksNextUnit(iIndex)).getDefaultProfession() == GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession())))
		{
			const int iCost = getHurryGold(HURRY_IMMIGRANT, iIndex);
			if (iCost < iBestCost)
			{
				iBestCost = iCost;
				iBestIndex = iIndex;
			}
		}
	}
	if (iBestIndex < 0 || (!bUnconditional && iBestCost >= iColonistPrice) || !AI_canAffordCivilianPurchase(iBestCost))
	{
		return NULL;
	}
	// the new immigrant is the one on the docks that was not there before
	std::vector<int> aiBefore;
	for (int i = 0; i < getNumEuropeUnits(); ++i)
	{
		aiBefore.push_back(getEuropeUnit(i)->getID());
	}
	hurry(HURRY_IMMIGRANT, iBestIndex);
	for (int i = 0; i < getNumEuropeUnits(); ++i)
	{
		CvUnit* const pUnit = getEuropeUnit(i);
		if (std::find(aiBefore.begin(), aiBefore.end(), pUnit->getID()) == aiBefore.end())
		{
			return pUnit;
		}
	}
	return NULL;
}

// WTP: priority of buying the expert of a city profession, 0 = not bought this way. Carpenters before preachers,
// preachers before statesmen; at most 5 of an expert, from the 4th on with half the priority
int CvPlayerAI::AI_civilianExpertPriority(ProfessionTypes eProfession, UnitClassTypes& eExpertClass)
{
	const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
	eExpertClass = (UnitClassTypes)kProfession.LbD_getExpert();
	if (!kProfession.isCitizen() || eExpertClass == NO_UNITCLASS || eExpertClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_COLONIST"))
	{
		return 0;
	}

	int iPriority = 0;
	const CvString szType = kProfession.getType();
	if (szType == "PROFESSION_CARPENTER") iPriority = 100;
	else if (szType == "PROFESSION_PREACHER") iPriority = 90;
	else if (szType == "PROFESSION_JUDGE") iPriority = 85;
	else if (szType == "PROFESSION_STATESMAN") iPriority = 80;
	else if (szType == "PROFESSION_LUMBERJACK" || szType == "PROFESSION_ORE_MINER" || szType == "PROFESSION_SILVER_MINER" || szType == "PROFESSION_GOLD_MINER"
		|| szType == "PROFESSION_FISHERMAN" || szType == "PROFESSION_FARMER" || szType == "PROFESSION_HUNTER" || szType == "PROFESSION_FUR_TRAPPER")
		iPriority = 70;
	else if (!kProfession.isWorkPlot() && kProfession.getYieldsConsumed(0) != NO_YIELD) iPriority = 40; // processing experts
	else return 0;

	const int iOwned = getUnitClassCount(eExpertClass);
	if (iOwned >= 5)
	{
		return 0;
	}
	if (iOwned >= 3)
	{
		iPriority /= 2;
	}
	return iPriority;
}

// WTP: once per turn, take one citizen out of a colony who does nothing useful (a free non-expert gathering a
// non-essential raw material on a plot without resource), if a native village nearby can teach it a profession
// or another colony has a free preacher, statesman or carpenter slot for it. Expert native traders always leave
// the colonies and found trade posts.
void CvPlayerAI::AI_doCitizenRelocation()
{
	MOD_PROFILE("CvPlayerAI::AI_doCitizenRelocation");
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || getNumCities() == 0)
	{
		return;
	}

	const ProfessionTypes eTraderProfession = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_NATIVE_TRADER");
	const ProfessionTypes eDefaultProfession = GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession();
	int iLoop;

	// expert native traders: never colonists
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->isExpertNativeTrader() && pLoopUnit->isOnMap() && !pLoopUnit->isCargo() && pLoopUnit->AI_getUnitAIType() != UNITAI_TRADER)
		{
			if (eTraderProfession != NO_PROFESSION && pLoopUnit->getProfession() != eTraderProfession && pLoopUnit->canHaveProfession(eTraderProfession, false))
			{
				pLoopUnit->setProfession(eTraderProfession);
			}
			pLoopUnit->AI_setUnitAIType(UNITAI_TRADER);
			if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP decision: Player %d (%S) turn %d sets %S back to native trader", getID(), getCivilizationDescription(0),
				GC.getGameINLINE().getGameTurn(), pLoopUnit->getName().GetCString());
		}
	}
	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		if (pCity->getPopulation() <= 1)
		{
			continue;
		}
		for (int i = 0; i < pCity->getPopulation(); ++i)
		{
			CvUnit* const pCitizen = pCity->getPopulationUnitByIndex(i);
			if (pCitizen != NULL && pCitizen->isExpertNativeTrader() && eTraderProfession != NO_PROFESSION && pCitizen->canHaveProfession(eTraderProfession, false))
			{
				if (pCity->removePopulationUnit(CREATE_ASSERT_DATA, pCitizen, false, eTraderProfession))
				{
					pCitizen->AI_setUnitAIType(UNITAI_TRADER);
					if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP decision: Player %d (%S) turn %d takes expert native trader %S out of %S",
						getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), pCitizen->getName().GetCString(), pCity->getName().GetCString());
				}
				break;
			}
		}
	}

	// citizens without a job: the workforce plan (AI_doWorkforcePlan) takes them out of the colony, they wait on its plot

	// more colonies wanted, but no settler on its way: a non-expert waiting for a job on a colony plot becomes the settler
	// (equipped by that colony); without one the largest colony (6 or more colonists) sends one of its least valuable
	// non-experts out as settler, once per turn. Without gold for settlers from Europe a single colony used to grow to
	// more than 20 colonists while the wanted colonies were never founded.
	const ProfessionTypes eSettlerJob = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_SETTLER");
	if (AI_desiredCityCount() > getNumCities() + AI_totalUnitAIs(UNITAI_SETTLER) && eSettlerJob != NO_PROFESSION)
	{
		for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
		{
			CvUnitAI& kLoopAI = static_cast<CvUnitAI&>(*pLoopUnit);
			const CvCity* const pHere = (pLoopUnit->isOnMap() && !pLoopUnit->isCargo() && pLoopUnit->plot() != NULL) ? pLoopUnit->plot()->getPlotCity() : NULL;
			if (kLoopAI.AI_getWfTask() == CvUnitAI::WF_WAIT && pLoopUnit->AI_getUnitAIType() == UNITAI_COLONIST && pHere != NULL && pHere->getOwnerINLINE() == getID()
				&& pLoopUnit->getUnitInfo().isFound() && pLoopUnit->AI_getIdealProfession() == NO_PROFESSION && pLoopUnit->canHaveProfession(eSettlerJob, false))
			{
				pLoopUnit->setProfession(eSettlerJob);
				kLoopAI.AI_clearWfTask();
				pLoopUnit->AI_setUnitAIType(UNITAI_SETTLER);
				if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_SETTLER, getID(), "WTP decision: Player %d (%S) turn %d %S waiting for a job in %S becomes a settler", getID(), getCivilizationDescription(0),
					GC.getGameINLINE().getGameTurn(), pLoopUnit->getName().GetCString(), pHere->getName().GetCString());
				break;
			}
		}
	}
	if (AI_desiredCityCount() > getNumCities() + AI_totalUnitAIs(UNITAI_SETTLER))
	{
		CvCity* pLargest = NULL;
		for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
		{
			if (pCity->getPopulation() >= 6 && pCity->area()->getBestFoundValue(getID()) > 0
				&& (pLargest == NULL || pCity->getPopulation() > pLargest->getPopulation()))
			{
				pLargest = pCity;
			}
		}
		// the colony has to equip the settler (tools, lumber, food); settlers should rather come from Europe
		const ProfessionTypes eSettlerProfession = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_SETTLER");
		bool bCanEquip = pLargest != NULL && eSettlerProfession != NO_PROFESSION;
		for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES && bCanEquip; ++eYield)
		{
			const int iAmount = GC.getProfessionInfo(eSettlerProfession).getYieldEquipmentAmount(eYield);
			if (iAmount > 0 && pLargest->getYieldStored(eYield) < iAmount)
			{
				bCanEquip = false;
			}
		}
		if (pLargest != NULL && bCanEquip)
		{
			CvUnit* pSettler = NULL;
			int iLowestValue = MAX_INT;
			for (int i = 0; i < pLargest->getPopulation(); ++i)
			{
				CvUnit* const pCitizen = pLargest->getPopulationUnitByIndex(i);
				// (the colonial AI's citizens are all locked by the workforce plan)
				if (pCitizen == NULL || !pCitizen->getUnitInfo().isFound() || pCitizen->AI_getIdealProfession() != NO_PROFESSION
					|| pCitizen->getProfession() == NO_PROFESSION)
				{
					continue;
				}
				const CvPlot* const pWorked = pLargest->getPlotWorkedByUnit(pCitizen);
				const int iValue = pLargest->AI().AI_citizenProfessionValue(pCitizen->getProfession(), *pCitizen, pWorked, NULL);
				if (iValue < iLowestValue)
				{
					iLowestValue = iValue;
					pSettler = pCitizen;
				}
			}
			if (pSettler != NULL && pLargest->removePopulationUnit(CREATE_ASSERT_DATA, pSettler, false,
				pSettler->canHaveProfession(eSettlerProfession, false) ? eSettlerProfession : eDefaultProfession))
			{
				pSettler->AI_setUnitAIType(UNITAI_SETTLER);
				if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_SETTLER, getID(), "WTP decision: Player %d (%S) turn %d %S leaves %S (%d colonists) as settler", getID(), getCivilizationDescription(0),
					GC.getGameINLINE().getGameTurn(), pSettler->getName().GetCString(), pLargest->getName().GetCString(), pLargest->getPopulation() + 1);
			}
		}
	}

	// WTP: food and idle citizens are handled by the workforce plan (AI_doWorkforcePlan)
}

// WTP: workforce plan helpers (the plan itself: AI_doWorkforcePlan)
namespace
{
	// the unit is an expert: it has an ideal profession, or it is an expert missionary (see wfIsExpertFor)
	// the profession the unit is an expert for; learning is none (its expert unit is the free colonist: a free colonist would
	// count as an expert and never study)
	ProfessionTypes wfIdealProfession(const CvUnitAI& kUnit)
	{
		const ProfessionTypes eIdeal = kUnit.AI_getIdealProfession();
		if (eIdeal != NO_PROFESSION && (YieldTypes)GC.getProfessionInfo(eIdeal).getYieldsProduced(0) == YIELD_EDUCATION)
		{
			return NO_PROFESSION;
		}
		return eIdeal;
	}

	bool wfIsExpert(const CvUnitAI& kUnit)
	{
		const CvUnitInfo& kInfo = kUnit.getUnitInfo();
		return wfIdealProfession(kUnit) != NO_PROFESSION
			|| (kInfo.NBMOD_GetTeachLevel() == 3 && (kInfo.getYieldChange(YIELD_CROSSES) > 0 || kInfo.getYieldModifier(YIELD_CROSSES) > 0));
	}

	// the unit is an expert for this profession; expert missionaries (university level, cross bonus) also count
	// as experts for the church jobs
	bool wfIsExpertFor(const CvUnitAI& kUnit, ProfessionTypes eProfession)
	{
		if (eProfession == NO_PROFESSION)
		{
			return false;
		}
		if (wfIdealProfession(kUnit) == eProfession)
		{
			return true;
		}
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
		const CvUnitInfo& kInfo = kUnit.getUnitInfo();
		const YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
		if (!kProfession.isWorkPlot() && eYield == YIELD_CROSSES && kInfo.NBMOD_GetTeachLevel() == 3
			&& (kInfo.getYieldChange(YIELD_CROSSES) > 0 || kInfo.getYieldModifier(YIELD_CROSSES) > 0))
		{
			return true;
		}
		// an expert for several trades (the prospector: silver, gold and gems; the miner: ore and salt): a large bonus on
		// the yield of the profession counts as expert; small side bonuses (+1) do not. Only real experts (with an ideal
		// profession): slaves, converted natives etc. have such bonuses too, but stay ordinary workers (their output is
		// counted with their own unit type anyway, and an expert may still be bought for their job)
		// The bonus only counts for the same kind of work as the ideal profession (plot or building, land or water): the
		// prospector gets no extra pearls on water, the farmer no extra food in the bakery.
		const ProfessionTypes eIdeal = wfIdealProfession(kUnit);
		if (eYield == NO_YIELD || eIdeal == NO_PROFESSION)
		{
			return false;
		}
		const CvProfessionInfo& kIdeal = GC.getProfessionInfo(eIdeal);
		return kIdeal.isWorkPlot() == kProfession.isWorkPlot() && kIdeal.isWater() == kProfession.isWater()
			&& (kInfo.getYieldChange(eYield) >= 2 || kInfo.getYieldModifier(eYield) >= 50);
	}
}

// the nearest native village (same landmass as the colony, at most iMaxDistance plots from it) that teaches eExpertClass
// and that no other unit of ours is heading for or learning in; NULL if there is none
CvCity* CvPlayerAI::AI_wfTeachingVillage(UnitClassTypes eExpertClass, const CvCity& kNearCity, int iMaxDistance) const
{
	CvCity* pBestVillage = NULL;
	int iBestDistance = MAX_INT;
	for (int iPlayer = 0; iPlayer < MAX_PLAYERS; ++iPlayer)
	{
		CvPlayer& kNative = GET_PLAYER((PlayerTypes)iPlayer);
		if (!kNative.isAlive() || !kNative.isNative() || GET_TEAM(getTeam()).isAtWar(kNative.getTeam()))
		{
			continue;
		}
		int iLoop;
		for (CvCity* pVillage = kNative.firstCity(&iLoop); pVillage != NULL; pVillage = kNative.nextCity(&iLoop))
		{
			const int iDistance = plotDistance(pVillage->getX_INLINE(), pVillage->getY_INLINE(), kNearCity.getX_INLINE(), kNearCity.getY_INLINE());
			if (pVillage->getTeachUnitClass() != eExpertClass || !pVillage->isScoutVisited(getTeam()) || pVillage->area() != kNearCity.area()
				|| iDistance > iMaxDistance || iDistance >= iBestDistance)
			{
				continue;
			}
			bool bTaken = false;
			int iUnitLoop;
			for (CvUnit* pLoopUnit = firstUnit(&iUnitLoop); pLoopUnit != NULL && !bTaken; pLoopUnit = nextUnit(&iUnitLoop))
			{
				const CvUnitAI& kLoopAI = static_cast<const CvUnitAI&>(*pLoopUnit);
				if ((kLoopAI.AI_getWfTask() == CvUnitAI::WF_LEARN || kLoopAI.AI_getWfTask() == CvUnitAI::WF_RETRAIN || kLoopAI.AI_getWfTask() == CvUnitAI::WF_FOOD_LEARN)
					&& kLoopAI.AI_getWfVillageX() == pVillage->getX_INLINE() && kLoopAI.AI_getWfVillageY() == pVillage->getY_INLINE())
				{
					bTaken = true;
				}
				else if (pLoopUnit->getUnitTravelState() == UNIT_TRAVEL_STATE_LIVE_AMONG_NATIVES && pLoopUnit->getX_INLINE() == pVillage->getX_INLINE()
					&& pLoopUnit->getY_INLINE() == pVillage->getY_INLINE())
				{
					bTaken = true;
				}
			}
			if (!bTaken)
			{
				pBestVillage = pVillage;
				iBestDistance = iDistance;
			}
		}
	}
	return pBestVillage;
}

// WTP: the food surplus a colony keeps after one more colonist joins it (he eats 2), counting the colonists already on
// their way to join it (except iSkipUnitID)
int CvPlayerAI::AI_foodSurplusForNewcomer(const CvCity& kCity, int iSkipUnitID) const
{
	int iIncoming = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		const CvUnitAI& kLoopAI = static_cast<const CvUnitAI&>(*pLoopUnit);
		if (pLoopUnit->getID() != iSkipUnitID && kLoopAI.AI_getWfTask() == CvUnitAI::WF_JOIN_CITY && kLoopAI.AI_getWfCityID() == kCity.getID())
		{
			++iIncoming;
		}
	}
	return kCity.foodDifference() - 2 * iIncoming - GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION;
}

// WTP: the colony (reachable from kFrom over land or by sea) with the most food to spare: at least +2 must stay after
// the newcomer and those already on their way joined; NULL if there is none
CvCity* CvPlayerAI::AI_bestFoodSurplusCity(const CvCity& kFrom, const CvCity* pExclude, int iSkipUnitID) const
{
	CvCity* pBest = NULL;
	int iBestSurplus = 1;
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity == pExclude)
		{
			continue;
		}
		const bool bReachable = pLoopCity->area() == kFrom.area()
			|| (pLoopCity->isCoastal(GC.getMIN_WATER_SIZE_FOR_OCEAN()) && kFrom.isCoastal(GC.getMIN_WATER_SIZE_FOR_OCEAN()));
		const int iSurplus = AI_foodSurplusForNewcomer(*pLoopCity, iSkipUnitID);
		if (bReachable && iSurplus >= 2 && iSurplus > iBestSurplus)
		{
			iBestSurplus = iSurplus;
			pBest = pLoopCity;
		}
	}
	return pBest;
}

// WTP: experts that never give up their specialty (renowned medics, expert farmers, expert fishermen)
bool CvPlayerAI::AI_isProtectedSpecialty(UnitClassTypes eUnitClass)
{
	return eUnitClass != NO_UNITCLASS && (eUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_DOCTOR")
		|| eUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FARMER")
		|| eUnitClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FISHERMAN"));
}

// WTP: a unit waiting in a village to learn: the number of turns it waits (bStart: it waits from now on if not yet)
int CvPlayerAI::AI_wfLearnWaitTurns(int iUnitID, bool bStart)
{
	const int iTurn = GC.getGameINLINE().getGameTurn();
	std::map<int, int>::iterator it = m_mapWfLearnWait.find(iUnitID);
	if (it == m_mapWfLearnWait.end())
	{
		if (bStart)
		{
			m_mapWfLearnWait[iUnitID] = iTurn;
		}
		return 0;
	}
	return iTurn - it->second;
}

void CvPlayerAI::AI_clearWfLearnWait(int iUnitID)
{
	m_mapWfLearnWait.erase(iUnitID);
}

// WTP: a colonist found no way to a colony (no path over land, no ship): the plan does not send it there for 10 turns
void CvPlayerAI::AI_setWfUnreachable(int iUnitID, int iCityID)
{
	m_mapWfUnreachable[std::make_pair(iUnitID, iCityID)] = GC.getGameINLINE().getGameTurn();
}

bool CvPlayerAI::AI_isWfUnreachable(int iUnitID, int iCityID) const
{
	const std::map<std::pair<int, int>, int>::const_iterator it = m_mapWfUnreachable.find(std::make_pair(iUnitID, iCityID));
	return it != m_mapWfUnreachable.end() && GC.getGameINLINE().getGameTurn() - it->second < 10;
}

// WTP: travel times for the workforce plan. For each colony the movement cost of a colonist from every plot to the colony
// over land (the movement costs of the terrain, peaks without a road and foreign colonies are no way; up to 40 turns), and
// for each pair of coastal colonies the turns a transport needs between them
void CvPlayerAI::AI_wfUpdateTravel(const CvUnit& kColonist)
{
	MOD_PROFILE("CvPlayerAI::AI_wfUpdateTravel");
	m_mapWfLandCost.clear();
	m_mapWfSeaTurns.clear();
	const CvMap& kMap = GC.getMap();
	const int iNumPlots = kMap.numPlotsINLINE();
	m_iWfLandMoves = std::max(1, kColonist.maxMoves());
	const int iMaxCost = 40 * m_iWfLandMoves;
	int iLoop;
	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		std::vector<int>& aiCost = m_mapWfLandCost[pCity->getID()];
		aiCost.assign(iNumPlots, MAX_INT);
		// Dijkstra backwards from the colony: the cost of a plot is the cost of the way from it to the colony
		std::priority_queue<std::pair<int, int> > aQueue; // (-cost, plot number)
		const int iStart = kMap.plotNumINLINE(pCity->getX_INLINE(), pCity->getY_INLINE());
		aiCost[iStart] = 0;
		aQueue.push(std::make_pair(0, iStart));
		while (!aQueue.empty())
		{
			const int iCost = -aQueue.top().first;
			const int iPlot = aQueue.top().second;
			aQueue.pop();
			if (iCost > aiCost[iPlot])
			{
				continue;
			}
			const CvPlot* const pTo = kMap.plotByIndexINLINE(iPlot);
			for (int iI = 0; iI < NUM_DIRECTION_TYPES; ++iI)
			{
				const CvPlot* const pFrom = plotDirection(pTo->getX_INLINE(), pTo->getY_INLINE(), (DirectionTypes)iI);
				if (pFrom == NULL || pFrom->isWater() || pFrom->isImpassable() || (pFrom->isPeak() && pFrom->getRouteType() == NO_ROUTE)
					|| (pFrom->isCity() && pFrom->getOwnerINLINE() != getID()))
				{
					continue;
				}
				const int iNewCost = iCost + pTo->movementCost(&kColonist, pFrom);
				const int iFrom = kMap.plotNumINLINE(pFrom->getX_INLINE(), pFrom->getY_INLINE());
				if (iNewCost < aiCost[iFrom] && iNewCost <= iMaxCost)
				{
					aiCost[iFrom] = iNewCost;
					aQueue.push(std::make_pair(-iNewCost, iFrom));
				}
			}
		}
	}

	// the transports: the fastest one sets the pace; a colony counts as a port if one of our transports reaches it
	int iShipMoves = 0;
	std::vector<UnitTypes> aeTransports;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA || pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_COAST)
		{
			iShipMoves = std::max(iShipMoves, pLoopUnit->baseMoves());
			aeTransports.push_back(pLoopUnit->getUnitType());
		}
	}
	if (iShipMoves <= 0)
	{
		return;
	}
	std::vector<int> aiSteps(iNumPlots, MAX_INT);
	for (CvCity* pPort = firstCity(&iLoop); pPort != NULL; pPort = nextCity(&iLoop))
	{
		bool bPort = false;
		for (uint i = 0; i < aeTransports.size() && !bPort; ++i)
		{
			bPort = AI_shipCanReachCity(aeTransports[i], *pPort);
		}
		if (!bPort)
		{
			continue;
		}
		// breadth first over the water from the port; a colony next to the water reached is reached by ship
		std::fill(aiSteps.begin(), aiSteps.end(), MAX_INT);
		std::deque<int> aOpen;
		const int iStart = kMap.plotNumINLINE(pPort->getX_INLINE(), pPort->getY_INLINE());
		aiSteps[iStart] = 0;
		aOpen.push_back(iStart);
		while (!aOpen.empty())
		{
			const int iPlot = aOpen.front();
			aOpen.pop_front();
			const CvPlot* const pPlot = kMap.plotByIndexINLINE(iPlot);
			const int iSteps = aiSteps[iPlot];
			if (iSteps >= 100)
			{
				continue;
			}
			for (int iI = 0; iI < NUM_DIRECTION_TYPES; ++iI)
			{
				const CvPlot* const pNext = plotDirection(pPlot->getX_INLINE(), pPlot->getY_INLINE(), (DirectionTypes)iI);
				if (pNext == NULL)
				{
					continue;
				}
				const int iNext = kMap.plotNumINLINE(pNext->getX_INLINE(), pNext->getY_INLINE());
				if (aiSteps[iNext] != MAX_INT)
				{
					continue;
				}
				if (pNext->isWater())
				{
					aiSteps[iNext] = iSteps + 1;
					aOpen.push_back(iNext);
				}
				else if (pPlot->isWater() && pNext->isCity() && pNext->getOwnerINLINE() == getID())
				{
					aiSteps[iNext] = iSteps + 1;
					CvCity* const pTarget = pNext->getPlotCity();
					bool bReach = false;
					for (uint i = 0; i < aeTransports.size() && !bReach; ++i)
					{
						bReach = AI_shipCanReachCity(aeTransports[i], *pTarget);
					}
					if (bReach)
					{
						// baseMoves: plots per turn
						m_mapWfSeaTurns[std::make_pair(pPort->getID(), pTarget->getID())] = (iSteps + 1 + iShipMoves - 1) / iShipMoves;
					}
				}
			}
		}
	}
}

// WTP: turns a colonist on kFrom needs to the colony kTo: over land, or walking to a port, waiting for a transport there
// and sailing (the faster way; the ship only if it saves 2 turns). piPortCityID gets the port (-1: over land). MAX_INT if
// there is no way (or no travel times yet)
int CvPlayerAI::AI_wfTravelTurns(const CvPlot& kFrom, const CvCity& kTo, int* piPortCityID, bool bNoShip, bool bShipThenWalk) const
{
	// waiting for a transport and boarding
	static const int WF_SHIP_WAIT = 3;
	if (piPortCityID != NULL)
	{
		*piPortCityID = -1;
	}
	const int iFrom = GC.getMap().plotNumINLINE(kFrom.getX_INLINE(), kFrom.getY_INLINE());
	int iLand = MAX_INT;
	const std::map<int, std::vector<int> >::const_iterator itTo = m_mapWfLandCost.find(kTo.getID());
	if (itTo != m_mapWfLandCost.end() && itTo->second[iFrom] != MAX_INT)
	{
		iLand = (itTo->second[iFrom] + m_iWfLandMoves - 1) / m_iWfLandMoves;
	}
	int iShip = MAX_INT;
	int iPort = -1;
	for (std::map<std::pair<int, int>, int>::const_iterator it = m_mapWfSeaTurns.begin(); it != m_mapWfSeaTurns.end() && !bNoShip; ++it)
	{
		if (it->first.second != kTo.getID())
		{
			continue;
		}
		const std::map<int, std::vector<int> >::const_iterator itPort = m_mapWfLandCost.find(it->first.first);
		if (itPort == m_mapWfLandCost.end() || itPort->second[iFrom] == MAX_INT)
		{
			continue;
		}
		const int iTurns = (itPort->second[iFrom] + m_iWfLandMoves - 1) / m_iWfLandMoves + WF_SHIP_WAIT + it->second;
		if (iTurns < iShip)
		{
			iShip = iTurns;
			iPort = it->first.first;
		}
	}
	// WTP: bShipThenWalk (human job search): also by ship to another colony and on foot from there (a colony inland on another
	// coast); the ship of Transport to City may put the colonist ashore nearer, this is the estimate
	if (bShipThenWalk && !bNoShip && itTo != m_mapWfLandCost.end())
	{
		for (std::map<std::pair<int, int>, int>::const_iterator it = m_mapWfSeaTurns.begin(); it != m_mapWfSeaTurns.end(); ++it)
		{
			const CvCity* const pLanding = getCity(it->first.second);
			if (it->first.second == kTo.getID() || pLanding == NULL)
			{
				continue;
			}
			const int iOnFoot = itTo->second[GC.getMap().plotNumINLINE(pLanding->getX_INLINE(), pLanding->getY_INLINE())];
			const std::map<int, std::vector<int> >::const_iterator itPort = m_mapWfLandCost.find(it->first.first);
			if (iOnFoot == MAX_INT || itPort == m_mapWfLandCost.end() || itPort->second[iFrom] == MAX_INT)
			{
				continue;
			}
			const int iTurns = (itPort->second[iFrom] + m_iWfLandMoves - 1) / m_iWfLandMoves + WF_SHIP_WAIT + it->second + (iOnFoot + m_iWfLandMoves - 1) / m_iWfLandMoves;
			if (iTurns < iShip)
			{
				iShip = iTurns;
				iPort = it->first.first;
			}
		}
	}
	if (iShip != MAX_INT && (iLand == MAX_INT || iShip + 2 <= iLand))
	{
		if (piPortCityID != NULL)
		{
			*piPortCityID = iPort;
		}
		return iShip;
	}
	return iLand;
}

// WTP: learning the expert was blocked for 15 turns: the expert is bought instead (in Europe or Africa), saving gold for it
void CvPlayerAI::AI_setWfExpertToBuy(UnitClassTypes eExpertClass)
{
	m_eWfExpertToBuy = eExpertClass;
}

bool CvPlayerAI::AI_doWfExpertPurchase()
{
	if (m_eWfExpertToBuy == NO_UNITCLASS || isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER)
	{
		return false;
	}
	const UnitTypes eExpert = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(m_eWfExpertToBuy);
	const int iEuropePrice = (eExpert != NO_UNIT && canTradeWithEurope()) ? getEuropeUnitBuyPrice(eExpert) : -1;
	const int iAfricaPrice = (eExpert != NO_UNIT) ? getAfricaUnitBuyPrice(eExpert) : -1;
	const bool bAfrica = iAfricaPrice > 0 && (iEuropePrice <= 0 || iAfricaPrice < iEuropePrice);
	const int iPrice = bAfrica ? iAfricaPrice : iEuropePrice;
	if (iPrice <= 0)
	{
		m_eWfExpertToBuy = NO_UNITCLASS; // not for sale
		return false;
	}
	if (getGold() < iPrice)
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) saves for %S in %s (%d gold, have %d): learning it was blocked", getID(),
			getCivilizationDescription(0), GC.getUnitClassInfo(m_eWfExpertToBuy).getDescription(), bAfrica ? "Africa" : "Europe", iPrice, getGold());
		return true;
	}
	CvUnit* const pUnit = bAfrica ? buyAfricaUnit(eExpert, 100) : buyEuropeUnit(eExpert, 100);
	if (pUnit != NULL)
	{
		pUnit->AI_setUnitAIType(UNITAI_COLONIST);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) buys %S in %s instead of learning it", getID(), getCivilizationDescription(0),
			pUnit->getName().GetCString(), bAfrica ? "Africa" : "Europe");
	}
	m_eWfExpertToBuy = NO_UNITCLASS;
	return false;
}

// WTP: an expert whose specialty is useless: the raw materials of its expert job are nowhere near the colonies (no
// colony produces them, no plot within the colonies' radius + 1 can); aiYieldAvailable caches the yields (-1 unknown)
bool CvPlayerAI::AI_isUselessExpert(const CvUnitAI& kUnit, std::vector<int>& aiYieldAvailable)
{
	const int iRadius = GC.getGameINLINE().isOption(GAMEOPTION_TWO_PLOT_CITY_RADIUS) ? 2 : 1;
	const ProfessionTypes eIdeal = wfIdealProfession(kUnit);
	if (eIdeal == NO_PROFESSION || AI_isProtectedSpecialty(kUnit.getUnitClassType()))
	{
		return false;
	}
	const CvProfessionInfo& kIdeal = GC.getProfessionInfo(eIdeal);
	std::vector<YieldTypes> aeRaw;
	if (kIdeal.isWorkPlot())
	{
		aeRaw.push_back((YieldTypes)kIdeal.getYieldsProduced(0));
	}
	else
	{
		for (int j = 0; j < kIdeal.getNumYieldsConsumed(); ++j)
		{
			aeRaw.push_back((YieldTypes)kIdeal.getYieldsConsumed(j));
		}
	}
	// all inputs have to be available (an expert needing two goods is useless if one of them is missing)
	bool bUseful = true;
	for (uint j = 0; j < aeRaw.size() && bUseful; ++j)
	{
		const YieldTypes eRaw = aeRaw[j];
		if (eRaw == NO_YIELD)
		{
			continue;
		}
		if (aiYieldAvailable[eRaw] < 0)
		{
			aiYieldAvailable[eRaw] = 0;
			int iCityLoop;
			for (CvCity* pLoopCity = firstCity(&iCityLoop); pLoopCity != NULL && aiYieldAvailable[eRaw] == 0; pLoopCity = nextCity(&iCityLoop))
			{
				if (pLoopCity->getRawYieldProduced(eRaw) > 0)
				{
					aiYieldAvailable[eRaw] = 1;
					break;
				}
				for (int iDX = -(iRadius + 1); iDX <= iRadius + 1 && aiYieldAvailable[eRaw] == 0; ++iDX)
				{
					for (int iDY = -(iRadius + 1); iDY <= iRadius + 1; ++iDY)
					{
						const CvPlot* const pLoopPlot = plotXY(pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE(), iDX, iDY);
						if (pLoopPlot != NULL && pLoopPlot->calculateBestNatureYield(eRaw, getTeam()) > 0)
						{
							aiYieldAvailable[eRaw] = 1;
							break;
						}
					}
				}
			}
		}
		bUseful = aiYieldAvailable[eRaw] > 0;
	}
	return !bUseful;
}

// WTP: workforce plan (general job planner of the colonial AI). Every turn the AI plans the jobs of all its colonists at once:
// 1. jobs: every workable plot of every colony (the best food and the best other professions, alternatives of one plot)
//    and every building slot, valued in gold (x100) per turn (inputs count half; hammers 15/10/8 gold for the 1st/2nd/3rd+
//    carpenter, who pay 5 gold per lumber; bells and crosses 10 gold). Food counts 4 gold per unit (flat),
//    a colonist eats 2 food. Side bonuses of experts are ignored: an expert outside its trade works like a free colonist.
// 2. workforce: the citizens, the colonists waiting on a colony plot or on their way, the colonists on ships and on the docks
//    in Europe, the units learning in a native village (they are bound to the job they learn for). Experts already working
//    or reserved for their expert job keep it (an expert bought or learning for a job is its reservation).
// 3. target plan: the most valuable (colonist, job) pairs, one after the other, as long as every colony keeps a food surplus
//    of at least 3 when all planned colonists have arrived; a job that needs more food is taken together with a food job.
//    Colonists far away count less (travel time), the job a colonist holds already counts more (no flip-flopping).
// 4. seats now: of the planned colonists that are in the colony those get in that the present food carries (food jobs
//    first). A job whose planned holder is still on its way may be worked by an idle colonist meanwhile.
// 5. carried out: citizens without a seat leave the colony and wait on its plot (they eat nothing there), seated colonists
//    join and get their job (locked: the colony's own job assignment leaves them alone), the others go to their colony.
// 6. blocked jobs (no food or no colonist): the colonist with the best value per price is bought (CvPlayerAI::
//    AI_buyExpertForOpportunity): an expert for a job, or a food provider; colonies short of food ask the pioneers for food
//    improvements. Free colonists near a village that teaches the expert of a valuable job learn it.
namespace
{
	const int WF_FOOD_GOLD = 400;      // gold x100 per food (4 gold flat, user decision)
	const int WF_FOOD_GOLD_HIGH = 400; // per food above the green zone (the same since 4 gold flat)
	const int WF_FOOD_MIN = 3;         // planned food surplus: at least 3 ...
	const int WF_FOOD_HIGH = 10;       // ... up to 10 is the green zone

	struct WfJob
	{
		CvCity* pCity;
		int iCity;
		ProfessionTypes eProfession;
		CvPlot* pPlot;          // NULL for building jobs
		int iPlotKey;           // plot number: the alternatives of a plot exclude each other; -1 for building jobs
		UnitClassTypes eExpertClass;
		YieldTypes eYield;
		bool bFood;
		bool bNotable;          // resource or improvement plot, or a building job: worth learning or buying an expert for
		int iSlot;              // building jobs: the how-manieth worker of this profession in the colony (0 = first)
		int iGroup;             // building jobs: the building (per colony) whose slots all its professions share, -1 for plot jobs
		int iCurrent;           // worker that works it now (or holds it as task), -1
		int iHolder;            // worker of the target plan, -1
		int iSeated;            // worker seated now, -1
	};

	struct WfWorker
	{
		CvUnitAI* pUnit;
		CvCity* pPopCity;       // citizen of this colony
		CvCity* pAtCity;        // waiting on the plot of this colony
		int iCurrentJob;        // job it works now or holds as task, -1
		int iJob;               // job of the target plan, -1
		int iTypeIdx;           // index of its unit type in the output table
		bool bExpert;           // has an expert profession
		bool bLearning;         // bound to its learning task
		bool bReserved;         // expert bound to its expert job (works it or is on its way)
		int iSeatCity;          // city index it is seated in now, -1
	};

	struct WfCity
	{
		CvCity* pCity;
		int iBaseFood;          // food of the colony without citizens
		int iSurplus;           // planned surplus of the target plan
		int iNowSurplus;        // surplus of the colonists seated now
		int iHeld;              // planned colonists
		int iBlockedValue;      // best job left out for lack of food (with a colonist free for it)
		int iBlockedJob;
		std::vector<int> aiRaw; // planned raw production per yield (center plot included)
		std::vector<int> aiUse; // planned consumption per yield
		std::vector<int> aiStock; // part of aiRaw that comes from the stock (a tenth of it), not from production
	};

	struct WfCand
	{
		int iWorker;
		int iJob;
		int iValue;
		int iFoodDelta;
	};

	// gold x100 of iDelta food (positive: produced, negative: eaten) at a planned surplus of iSurplus
	int wfFoodGold(int iSurplus, int iDelta)
	{
		int iGold = 0;
		if (iDelta >= 0)
		{
			for (int i = 0; i < iDelta; ++i)
			{
				iGold += (iSurplus + i < WF_FOOD_HIGH) ? WF_FOOD_GOLD : WF_FOOD_GOLD_HIGH;
			}
		}
		else
		{
			for (int i = 1; i <= -iDelta; ++i)
			{
				iGold -= (iSurplus - i < WF_FOOD_HIGH) ? WF_FOOD_GOLD : WF_FOOD_GOLD_HIGH;
			}
		}
		return iGold;
	}

	// output of a job as the plot is now: AI_professionBasicOutput also counts a plot job as if its feature (forest) were
	// cleared, the plan needs what the colonist produces today
	int wfOutput(const CvCity& kCity, ProfessionTypes eProfession, UnitTypes eUnit, const CvPlot* pPlot)
	{
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
		if (pPlot == NULL || !kProfession.isWorkPlot())
		{
			return kCity.AI_professionBasicOutput(eProfession, eUnit, pPlot);
		}
		const YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
		// a land profession produces nothing on water and a water profession nothing on land (a farmer on a fish plot)
		if (eYield == NO_YIELD || kProfession.isWater() != pPlot->isWater())
		{
			return 0;
		}
		return pPlot->calculatePotentialYield(eYield, kCity.getOwnerINLINE(), pPlot->getImprovementType(), false, pPlot->getRouteType(), eUnit, false);
	}

	// food of a job whose profession produces iOutput of its first yield: all of it for food jobs; jobs with food as second
	// yield (hunters, fruit pickers, rice farmers, butchers) give food as well: half of the first yield on a plot
	// (CvPlot::calculatePotentialProfessionYieldAmount), as much as the first yield in a building
	int wfFoodOutput(ProfessionTypes eProfession, int iOutput)
	{
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
		if ((YieldTypes)kProfession.getYieldsProduced(0) == YIELD_FOOD)
		{
			return iOutput;
		}
		for (int i = 1; i < kProfession.getNumYieldsProduced(); ++i)
		{
			if ((YieldTypes)kProfession.getYieldsProduced(i) == YIELD_FOOD)
			{
				return kProfession.isWorkPlot() ? iOutput / 2 : iOutput;
			}
		}
		return 0;
	}

	const int WF_LUMBER_GOLD = 500; // gold x100 of lumber the carpenters use (the lumberjack for them earns it)

	// gold x100 of one unit of a yield: the price Europe pays for it (AI_estimateYieldValue adds the AI base value of the
	// XML, +2 per unit, which made cheap raw materials worth 40% more); yields Europe does not buy keep the estimate
	int wfSellPrice(const CvCity& kCity, YieldTypes eYield)
	{
		// lumber at a fixed 3 gold (Europe pays 1, it is worth more as building material; user decision)
		if (eYield == YIELD_LUMBER)
		{
			return 300;
		}
		const PlayerTypes eParent = GET_PLAYER(kCity.getOwnerINLINE()).getParent();
		const int iPrice = (eParent != NO_PLAYER) ? GET_PLAYER(eParent).getYieldBuyPrice(eYield) : 0;
		return (iPrice > 0) ? 100 * iPrice : 100 * kCity.AI_estimateYieldValue(eYield, 1);
	}

	// gold x100 of a job's output (iSlot: the how-manieth worker of the profession in the colony). Hammers are staged: 15 gold
	// for the first carpenter, 10 for the second, 8 from the third on (the lumber is paid from it, see WF_LUMBER_GOLD);
	// bells and crosses 10 gold; everything else at the price Europe pays
	int wfYieldGold(const CvCity& kCity, YieldTypes eYield, int iAmount, int iSlot)
	{
		switch (eYield)
		{
		case YIELD_HAMMERS:
			return iAmount * (iSlot == 0 ? 1500 : (iSlot == 1 ? 1000 : 800));
		case YIELD_BELLS:
		case YIELD_CROSSES:
			return iAmount * 1000;
		default:
			return iAmount * wfSellPrice(kCity, eYield);
		}
	}

	// gold x100 of the input a building job uses: the lumber of the carpenters at WF_LUMBER_GOLD, other inputs at the price
	// Europe pays for them (the raw material could be sold instead; at half price experts were overrated)
	int wfInputGold(const CvCity& kCity, YieldTypes eOutput, YieldTypes eInput, int iAmount)
	{
		if (eOutput == YIELD_HAMMERS && eInput == YIELD_LUMBER)
		{
			return iAmount * WF_LUMBER_GOLD;
		}
		return iAmount * wfSellPrice(kCity, eInput);
	}

	bool wfJobMatches(const WfJob& kJob, int iCityID, ProfessionTypes eProfession, int iX, int iY)
	{
		if (kJob.pCity->getID() != iCityID || kJob.eProfession != eProfession)
		{
			return false;
		}
		if (kJob.pPlot == NULL)
		{
			return iX < 0;
		}
		return kJob.pPlot->getX_INLINE() == iX && kJob.pPlot->getY_INLINE() == iY;
	}

	const char* wfJobStatus(const WfJob& kJob, const std::vector<WfWorker>& aWorkers)
	{
		const bool bHolderExpert = kJob.iHolder >= 0 && wfIsExpertFor(*aWorkers[kJob.iHolder].pUnit, kJob.eProfession);
		if (kJob.iSeated >= 0 && kJob.iSeated == kJob.iHolder)
		{
			return bHolderExpert ? "expert" : "regular";
		}
		if (kJob.iSeated >= 0)
		{
			return bHolderExpert ? "regular, expert on its way" : "regular, colonist on its way";
		}
		if (kJob.iHolder >= 0)
		{
			const WfWorker& kHolder = aWorkers[kJob.iHolder];
			if (kHolder.pPopCity == kJob.pCity || kHolder.pAtCity == kJob.pCity)
			{
				// seated in another job: it stands in for a colonist on its way
				return (kHolder.iSeatCity >= 0) ? "planned, stands in elsewhere" : "planned, waits for food";
			}
			return bHolderExpert ? "free, expert on its way" : "planned, colonist on its way";
		}
		return "free";
	}
}

// WTP: in peace the colonial AI uses its warships with cargo room as transports, all but the strongest one, which keeps
// guarding the coast (sea goodies, pirates, sea animals). At war (or with war plans) they become warships again as soon as
// they are empty
void CvPlayerAI::AI_doPeaceWarships()
{
	if (!AI_isWorkforcePlanned())
	{
		return;
	}
	const CvTeamAI& kTeam = GET_TEAM(getTeam());
	// (wars with natives do not count: they have no ships; the Danes kept five warships idle in such a war)
	bool bWar = kTeam.getAnyWarPlanCount() > 0;
	for (PlayerTypes ePlayer = FIRST_PLAYER; ePlayer < NUM_PLAYER_TYPES && !bWar; ++ePlayer)
	{
		const CvPlayer& kOther = GET_PLAYER(ePlayer);
		bWar = kOther.isAlive() && kOther.getTeam() != getTeam() && !kOther.isNative() && !GC.getGameINLINE().isBarbarianPlayer(ePlayer)
			&& kTeam.isAtWar(kOther.getTeam());
	}
	CvUnit* pStrongest = NULL;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getUnitInfo().getDefaultUnitAIType() == UNITAI_COMBAT_SEA && pLoopUnit->cargoSpace() > 0
			&& (pStrongest == NULL || pLoopUnit->baseCombatStr() > pStrongest->baseCombatStr()))
		{
			pStrongest = pLoopUnit;
		}
	}
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getUnitInfo().getDefaultUnitAIType() != UNITAI_COMBAT_SEA || pLoopUnit->cargoSpace() <= 0)
		{
			continue;
		}
		const bool bTransport = pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA;
		const bool bWantTransport = !bWar && pLoopUnit != pStrongest;
		if (bWantTransport && pLoopUnit->AI_getUnitAIType() == UNITAI_COMBAT_SEA)
		{
			pLoopUnit->AI_setUnitAIType(UNITAI_TRANSPORT_SEA);
			if (gUnitLogLevel >= 1) logWTP(WTP_LOG_SHIP, getID(), "WTP ship: Player %d (%S) warship %S carries goods and colonists in peace", getID(), getCivilizationDescription(0),
				pLoopUnit->getName().GetCString());
		}
		else if (!bWantTransport && bTransport && !pLoopUnit->hasCargo())
		{
			pLoopUnit->AI_setUnitAIType(UNITAI_COMBAT_SEA);
			if (gUnitLogLevel >= 1) logWTP(WTP_LOG_SHIP, getID(), "WTP ship: Player %d (%S) %S is a warship again (%s)", getID(), getCivilizationDescription(0),
				pLoopUnit->getName().GetCString(), bWar ? "war" : "the strongest one guards the coast");
		}
	}
}

// WTP: since when the storage of each colony is more than 90% full (a reason to buy another transport)
void CvPlayerAI::AI_updateFullStorage()
{
	if (isHuman() || isNative() || isEurope())
	{
		return;
	}
	const int iTurn = GC.getGameINLINE().getGameTurn();
	std::map<int, int> mapNew;
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		if (pLoopCity->getMaxYieldCapacity() > 0 && pLoopCity->getTotalYieldStored() * 10 > pLoopCity->getMaxYieldCapacity() * 9)
		{
			const std::map<int, int>::const_iterator it = m_mapFullStorageSince.find(pLoopCity->getID());
			mapNew[pLoopCity->getID()] = (it != m_mapFullStorageSince.end()) ? it->second : iTurn;
		}
	}
	m_mapFullStorageSince.swap(mapNew);
}

int CvPlayerAI::AI_numWarshipsAsTransports() const
{
	int iCount = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getUnitInfo().getDefaultUnitAIType() == UNITAI_COMBAT_SEA && pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA)
		{
			++iCount;
		}
	}
	return iCount;
}

bool CvPlayerAI::AI_isWorkforcePlanned() const
{
	return !isHuman() && !isNative() && !isEurope() && getParent() != NO_PLAYER;
}

void CvPlayerAI::AI_doWorkforcePlan()
{
	MOD_PROFILE("CvPlayerAI::AI_doWorkforcePlan");
	m_aWfOpenOpportunities.clear();
	m_iWfIdleCount = 0;
	m_iWfFoodCityID = -1;
	m_bWfFoodWater = false;
	if (!AI_isWorkforcePlanned() || getNumCities() == 0)
	{
		return;
	}

	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	const int iTurn = GC.getGameINLINE().getGameTurn();
	const ProfessionTypes eDefaultProfession = kCiv.getDefaultProfession();
	const UnitTypes eColonistUnit = kCiv.getCivilizationUnits(GLOBAL_DEFINE_DEFAULT_POPULATION_UNIT);
	const int iConsumption = GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION;
	int iLoop;

	// 0. experts working in a colony whose raw material is nowhere to be had (no plot within the colonies' radius + 1 can
	// produce it, no colony produces it) become free colonists right away: these can still become experts by learning by
	// doing. Experts trained at the university (teach level 3) always stay.
	{
		std::vector<int> aiYieldAvailable(NUM_YIELD_TYPES, -1); // -1 unknown, 0 no, 1 yes
		std::vector<CvUnit*> apToClear;
		for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
		{
			for (int i = 0; i < pCity->getPopulation(); ++i)
			{
				CvUnitAI* const pCitizen = static_cast<CvUnitAI*>(pCity->getPopulationUnitByIndex(i));
				if (pCitizen == NULL || !pCitizen->canClearSpecialty() || pCitizen->getUnitInfo().NBMOD_GetTeachLevel() >= 3)
				{
					continue;
				}
				if (AI_isUselessExpert(*pCitizen, aiYieldAvailable))
				{
					apToClear.push_back(pCitizen);
				}
			}
		}
		for (uint i = 0; i < apToClear.size(); ++i)
		{
			if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP plan: Player %d (%S) turn %d %S gives up its specialty: its raw material is nowhere near the colonies",
				getID(), getCivilizationDescription(0), iTurn, apToClear[i]->getName().GetCString());
			apToClear[i]->clearSpecialty();
		}
	}

	// colonies
	std::vector<WfCity> aCities;
	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		WfCity kCity;
		kCity.pCity = pCity;
		kCity.aiRaw.assign(NUM_YIELD_TYPES, 0);
		kCity.aiUse.assign(NUM_YIELD_TYPES, 0);
		kCity.aiStock.assign(NUM_YIELD_TYPES, 0);
		// the food of the colony without its citizens: the center plot and the food modifiers
		int iCitizenFood = 0;
		for (int i = 0; i < pCity->getPopulation(); ++i)
		{
			const CvUnit* const pCitizen = pCity->getPopulationUnitByIndex(i);
			if (pCitizen == NULL || pCitizen->getProfession() == NO_PROFESSION)
			{
				continue;
			}
			// a plot profession without a plot (the plot was lost) produces nothing
			const CvProfessionInfo& kCitizenJob = GC.getProfessionInfo(pCitizen->getProfession());
			const CvPlot* const pWorked = pCity->getPlotWorkedByUnit(pCitizen);
			if (pWorked != NULL || !kCitizenJob.isWorkPlot())
			{
				iCitizenFood += wfFoodOutput(pCitizen->getProfession(), wfOutput(*pCity, pCitizen->getProfession(), pCitizen->getUnitType(), pWorked));
			}
		}
		kCity.iBaseFood = pCity->foodDifference() + iConsumption * pCity->getPopulation() - iCitizenFood;
		kCity.iSurplus = kCity.iBaseFood;
		kCity.iNowSurplus = kCity.iBaseFood;
		kCity.iHeld = 0;
		kCity.iBlockedValue = 0;
		kCity.iBlockedJob = -1;
		// inputs for building jobs: the center plot and a tenth of the stock (the planned plot jobs are added)
		for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_YIELD_TYPES; ++eYield)
		{
			kCity.aiRaw[eYield] = pCity->plot()->getYield(eYield);
		}
		for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
		{
			kCity.aiStock[eYield] = pCity->getYieldStored(eYield) / 10;
			kCity.aiRaw[eYield] += kCity.aiStock[eYield];
		}
		aCities.push_back(kCity);
	}
	const int iNumCities = (int)aCities.size();

	// travel times over land and by ship (a citizen stands for all colonists: same moves and movement costs)
	{
		const CvUnit* pColonist = NULL;
		for (int c = 0; c < iNumCities && pColonist == NULL; ++c)
		{
			if (aCities[c].pCity->getPopulation() > 0)
			{
				pColonist = aCities[c].pCity->getPopulationUnitByIndex(0);
			}
		}
		if (pColonist != NULL)
		{
			AI_wfUpdateTravel(*pColonist);
		}
	}

	// 1. jobs
	std::vector<ProfessionTypes> aePlotProfessions;
	std::vector<ProfessionTypes> aeBuildingProfessions;
	for (ProfessionTypes eProfession = FIRST_PROFESSION; eProfession < NUM_PROFESSION_TYPES; ++eProfession)
	{
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
		if (!kProfession.isCitizen() || !kCiv.isValidProfession(eProfession) || (YieldTypes)kProfession.getYieldsProduced(0) == NO_YIELD)
		{
			continue;
		}
		if (kProfession.isWorkPlot())
		{
			aePlotProfessions.push_back(eProfession);
		}
		else
		{
			aeBuildingProfessions.push_back(eProfession);
		}
	}
	std::vector<WfJob> aJobs;
	// WTP: the slots of a building are shared by all its professions (coffee, cocoa and peanut roasters; blacksmiths and
	// bladesmiths, ...): per (colony, building) a group with the number of slots
	std::vector<int> aiGroupSlots;
	std::map<std::pair<int, int>, int> mapJobGroup;
	for (int iCity = 0; iCity < iNumCities; ++iCity)
	{
		CvCity* const pCity = aCities[iCity].pCity;
		WfJob kJob;
		kJob.pCity = pCity;
		kJob.iCity = iCity;
		kJob.iCurrent = -1;
		kJob.iHolder = -1;
		kJob.iSeated = -1;
		FOREACH(CityPlot)
		{
			if (eLoopCityPlot == CITY_HOME_PLOT)
			{
				continue;
			}
			CvPlot* const pLoopPlot = pCity->getCityIndexPlot(eLoopCityPlot);
			if (pLoopPlot == NULL || (pLoopPlot->getWorkingCity() != pCity && pLoopPlot->getWorkingCity() != NULL)
				|| (!pCity->canWork(pLoopPlot) && pCity->getUnitWorkingPlot(pLoopPlot) == NULL))
			{
				continue;
			}
			// the best food profession, the best other profession (by expert output) and lumber are the alternatives of the
			// plot; the profession a citizen works there now is one as well
			ProfessionTypes eBestFood = NO_PROFESSION;
			ProfessionTypes eBestOther = NO_PROFESSION;
			ProfessionTypes eBestLumber = NO_PROFESSION;
			int iBestFood = 0;
			int iBestOther = 0;
			int iBestLumber = 0;
			for (uint i = 0; i < aePlotProfessions.size(); ++i)
			{
				const ProfessionTypes eProfession = aePlotProfessions[i];
				const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
				const YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
				const UnitClassTypes eExpert = (UnitClassTypes)kProfession.LbD_getExpert();
				const UnitTypes eExpertUnit = (eExpert != NO_UNITCLASS) ? kCiv.getCivilizationUnits(eExpert) : NO_UNIT;
				const int iOutput = wfOutput(*pCity, eProfession, eExpertUnit != NO_UNIT ? eExpertUnit : eColonistUnit, pLoopPlot);
				if (iOutput <= 0)
				{
					continue;
				}
				if (eYield == YIELD_FOOD)
				{
					if (iOutput > iBestFood)
					{
						iBestFood = iOutput;
						eBestFood = eProfession;
					}
				}
				else if (eYield == YIELD_LUMBER)
				{
					// lumber is always an alternative: the carpenters need it (its value depends on them)
					if (iOutput > iBestLumber)
					{
						iBestLumber = iOutput;
						eBestLumber = eProfession;
					}
				}
				else
				{
					// sale value plus the food of a second yield (hunters, fruit pickers, rice farmers)
					const int iValue = wfYieldGold(*pCity, eYield, iOutput, 0) + wfFoodOutput(eProfession, iOutput) * WF_FOOD_GOLD;
					if (iValue > iBestOther)
					{
						iBestOther = iValue;
						eBestOther = eProfession;
					}
				}
			}
			const CvUnit* const pWorker = pCity->getUnitWorkingPlot(pLoopPlot);
			const ProfessionTypes eWorked = (pWorker != NULL) ? pWorker->getProfession() : NO_PROFESSION;
			ProfessionTypes aeAlternatives[4] = { eBestFood, eBestOther, eBestLumber,
				(eWorked != eBestFood && eWorked != eBestOther && eWorked != eBestLumber) ? eWorked : NO_PROFESSION };
			for (int iAlt = 0; iAlt < 4; ++iAlt)
			{
				const ProfessionTypes eProfession = aeAlternatives[iAlt];
				if (eProfession == NO_PROFESSION || !GC.getProfessionInfo(eProfession).isWorkPlot())
				{
					continue;
				}
				const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
				kJob.eProfession = eProfession;
				kJob.pPlot = pLoopPlot;
				kJob.iPlotKey = GC.getMap().plotNumINLINE(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
				kJob.eExpertClass = (UnitClassTypes)kProfession.LbD_getExpert();
				kJob.eYield = (YieldTypes)kProfession.getYieldsProduced(0);
				kJob.bFood = kJob.eYield == YIELD_FOOD;
				const BonusTypes eBonus = pLoopPlot->getBonusType();
				kJob.bNotable = (eBonus != NO_BONUS && GC.getBonusInfo(eBonus).getYieldChange(kJob.eYield) > 0) || pLoopPlot->getImprovementType() != NO_IMPROVEMENT;
				kJob.iSlot = 0;
				kJob.iGroup = -1;
				aJobs.push_back(kJob);
			}
		}
		for (uint i = 0; i < aeBuildingProfessions.size(); ++i)
		{
			const ProfessionTypes eProfession = aeBuildingProfessions[i];
			const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
			const int iSlots = pCity->getNumProfessionBuildingSlots(eProfession);
			kJob.eProfession = eProfession;
			kJob.pPlot = NULL;
			kJob.iPlotKey = -1;
			kJob.eExpertClass = (UnitClassTypes)kProfession.LbD_getExpert();
			kJob.eYield = (YieldTypes)kProfession.getYieldsProduced(0);
			kJob.bFood = kJob.eYield == YIELD_FOOD;
			kJob.bNotable = true;
			const std::pair<int, int> kGroupKey(iCity, kProfession.getSpecialBuilding());
			std::map<std::pair<int, int>, int>::const_iterator itGroup = mapJobGroup.find(kGroupKey);
			if (itGroup == mapJobGroup.end())
			{
				mapJobGroup[kGroupKey] = (int)aiGroupSlots.size();
				aiGroupSlots.push_back(iSlots);
			}
			kJob.iGroup = mapJobGroup[kGroupKey];
			for (int iSlot = 0; iSlot < iSlots; ++iSlot)
			{
				kJob.iSlot = iSlot;
				aJobs.push_back(kJob);
			}
		}
	}
	const int iNumJobs = (int)aJobs.size();

	// 2. workforce
	std::vector<WfWorker> aWorkers;
	std::vector<UnitTypes> aeTypes; // unit types of the output table
	aeTypes.push_back(eColonistUnit);
	{
		std::vector<CvUnit*> apUnits;
		for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
		{
			for (int i = 0; i < pCity->getPopulation(); ++i)
			{
				CvUnit* const pCitizen = pCity->getPopulationUnitByIndex(i);
				if (pCitizen != NULL)
				{
					apUnits.push_back(pCitizen);
				}
			}
		}
		for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
		{
			const CvUnitAI& kLoopAI = static_cast<const CvUnitAI&>(*pLoopUnit);
			const bool bTask = kLoopAI.AI_getWfTask() != CvUnitAI::WF_NONE;
			if (pLoopUnit->getDomainType() != DOMAIN_LAND || pLoopUnit->isGoods() || pLoopUnit->isExpertNativeTrader()
				|| pLoopUnit->AI_getUnitAIState() == UNITAI_STATE_EXPLORER_PASSENGER
				|| (pLoopUnit->AI_getUnitAIType() != UNITAI_COLONIST && !(bTask && pLoopUnit->getUnitTravelState() == UNIT_TRAVEL_STATE_LIVE_AMONG_NATIVES)))
			{
				if (bTask && pLoopUnit->AI_getUnitAIType() != UNITAI_COLONIST)
				{
					// became a soldier, pioneer, settler, ...
					if (gPlayerLogLevel >= 1)
					{
						const CvCity* const pTaskCity = getCity(kLoopAI.AI_getWfCityID());
						logWTP(WTP_LOG_PLAN, getID(), "WTP plan: Player %d (%S) turn %d %S loses its task %d (%S in %S): it is no colonist any more (unit AI %d, profession %S)",
							getID(), getCivilizationDescription(0), iTurn, pLoopUnit->getName().GetCString(), kLoopAI.AI_getWfTask(),
							kLoopAI.AI_getWfProfession() != NO_PROFESSION ? GC.getProfessionInfo(kLoopAI.AI_getWfProfession()).getDescription() : L"-",
							pTaskCity != NULL ? pTaskCity->getName().GetCString() : L"-", (int)pLoopUnit->AI_getUnitAIType(),
							pLoopUnit->getProfession() != NO_PROFESSION ? GC.getProfessionInfo(pLoopUnit->getProfession()).getDescription() : L"-");
					}
					static_cast<CvUnitAI*>(pLoopUnit)->AI_clearWfTask();
				}
				continue;
			}
			apUnits.push_back(pLoopUnit);
		}
		// the docks in Europe (bought and hurried colonists wait there for a ship)
		for (int i = 0; i < getNumEuropeUnits(); ++i)
		{
			CvUnit* const pUnit = getEuropeUnit(i);
			if (pUnit != NULL && pUnit->AI_getUnitAIType() == UNITAI_COLONIST && pUnit->getDomainType() == DOMAIN_LAND && !pUnit->isExpertNativeTrader())
			{
				apUnits.push_back(pUnit);
			}
			else if (pUnit != NULL && static_cast<const CvUnitAI*>(pUnit)->AI_getWfTask() != CvUnitAI::WF_NONE)
			{
				if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP plan: Player %d (%S) turn %d %S on the docks loses its task %d: it is no colonist any more (unit AI %d)",
					getID(), getCivilizationDescription(0), iTurn, pUnit->getName().GetCString(), static_cast<const CvUnitAI*>(pUnit)->AI_getWfTask(), (int)pUnit->AI_getUnitAIType());
				static_cast<CvUnitAI*>(pUnit)->AI_clearWfTask();
			}
		}
		// the docks in Africa (slaves the plan bought wait there for a ship)
		for (int i = 0; i < getNumAfricaUnits(); ++i)
		{
			CvUnit* const pUnit = getAfricaUnit(i);
			if (pUnit != NULL && pUnit->AI_getUnitAIType() == UNITAI_COLONIST && pUnit->getDomainType() == DOMAIN_LAND)
			{
				apUnits.push_back(pUnit);
			}
		}
		for (uint i = 0; i < apUnits.size(); ++i)
		{
			CvUnitAI& kUnit = static_cast<CvUnitAI&>(*apUnits[i]);
			WfWorker kWorker;
			kWorker.pUnit = &kUnit;
			kWorker.pPopCity = getPopulationUnitCity(kUnit.getID());
			kWorker.pAtCity = NULL;
			if (kWorker.pPopCity == NULL && kUnit.isOnMap() && !kUnit.isCargo() && kUnit.plot() != NULL)
			{
				CvCity* const pHere = kUnit.plot()->getPlotCity();
				if (pHere != NULL && pHere->getOwnerINLINE() == getID())
				{
					kWorker.pAtCity = pHere;
				}
			}
			kWorker.iCurrentJob = -1;
			kWorker.iJob = -1;
			// an expert has an ideal profession; expert missionaries (teach level 3 with a cross bonus, see wfIsExpertFor)
			// have none among the citizen professions but are experts for the church jobs
			kWorker.bExpert = wfIsExpert(kUnit);
			const int iTask = kUnit.AI_getWfTask();
			kWorker.bLearning = kUnit.getUnitTravelState() == UNIT_TRAVEL_STATE_LIVE_AMONG_NATIVES
				|| iTask == CvUnitAI::WF_LEARN || iTask == CvUnitAI::WF_RETRAIN || iTask == CvUnitAI::WF_FOOD_LEARN;
			kWorker.bReserved = false;
			kWorker.iSeatCity = -1;
			const UnitTypes eType = kUnit.getUnitType();
			kWorker.iTypeIdx = -1;
			for (uint t = 0; t < aeTypes.size(); ++t)
			{
				if (aeTypes[t] == eType)
				{
					kWorker.iTypeIdx = t;
				}
			}
			if (kWorker.iTypeIdx < 0)
			{
				kWorker.iTypeIdx = (int)aeTypes.size();
				aeTypes.push_back(eType);
			}
			aWorkers.push_back(kWorker);
		}
	}
	const int iNumWorkers = (int)aWorkers.size();

	// output table: gold x100 of the non-food output (inputs deducted) and food per (unit type, job)
	const int iNumTypes = (int)aeTypes.size();
	// an expert never studies: its student jobs (school) use an extra row in which no job is possible
	const int iNoStudentType = iNumTypes;
	std::vector<int> aiGold((iNumTypes + 1) * iNumJobs, 0);
	std::vector<int> aiFood((iNumTypes + 1) * iNumJobs, 0);
	std::vector<int> aiOutput((iNumTypes + 1) * iNumJobs, 0);
	for (int j = 0; j < iNumJobs; ++j)
	{
		aiGold[iNoStudentType * iNumJobs + j] = -1000000;
	}
	// colonists that never work in buildings (indentured servants, mulattoes, mestizos, slaves, converted natives); failed
	// traders only as carpenters, disillusioned missionaries only in the church (all of them may work plots)
	const UnitClassTypes aeFieldOnly[6] = { (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_INDENTURED_SERVANT"),
		(UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_MULATTO"), (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_MESTIZZO"),
		(UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_AFRICAN_SLAVE"), (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_NATIVE_SLAVE"),
		(UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_CONVERTED_NATIVE") };
	const UnitClassTypes eFailedTrader = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FAILED_TRADER");
	const UnitClassTypes eFailedMissionary = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FAILED_MISSIONARY");
	for (int t = 0; t < iNumTypes; ++t)
	{
		const UnitClassTypes eTypeClass = (UnitClassTypes)GC.getUnitInfo(aeTypes[t]).getUnitClassType();
		bool bFieldOnly = false;
		for (int k = 0; k < 6; ++k)
		{
			bFieldOnly = bFieldOnly || (eTypeClass != NO_UNITCLASS && eTypeClass == aeFieldOnly[k]);
		}
		for (int j = 0; j < iNumJobs; ++j)
		{
			const WfJob& kJob = aJobs[j];
			const bool bBuilding = kJob.pPlot == NULL;
			if (!isProfessionValid(kJob.eProfession, aeTypes[t])
				|| (bBuilding && (bFieldOnly || (eTypeClass == eFailedTrader && kJob.eYield != YIELD_HAMMERS)
					|| (eTypeClass == eFailedMissionary && kJob.eYield != YIELD_CROSSES))))
			{
				aiGold[t * iNumJobs + j] = -1000000;
				continue;
			}
			const int iOutput = wfOutput(*kJob.pCity, kJob.eProfession, aeTypes[t], kJob.pPlot);
			aiOutput[t * iNumJobs + j] = iOutput;
			if (iOutput <= 0)
			{
				aiGold[t * iNumJobs + j] = -1000000;
				continue;
			}
			aiFood[t * iNumJobs + j] = wfFoodOutput(kJob.eProfession, iOutput);
			if (kJob.bFood)
			{
				continue;
			}
			// the plan counts gold x100 (AI_estimateYieldValue is in gold)
			int iGold = wfYieldGold(*kJob.pCity, kJob.eYield, iOutput, kJob.iSlot);
			const CvProfessionInfo& kProfession = GC.getProfessionInfo(kJob.eProfession);
			for (int k = 0; k < kProfession.getNumYieldsConsumed(); ++k)
			{
				const YieldTypes eInput = (YieldTypes)kProfession.getYieldsConsumed(k);
				if (eInput != NO_YIELD)
				{
					iGold -= wfInputGold(*kJob.pCity, kJob.eYield, eInput, iOutput);
				}
			}
			aiGold[t * iNumJobs + j] = iGold;
		}
	}

	// the unit type that does the job: an expert outside its trade works like a free colonist (side bonuses ignored), it never studies
	#define WF_TYPE(w, j) ((aWorkers[w].bExpert && aJobs[j].eYield == YIELD_EDUCATION) ? iNoStudentType : (aWorkers[w].bExpert && !wfIsExpertFor(*aWorkers[w].pUnit, aJobs[j].eProfession)) ? 0 : aWorkers[w].iTypeIdx)

	// the jobs worked now and the tasks held
	std::vector<int> aiPlotTaken(GC.getMap().numPlotsINLINE(), -1); // plot number -> job holding it in the target plan
	std::vector<int> aiGroupHeld(aiGroupSlots.size(), 0); // building group -> jobs of it held in the target plan
	#define WF_GROUP_FREE(j) (aJobs[j].iGroup < 0 || aiGroupHeld[aJobs[j].iGroup] < aiGroupSlots[aJobs[j].iGroup])
	for (int w = 0; w < iNumWorkers; ++w)
	{
		WfWorker& kWorker = aWorkers[w];
		const CvUnitAI& kUnit = *kWorker.pUnit;
		int iCityID = -1;
		ProfessionTypes eProfession = NO_PROFESSION;
		int iX = -1;
		int iY = -1;
		if (kWorker.pPopCity != NULL && kUnit.getProfession() != NO_PROFESSION)
		{
			iCityID = kWorker.pPopCity->getID();
			eProfession = kUnit.getProfession();
			const CvPlot* const pWorked = kWorker.pPopCity->getPlotWorkedByUnit(&kUnit);
			if (pWorked != NULL)
			{
				iX = pWorked->getX_INLINE();
				iY = pWorked->getY_INLINE();
			}
		}
		else if (kUnit.AI_getWfTask() != CvUnitAI::WF_NONE && kUnit.AI_getWfTask() != CvUnitAI::WF_WAIT && kUnit.AI_getWfTask() != CvUnitAI::WF_JOIN_CITY)
		{
			iCityID = kUnit.AI_getWfCityID();
			eProfession = kUnit.AI_getWfProfession();
			iX = kUnit.AI_getWfPlotX();
			iY = kUnit.AI_getWfPlotY();
		}
		if (eProfession == NO_PROFESSION)
		{
			continue;
		}
		// an expert or learner that holds the job as its task (its reservation) takes it over from a regular colonist
		// working it meanwhile; the regular one may go on working it until the expert arrives
		// (a free slot of a building job first)
		const bool bClaims = kWorker.bLearning || wfIsExpertFor(kUnit, eProfession);
		for (int iPass = 0; iPass < 2 && kWorker.iCurrentJob < 0; ++iPass)
		{
			for (int j = 0; j < iNumJobs; ++j)
			{
				if (!wfJobMatches(aJobs[j], iCityID, eProfession, iX, iY))
				{
					continue;
				}
				const int iOld = aJobs[j].iCurrent;
				if (iOld < 0 || (iPass == 1 && bClaims && !aWorkers[iOld].bLearning && !wfIsExpertFor(*aWorkers[iOld].pUnit, eProfession)))
				{
					if (iOld >= 0)
					{
						aWorkers[iOld].iCurrentJob = -1;
					}
					aJobs[j].iCurrent = w;
					kWorker.iCurrentJob = j;
					break;
				}
			}
		}
	}

	// assignment to the target plan
	int iAssignWorker = -1;
	int iAssignJob = -1;
	#define WF_ASSIGN(w, j) \
		iAssignWorker = (w); iAssignJob = (j); \
		aWorkers[iAssignWorker].iJob = iAssignJob; \
		aJobs[iAssignJob].iHolder = iAssignWorker; \
		if (aJobs[iAssignJob].iPlotKey >= 0) aiPlotTaken[aJobs[iAssignJob].iPlotKey] = iAssignJob; 		if (aJobs[iAssignJob].iGroup >= 0) ++aiGroupHeld[aJobs[iAssignJob].iGroup]; \
		{ \
			WfCity& kAssignCity = aCities[aJobs[iAssignJob].iCity]; \
			const int iT = WF_TYPE(iAssignWorker, iAssignJob); \
			kAssignCity.iSurplus += aiFood[iT * iNumJobs + iAssignJob] - iConsumption; \
			++kAssignCity.iHeld; \
			const CvProfessionInfo& kAssignProf = GC.getProfessionInfo(aJobs[iAssignJob].eProfession); \
			if (kAssignProf.isWorkPlot()) kAssignCity.aiRaw[aJobs[iAssignJob].eYield] += aiOutput[iT * iNumJobs + iAssignJob]; \
			for (int k = 0; k < kAssignProf.getNumYieldsConsumed(); ++k) \
			{ \
				const YieldTypes eIn = (YieldTypes)kAssignProf.getYieldsConsumed(k); \
				if (eIn != NO_YIELD) kAssignCity.aiUse[eIn] += aiOutput[iT * iNumJobs + iAssignJob]; \
			} \
		}

	// travel time in turns of each worker to each colony: over land or by ship (the travel times of the plan); 1000 if there
	// is no way
	std::vector<int> aiEta(iNumWorkers * std::max(1, iNumCities), 0);
	for (int w = 0; w < iNumWorkers; ++w)
	{
		const WfWorker& kWorker = aWorkers[w];
		const CvUnit& kUnit = *kWorker.pUnit;
		const CvPlot* const pFrom = (kWorker.pPopCity != NULL) ? kWorker.pPopCity->plot() : (kUnit.isOnMap() ? kUnit.plot() : NULL);
		for (int c = 0; c < iNumCities; ++c)
		{
			const CvCity* const pCity = aCities[c].pCity;
			int iEta;
			if (kWorker.pPopCity == pCity || kWorker.pAtCity == pCity)
			{
				iEta = 0;
			}
			else if (pFrom == NULL)
			{
				iEta = 9; // in Europe, on the ocean or living among the natives (user estimate)
			}
			else if (kUnit.isCargo() || m_mapWfLandCost.empty())
			{
				// on a ship (it sails where its passengers go) or no travel times yet
				const int iDistance = plotDistance(pFrom->getX_INLINE(), pFrom->getY_INLINE(), pCity->getX_INLINE(), pCity->getY_INLINE());
				iEta = (kUnit.isCargo() || pFrom->area() != pCity->area()) ? 6 + iDistance / 4 : 1 + iDistance / 2;
			}
			else
			{
				const int iTurns = AI_wfTravelTurns(*pFrom, *pCity);
				iEta = (iTurns == MAX_INT) ? 1000 : std::max(1, iTurns);
			}
			// the colonist found no way there lately (no path over land, no ship): not planned there
			if (iEta > 0 && AI_isWfUnreachable(kUnit.getID(), pCity->getID()))
			{
				iEta = 1000;
			}
			aiEta[w * iNumCities + c] = iEta;
		}
	}
	// the colony each worker lives in, waits in or is on its way to for a job (-1: none, e.g. idle or in Europe)
	std::vector<int> aiHomeCity(iNumWorkers, -1);
	for (int w = 0; w < iNumWorkers; ++w)
	{
		const WfWorker& kWorker = aWorkers[w];
		const CvUnitAI& kUnit = *kWorker.pUnit;
		const CvCity* pHome = (kWorker.pPopCity != NULL) ? kWorker.pPopCity : kWorker.pAtCity;
		if (pHome == NULL && kUnit.AI_getWfTask() == CvUnitAI::WF_EXPERT_JOB)
		{
			pHome = getCity(kUnit.AI_getWfCityID());
		}
		for (int c = 0; c < iNumCities && pHome != NULL; ++c)
		{
			if (aCities[c].pCity == pHome)
			{
				aiHomeCity[w] = c;
				break;
			}
		}
	}

	// fixed jobs: learners keep the job they learn for; an expert keeps its expert job while it is on its way there (bought
	// or planned: the travel time counted when it got the job) and for good on a resource of its trade. An expert working
	// in its colony otherwise keeps its job only by the bonus for the job it holds (it moves for a clearly better one)
	for (int w = 0; w < iNumWorkers; ++w)
	{
		WfWorker& kWorker = aWorkers[w];
		const int j = kWorker.iCurrentJob;
		if (j < 0 || aJobs[j].iHolder >= 0 || (aJobs[j].iPlotKey >= 0 && aiPlotTaken[aJobs[j].iPlotKey] >= 0) || !WF_GROUP_FREE(j))
		{
			continue;
		}
		const WfJob& kJob = aJobs[j];
		const bool bExpertJob = wfIsExpertFor(*kWorker.pUnit, kJob.eProfession);
		const bool bThere = kWorker.pPopCity == kJob.pCity || kWorker.pAtCity == kJob.pCity;
		const BonusTypes eBonus = (kJob.pPlot != NULL) ? kJob.pPlot->getBonusType() : NO_BONUS;
		const bool bResource = eBonus != NO_BONUS && GC.getBonusInfo(eBonus).getYieldChange(kJob.eYield) > 0;
		// (not an expert that has no way to the colony)
		if (kWorker.bLearning || (bExpertJob && (!bThere || bResource) && aiEta[w * iNumCities + kJob.iCity] < 1000))
		{
			// an expert on its way changes to another job of its trade that is worth 1.5 times as much and not farther away (a
			// Master Carpenter waited in La Isabela for a ship to a 22 gold carpenter job while La Isabela's carpenter jobs
			// were worth 92 gold); not from a resource of its trade
			int jUse = j;
			if (!kWorker.bLearning && !bThere && !bResource)
			{
				const int iT = kWorker.iTypeIdx;
				const int iEtaNow = aiEta[w * iNumCities + kJob.iCity];
				const int iValueNow = aiGold[iT * iNumJobs + j] + (aiFood[iT * iNumJobs + j] - iConsumption) * WF_FOOD_GOLD;
				int iBestAlt = iValueNow * 3 / 2;
				for (int jj = 0; jj < iNumJobs; ++jj)
				{
					const WfJob& kAlt = aJobs[jj];
					const int iEtaAlt = aiEta[w * iNumCities + kAlt.iCity];
					if (jj == j || kAlt.iHolder >= 0 || (kAlt.iPlotKey >= 0 && aiPlotTaken[kAlt.iPlotKey] >= 0) || !WF_GROUP_FREE(jj) || iEtaAlt > iEtaNow
						|| aiGold[iT * iNumJobs + jj] <= -1000000 || !wfIsExpertFor(*kWorker.pUnit, kAlt.eProfession))
					{
						continue;
					}
					const int iValueAlt = aiGold[iT * iNumJobs + jj] + (aiFood[iT * iNumJobs + jj] - iConsumption) * WF_FOOD_GOLD;
					if (iValueAlt >= iBestAlt && iValueAlt > 0)
					{
						iBestAlt = iValueAlt;
						jUse = jj;
					}
				}
				if (jUse != j && gPlayerLogLevel >= 1)
				{
					logWTP(WTP_LOG_DIAG, getID(), "diag %S changes its reserved job %s in %S (%d gold, %d turns) for %s in %S (%d gold, %d turns)",
						kWorker.pUnit->getName().GetCString(), CvString(GC.getProfessionInfo(kJob.eProfession).getType()).GetCString(), kJob.pCity->getName().GetCString(),
						iValueNow / 100, iEtaNow, CvString(GC.getProfessionInfo(aJobs[jUse].eProfession).getType()).GetCString(), aJobs[jUse].pCity->getName().GetCString(),
						iBestAlt / 100, aiEta[w * iNumCities + aJobs[jUse].iCity]);
				}
			}
			const WfJob& kUse = aJobs[jUse];
			// an expert on its way to the first carpenter job reserves another carpenter job of the colony if there is one:
			// the first one is worked by someone in the colony until it has arrived
			int jReserve = jUse;
			const bool bThereUse = kWorker.pPopCity == kUse.pCity || kWorker.pAtCity == kUse.pCity;
			if (!bThereUse && kUse.eYield == YIELD_HAMMERS && kUse.iSlot == 0)
			{
				for (int jj = 0; jj < iNumJobs; ++jj)
				{
					if (aJobs[jj].iCity == kUse.iCity && aJobs[jj].eProfession == kUse.eProfession && aJobs[jj].iSlot > 0 && aJobs[jj].iHolder < 0)
					{
						jReserve = jj;
						break;
					}
				}
			}
			kWorker.bReserved = !kWorker.bLearning;
			WF_ASSIGN(w, jReserve);
		}
	}

	// value of a worker in a job at the planned surplus of its colony; MIN_INT if the job is not possible
	#define WF_IS_FREE_JOB(j) (aJobs[j].iHolder < 0 && (aJobs[j].iPlotKey < 0 || aiPlotTaken[aJobs[j].iPlotKey] < 0) && WF_GROUP_FREE(j))

	// colonies with a carpenter job (their first carpenter needs lumber before it is planned)
	std::vector<bool> abHammerJob(iNumCities, false);
	for (int j = 0; j < iNumJobs; ++j)
	{
		if (aJobs[j].eYield == YIELD_HAMMERS)
		{
			abHammerJob[aJobs[j].iCity] = true;
		}
	}

	// 3. target plan: greedy, the best (worker, job) pair (or pair of a job and a food job, or of a carpenter and a lumberjack)
	// first. The first carpenter of a colony (with its lumber) comes before all other jobs (a colony without hammers builds
	// nothing), and expert carpenters, statesmen and preachers always get a job of their trade.
	const int WF_PRIORITY = 10000000;
	for (int iRound = 0; iRound < iNumWorkers * 2 + 10; ++iRound)
	{
		for (int c = 0; c < iNumCities; ++c)
		{
			aCities[c].iBlockedValue = 0;
			aCities[c].iBlockedJob = -1;
		}
		WfCand kBest = { -1, -1, 0, 0 };
		// per colony: the two best food jobs and the two best lumber jobs (distinct workers), the three best jobs that need
		// more food and the two best carpenter jobs that need lumber
		std::vector<WfCand> aFood1(iNumCities, kBest);
		std::vector<WfCand> aFood2(iNumCities, kBest);
		std::vector<WfCand> aLumber1(iNumCities, kBest);
		std::vector<WfCand> aLumber2(iNumCities, kBest);
		std::vector<WfCand> aShort(iNumCities * 3, kBest);
		std::vector<WfCand> aNoLumber(iNumCities * 2, kBest);
		for (int w = 0; w < iNumWorkers; ++w)
		{
			const WfWorker& kWorker = aWorkers[w];
			if (kWorker.iJob >= 0 || kWorker.bLearning)
			{
				continue;
			}
			for (int j = 0; j < iNumJobs; ++j)
			{
				if (!WF_IS_FREE_JOB(j))
				{
					continue;
				}
				const WfJob& kJob = aJobs[j];
				const int iT = WF_TYPE(w, j);
				const int iGold = aiGold[iT * iNumJobs + j];
				if (iGold <= -1000000 || aiEta[w * iNumCities + kJob.iCity] >= 1000)
				{
					continue;
				}
				// a regular colonist takes only jobs it reaches within 12 turns (one 35 turns away held a coal mine, and the
				// colonist standing in for it left the lumber the carpenters needed); experts go to a job of their trade anywhere
				if (aiEta[w * iNumCities + kJob.iCity] > 12 && !wfIsExpertFor(*kWorker.pUnit, kJob.eProfession))
				{
					continue;
				}
				WfCity& kCity = aCities[kJob.iCity];
				const int iOutput = aiOutput[iT * iNumJobs + j];
				const bool bCarpenter = kJob.eYield == YIELD_HAMMERS;
				// a building job needs its input (stock, center plot or planned production); a carpenter without lumber may
				// still be planned together with a lumberjack
				const CvProfessionInfo& kProfession = GC.getProfessionInfo(kJob.eProfession);
				bool bInput = true;
				for (int k = 0; k < kProfession.getNumYieldsConsumed() && bInput; ++k)
				{
					const YieldTypes eIn = (YieldTypes)kProfession.getYieldsConsumed(k);
					bInput = eIn == NO_YIELD || kCity.aiRaw[eIn] - kCity.aiUse[eIn] >= std::max(1, iOutput / 2);
				}
				if (!bInput && !bCarpenter)
				{
					continue;
				}
				const int iFoodDelta = aiFood[iT * iNumJobs + j] - iConsumption;
				int iJobGold = iGold;
				// lumber the carpenters need (planned ones, or the first one still to come) is worth WF_LUMBER_GOLD
				if (kJob.eYield == YIELD_LUMBER && kJob.pPlot != NULL)
				{
					const int iUse = kCity.aiUse[YIELD_LUMBER];
					const int iNeed = iUse + ((iUse == 0 && abHammerJob[kJob.iCity]) ? 3 : 0) - kCity.aiRaw[YIELD_LUMBER];
					if (iNeed > 0 && iOutput > 0)
					{
						const int iForCarpenters = std::min(iNeed, iOutput);
						iJobGold = std::max(iGold, iGold * (iOutput - iForCarpenters) / iOutput + iForCarpenters * WF_LUMBER_GOLD);
					}
				}
				// the first carpenter of a colony is a must; expert carpenters, statesmen and preachers always work in their
				// trade (other experts get their job by value like everybody else)
				const bool bExpertBuilding = !kProfession.isWorkPlot() && wfIsExpertFor(*kWorker.pUnit, kJob.eProfession)
					&& (kJob.eYield == YIELD_HAMMERS || kJob.eYield == YIELD_BELLS || kJob.eYield == YIELD_CROSSES);
				// the first carpenter job is a must for colonists in the colony (someone works as carpenter now, an expert on
				// its way takes another carpenter job and the first one when it has arrived); not for an expert of another trade
				// (a prospector, a fisherman): it gets 30 gold more there and takes the job unless it earns clearly more elsewhere
				const bool bFirstCarpenterHere = bCarpenter && kJob.iSlot == 0 && aiEta[w * iNumCities + kJob.iCity] == 0;
				const bool bOtherTradeExpert = kWorker.bExpert && !bExpertBuilding;
				// expert farmers always farm (a land plot with 4 food or more as a free colonist works it), expert fishermen
				// always fish (a water plot with food)
				const bool bExpertFood = kJob.bFood && kJob.pPlot != NULL && kProfession.isWorkPlot() && wfIsExpertFor(*kWorker.pUnit, kJob.eProfession)
					&& (kJob.pPlot->isWater() ? aiFood[j] > 0 : aiFood[j] >= 4);
				const bool bMust = (bFirstCarpenterHere && !bOtherTradeExpert) || bExpertBuilding || bExpertFood;
				int iValue = iJobGold + wfFoodGold(kCity.iSurplus, iFoodDelta);
				if (bFirstCarpenterHere && bOtherTradeExpert)
				{
					iValue += 3000;
				}
				if (iValue <= 0 && !bMust)
				{
					continue;
				}
				iValue = std::max(1, iValue) * 20 / (20 + aiEta[w * iNumCities + kJob.iCity]);
				// a colonist in a colony or on its way to one goes to another colony only for a job worth half as much again
				// (no going back and forth between two colonies for jobs of about the same value)
				if (aiHomeCity[w] >= 0 && aiHomeCity[w] != kJob.iCity)
				{
					iValue = iValue * 2 / 3;
				}
				// the job held already x1.2 (no moving for a slightly better job); experts are fixed only on a resource of their
				// trade, while learning or on their way
				if (kWorker.iCurrentJob == j)
				{
					iValue = iValue * 6 / 5;
				}
				if (bFirstCarpenterHere && !bOtherTradeExpert)
				{
					iValue += WF_PRIORITY;
				}
				if (bExpertBuilding || bExpertFood)
				{
					iValue += WF_PRIORITY / 2;
				}
				WfCand kCand = { w, j, iValue, iFoodDelta };
				// a lumber job is a partner for a carpenter without lumber
				if (kJob.eYield == YIELD_LUMBER && kJob.pPlot != NULL && iOutput > 0)
				{
					WfCand& kL1 = aLumber1[kJob.iCity];
					WfCand& kL2 = aLumber2[kJob.iCity];
					if (iValue > kL1.iValue)
					{
						if (kL1.iWorker != w)
						{
							kL2 = kL1;
						}
						kL1 = kCand;
					}
					else if (iValue > kL2.iValue && kL1.iWorker != w)
					{
						kL2 = kCand;
					}
				}
				if (!bInput)
				{
					// a carpenter without lumber: only together with a lumberjack
					for (int s = 0; s < 2; ++s)
					{
						WfCand& kNo = aNoLumber[kJob.iCity * 2 + s];
						if (iValue > kNo.iValue)
						{
							if (s == 0)
							{
								aNoLumber[kJob.iCity * 2 + 1] = kNo;
							}
							kNo = kCand;
							break;
						}
					}
					continue;
				}
				// a job that raises the surplus is always possible (a colony below the green zone gets there with food jobs)
				// (a must job, the first carpenter or an expert carpenter, statesman or preacher, may go below it: the food
				// jobs planned afterwards bring the colony back)
				if (kCity.iSurplus + iFoodDelta >= WF_FOOD_MIN || iFoodDelta > 0 || bMust)
				{
					if (iValue > kBest.iValue)
					{
						kBest = kCand;
					}
					// every job that raises the surplus (food jobs, hunters, fruit pickers, ...) is a food partner
					if (iFoodDelta > 0)
					{
						WfCand& kF1 = aFood1[kJob.iCity];
						WfCand& kF2 = aFood2[kJob.iCity];
						if (iValue > kF1.iValue)
						{
							if (kF1.iWorker != w)
							{
								kF2 = kF1;
							}
							kF1 = kCand;
						}
						else if (iValue > kF2.iValue && kF1.iWorker != w)
						{
							kF2 = kCand;
						}
					}
				}
				else
				{
					// too little food: a candidate for a pair with a food job; also the blocked value of the colony
					if (iValue > kCity.iBlockedValue)
					{
						kCity.iBlockedValue = iValue;
						kCity.iBlockedJob = j;
					}
					for (int s = 0; s < 3; ++s)
					{
						WfCand& kShort = aShort[kJob.iCity * 3 + s];
						if (iValue > kShort.iValue)
						{
							for (int s2 = 2; s2 > s; --s2)
							{
								aShort[kJob.iCity * 3 + s2] = aShort[kJob.iCity * 3 + s2 - 1];
							}
							kShort = kCand;
							break;
						}
					}
				}
			}
		}
		// pairs: a job short of food together with a food job, or a carpenter without lumber together with a lumberjack, of
		// the same colony
		WfCand kPairFood = { -1, -1, 0, 0 };
		int iBestPairValue = 0;
		WfCand kPairJob = kPairFood;
		for (int c = 0; c < iNumCities; ++c)
		{
			for (int s = 0; s < 3; ++s)
			{
				const WfCand& kShort = aShort[c * 3 + s];
				if (kShort.iWorker < 0)
				{
					continue;
				}
				const WfCand* apFood[2] = { &aFood1[c], &aFood2[c] };
				for (int f = 0; f < 2; ++f)
				{
					const WfCand& kFood = *apFood[f];
					if (kFood.iWorker < 0 || kFood.iWorker == kShort.iWorker
						|| (aJobs[kFood.iJob].iPlotKey >= 0 && aJobs[kFood.iJob].iPlotKey == aJobs[kShort.iJob].iPlotKey)
						|| aCities[c].iSurplus + kShort.iFoodDelta + kFood.iFoodDelta < WF_FOOD_MIN)
					{
						continue;
					}
					const int iPairValue = (kShort.iValue + kFood.iValue) / 2;
					if (iPairValue > iBestPairValue)
					{
						iBestPairValue = iPairValue;
						kPairJob = kShort;
						kPairFood = kFood;
					}
				}
			}
			for (int s = 0; s < 2; ++s)
			{
				const WfCand& kCarpenter = aNoLumber[c * 2 + s];
				if (kCarpenter.iWorker < 0)
				{
					continue;
				}
				const int iCarpenterOutput = aiOutput[WF_TYPE(kCarpenter.iWorker, kCarpenter.iJob) * iNumJobs + kCarpenter.iJob];
				// WTP: the lumberjack covers the new carpenter's need only, not a deficit the colony has already (one regular
				// colonist had to bring 7 lumber in La Isabela, so no third carpenter was planned with 10 colonists idle); if
				// one lumberjack is not enough, the two best together do (the second one follows by its lumber value)
				const int iMissing = std::max(1, iCarpenterOutput / 2) - std::max(0, aCities[c].aiRaw[YIELD_LUMBER] - aCities[c].aiUse[YIELD_LUMBER]);
				const WfCand* apLumber[2] = { &aLumber1[c], &aLumber2[c] };
				const int iBothLumber = (aLumber1[c].iWorker >= 0 && aLumber2[c].iWorker >= 0 && aLumber1[c].iWorker != kCarpenter.iWorker && aLumber2[c].iWorker != kCarpenter.iWorker)
					? aiOutput[WF_TYPE(aLumber1[c].iWorker, aLumber1[c].iJob) * iNumJobs + aLumber1[c].iJob] + aiOutput[WF_TYPE(aLumber2[c].iWorker, aLumber2[c].iJob) * iNumJobs + aLumber2[c].iJob] : 0;
				for (int l = 0; l < 2; ++l)
				{
					const WfCand& kLumber = *apLumber[l];
					if (kLumber.iWorker < 0 || kLumber.iWorker == kCarpenter.iWorker
						|| (aiOutput[WF_TYPE(kLumber.iWorker, kLumber.iJob) * iNumJobs + kLumber.iJob] < iMissing && iBothLumber < iMissing)
						|| (aCities[c].iSurplus + kCarpenter.iFoodDelta + kLumber.iFoodDelta < WF_FOOD_MIN && kCarpenter.iFoodDelta + kLumber.iFoodDelta <= 0
							&& kCarpenter.iValue < WF_PRIORITY / 2)) // a must carpenter takes its lumberjack below the food floor
					{
						continue;
					}
					const int iPairValue = (kCarpenter.iValue + kLumber.iValue) / 2;
					if (iPairValue > iBestPairValue)
					{
						iBestPairValue = iPairValue;
						kPairJob = kCarpenter;
						kPairFood = kLumber; // the partner (food or lumber job) is assigned first
					}
				}
			}
		}
		if (kBest.iWorker < 0 && kPairJob.iWorker < 0)
		{
			break;
		}
		if (kPairJob.iWorker >= 0 && iBestPairValue > kBest.iValue)
		{
			WF_ASSIGN(kPairFood.iWorker, kPairFood.iJob);
			WF_ASSIGN(kPairJob.iWorker, kPairJob.iJob);
		}
		else
		{
			WF_ASSIGN(kBest.iWorker, kBest.iJob);
		}
	}

	// 4. seats now: the planned colonists present in the colony, food jobs first, as far as the present food carries
	std::vector<int> aiOrder;
	// (for the diagnosis log: the stand-ins of this turn: worker, job left (-1), job taken, value of both)
	std::vector<int> aiStandWorker, aiStandFrom, aiStandTo, aiStandOld, aiStandNew;
	for (int c = 0; c < iNumCities; ++c)
	{
		WfCity& kCity = aCities[c];
		aiOrder.clear();
		for (int j = 0; j < iNumJobs; ++j)
		{
			if (aJobs[j].iCity == c && aJobs[j].iHolder >= 0 && aiEta[aJobs[j].iHolder * iNumCities + c] == 0)
			{
				aiOrder.push_back(j);
			}
		}
		// food jobs (by food) first, then the carpenters and the expert statesmen and preachers, then the lumberjacks working
		// for the carpenters, then the others by value
		#define WF_SEAT_KEY(j, iT) (aiFood[(iT) * iNumJobs + (j)] * 1000000 + std::max(0, aiGold[(iT) * iNumJobs + (j)]) \
			+ ((aJobs[j].eYield == YIELD_HAMMERS || ((aJobs[j].eYield == YIELD_BELLS || aJobs[j].eYield == YIELD_CROSSES) \
				&& wfIsExpertFor(*aWorkers[aJobs[j].iHolder].pUnit, aJobs[j].eProfession))) ? 900000 \
				: ((aJobs[j].eYield == YIELD_LUMBER && kCity.aiUse[YIELD_LUMBER] > 0) ? 800000 : 0)))
		for (uint a = 0; a < aiOrder.size(); ++a)
		{
			for (uint b = a + 1; b < aiOrder.size(); ++b)
			{
				const int ja = aiOrder[a];
				const int jb = aiOrder[b];
				const int iTa = WF_TYPE(aJobs[ja].iHolder, ja);
				const int iTb = WF_TYPE(aJobs[jb].iHolder, jb);
				const int iKeyA = WF_SEAT_KEY(ja, iTa);
				const int iKeyB = WF_SEAT_KEY(jb, iTb);
				if (iKeyB > iKeyA)
				{
					std::swap(aiOrder[a], aiOrder[b]);
				}
			}
		}
		for (uint a = 0; a < aiOrder.size(); ++a)
		{
			const int j = aiOrder[a];
			const int w = aJobs[j].iHolder;
			const int iDelta = aiFood[WF_TYPE(w, j) * iNumJobs + j] - iConsumption;
			// the first colonist always gets in (a colony keeps at least one citizen)
			if (kCity.iNowSurplus + iDelta >= 0 || a == 0)
			{
				aJobs[j].iSeated = w;
				aWorkers[w].iSeatCity = c;
				kCity.iNowSurplus += iDelta;
			}
		}
		// jobs whose planned holder is on its way: any colonist present works them meanwhile, an idle one or one waiting for
		// food, or one who leaves a job worth clearly less for it (not a carpenter, an expert statesman or preacher); the
		// carpenter jobs first. The planned holder takes the job when it has arrived.
		for (int iGuard = 0; iGuard < 40; ++iGuard)
		{
			int iBestWorker = -1;
			int iBestJob = -1;
			int iBestFrom = -1;
			int iBestValue = 0;
			int iBestOld = 0;
			for (int j = 0; j < iNumJobs; ++j)
			{
				const WfJob& kJob = aJobs[j];
				if (kJob.iCity != c || kJob.iHolder < 0 || kJob.iSeated >= 0 || aiEta[kJob.iHolder * iNumCities + c] == 0)
				{
					continue;
				}
				// a carpenter job needs lumber now (stock, center plot or the plan)
				if (kJob.eYield == YIELD_HAMMERS && kCity.aiRaw[YIELD_LUMBER] <= 0)
				{
					continue;
				}
				for (int w = 0; w < iNumWorkers; ++w)
				{
					const WfWorker& kWorker = aWorkers[w];
					if (kWorker.bLearning || aiEta[w * iNumCities + c] != 0 || (kWorker.iJob >= 0 && aJobs[kWorker.iJob].iCity != c))
					{
						continue;
					}
					// the job it works now (if seated here)
					int iFrom = -1;
					if (kWorker.iSeatCity == c)
					{
						for (int k = 0; k < iNumJobs && iFrom < 0; ++k)
						{
							if (aJobs[k].iSeated == w)
							{
								iFrom = k;
							}
						}
						if (iFrom < 0 || aJobs[iFrom].eYield == YIELD_HAMMERS
							|| ((aJobs[iFrom].eYield == YIELD_BELLS || aJobs[iFrom].eYield == YIELD_CROSSES) && wfIsExpertFor(*kWorker.pUnit, aJobs[iFrom].eProfession)))
						{
							continue;
						}
					}
					else if (kWorker.iSeatCity >= 0)
					{
						continue;
					}
					const int iT = WF_TYPE(w, j);
					const int iGold = aiGold[iT * iNumJobs + j];
					int iDelta = aiFood[iT * iNumJobs + j] - iConsumption;
					int iOldValue = 0;
					if (iFrom >= 0)
					{
						const int iTFrom = WF_TYPE(w, iFrom);
						const int iOldDelta = aiFood[iTFrom * iNumJobs + iFrom] - iConsumption;
						int iOldGold = aiGold[iTFrom * iNumJobs + iFrom];
						// a lumberjack's lumber the carpenters need is worth WF_LUMBER_GOLD, as in the plan (at 1 gold the lumberjacks
						// of Trankebar and Fort Christina stood in for coal miners and hunters while the carpenters lacked lumber)
						const int iOldOutput = aiOutput[iTFrom * iNumJobs + iFrom];
						if (aJobs[iFrom].eYield == YIELD_LUMBER && aJobs[iFrom].pPlot != NULL && iOldOutput > 0)
						{
							const int iForCarpenters = std::max(0, std::min(iOldOutput, kCity.aiUse[YIELD_LUMBER] - (kCity.aiRaw[YIELD_LUMBER] - iOldOutput)));
							iOldGold = std::max(iOldGold, iOldGold * (iOldOutput - iForCarpenters) / iOldOutput + iForCarpenters * WF_LUMBER_GOLD);
						}
						iOldValue = iOldGold + wfFoodGold(kCity.iNowSurplus - iOldDelta, iOldDelta);
						iDelta -= iOldDelta; // the change of the food
					}
					if (iGold <= -1000000 || kCity.iNowSurplus + iDelta < 0)
					{
						continue;
					}
					int iValue = iGold + wfFoodGold(kCity.iNowSurplus, aiFood[iT * iNumJobs + j] - iConsumption);
					if (iFrom >= 0 && iValue <= iOldValue * 6 / 5)
					{
						continue;
					}
					iValue -= iOldValue;
					if (kJob.eYield == YIELD_HAMMERS)
					{
						iValue += WF_PRIORITY;
					}
					if (iValue > iBestValue)
					{
						iBestValue = iValue;
						iBestWorker = w;
						iBestJob = j;
						iBestFrom = iFrom;
						iBestOld = iOldValue;
					}
				}
			}
			if (iBestWorker < 0)
			{
				break;
			}
			int iChange = aiFood[WF_TYPE(iBestWorker, iBestJob) * iNumJobs + iBestJob] - iConsumption;
			if (iBestFrom >= 0)
			{
				iChange -= aiFood[WF_TYPE(iBestWorker, iBestFrom) * iNumJobs + iBestFrom] - iConsumption;
				aJobs[iBestFrom].iSeated = -1;
			}
			aJobs[iBestJob].iSeated = iBestWorker;
			aWorkers[iBestWorker].iSeatCity = c;
			kCity.iNowSurplus += iChange;
			aiStandWorker.push_back(iBestWorker);
			aiStandFrom.push_back(iBestFrom);
			aiStandTo.push_back(iBestJob);
			aiStandOld.push_back(iBestOld);
			aiStandNew.push_back(iBestValue + iBestOld - (aJobs[iBestJob].eYield == YIELD_HAMMERS ? WF_PRIORITY : 0));
		}
		// a colony nobody is seated in keeps its best citizen in its job
		bool bSeated = false;
		for (int j = 0; j < iNumJobs && !bSeated; ++j)
		{
			bSeated = aJobs[j].iCity == c && aJobs[j].iSeated >= 0;
		}
		if (!bSeated && kCity.pCity->getPopulation() > 0)
		{
			for (int w = 0; w < iNumWorkers; ++w)
			{
				const int j = aWorkers[w].iCurrentJob;
				if (aWorkers[w].pPopCity == kCity.pCity && j >= 0 && aJobs[j].iSeated < 0 && aWorkers[w].iSeatCity < 0)
				{
					aJobs[j].iSeated = w;
					aWorkers[w].iSeatCity = c;
					break;
				}
			}
		}
	}

	// 5. carry out: the colony the idle colonists wait in (where they are, else the colony with the most valuable job
	// short of food, else the first one)
	CvCity* pIdleCity = NULL;
	{
		int iBestBlocked = 0;
		for (int c = 0; c < iNumCities; ++c)
		{
			if (aCities[c].iBlockedValue > iBestBlocked)
			{
				iBestBlocked = aCities[c].iBlockedValue;
				pIdleCity = aCities[c].pCity;
			}
		}
		if (pIdleCity == NULL)
		{
			pIdleCity = aCities[0].pCity;
		}
	}
	for (int c = 0; c < iNumCities; ++c)
	{
		CvCity* const pCity = aCities[c].pCity;
		// a. citizens without a seat here leave the colony (they wait on its plot or go to their job)
		std::vector<CvUnit*> apLeaving;
		for (int i = 0; i < pCity->getPopulation(); ++i)
		{
			CvUnit* const pCitizen = pCity->getPopulationUnitByIndex(i);
			bool bSeatedHere = false;
			for (int w = 0; w < iNumWorkers && !bSeatedHere; ++w)
			{
				bSeatedHere = aWorkers[w].pUnit == pCitizen && aWorkers[w].iSeatCity == c;
			}
			if (pCitizen != NULL && !bSeatedHere)
			{
				apLeaving.push_back(pCitizen);
			}
		}
		// b. seated colonists not yet in the colony join it
		for (int w = 0; w < iNumWorkers; ++w)
		{
			WfWorker& kWorker = aWorkers[w];
			if (kWorker.iSeatCity != c || kWorker.pPopCity == pCity)
			{
				continue;
			}
			if (kWorker.pAtCity == pCity && kWorker.pUnit->canJoinCity(pCity->plot(), false, true))
			{
				pCity->addPopulationUnit(kWorker.pUnit, NO_PROFESSION);
				kWorker.pPopCity = pCity;
				kWorker.pAtCity = NULL;
			}
			else
			{
				// cannot join now, tries again next turn: its seat and its food are free again
				if (gPlayerLogLevel >= 1)
				{
					const CvUnit& kUnit = *kWorker.pUnit;
					logWTP(WTP_LOG_DIAG, getID(), "diag %S: %S cannot join (%s)", pCity->getName().GetCString(), kUnit.getName().GetCString(),
						kWorker.pAtCity != pCity ? "not on the colony plot" : pCity->isDisorder() ? "colony in disorder" : kUnit.movesLeft() <= 0 ? "no moves left"
						: !kUnit.getUnitInfo().isFound() ? "unit cannot join colonies" : "other reason");
				}
				kWorker.iSeatCity = -1;
				for (int j = 0; j < iNumJobs; ++j)
				{
					if (aJobs[j].iSeated == w)
					{
						aJobs[j].iSeated = -1;
						aCities[c].iNowSurplus -= aiFood[WF_TYPE(w, j) * iNumJobs + j] - iConsumption;
					}
				}
			}
		}
		for (uint i = 0; i < apLeaving.size(); ++i)
		{
			if (pCity->getPopulation() <= 1)
			{
				break;
			}
			if (pCity->removePopulationUnit(CREATE_ASSERT_DATA, apLeaving[i], false, eDefaultProfession))
			{
				apLeaving[i]->AI_setUnitAIType(UNITAI_COLONIST);
				for (int w = 0; w < iNumWorkers; ++w)
				{
					if (aWorkers[w].pUnit == apLeaving[i])
					{
						aWorkers[w].pPopCity = NULL;
						aWorkers[w].pAtCity = pCity;
					}
				}
			}
		}
		// c. the jobs: first everybody whose job changes stops working, then everybody gets the new job
		std::vector<int> aiChange;
		for (int j = 0; j < iNumJobs; ++j)
		{
			const WfJob& kJob = aJobs[j];
			if (kJob.iCity != c || kJob.iSeated < 0 || aWorkers[kJob.iSeated].iSeatCity != c)
			{
				continue;
			}
			CvUnit* const pUnit = aWorkers[kJob.iSeated].pUnit;
			const CvPlot* const pWorked = pCity->getPlotWorkedByUnit(pUnit);
			if (pUnit->getProfession() != kJob.eProfession || pWorked != kJob.pPlot)
			{
				aiChange.push_back(j);
				if (pWorked != NULL)
				{
					pCity->clearUnitWorkingPlot(pCity->getCityPlotIndex(pWorked));
				}
				pUnit->setColonistLocked(false);
				pUnit->setProfession(NO_PROFESSION);
			}
		}
		for (uint i = 0; i < aiChange.size(); ++i)
		{
			const WfJob& kJob = aJobs[aiChange[i]];
			CvUnit* const pUnit = aWorkers[kJob.iSeated].pUnit;
			if (kJob.pPlot != NULL)
			{
				const CityPlotTypes ePlot = pCity->getCityPlotIndex(kJob.pPlot);
				if (pUnit->canHaveProfession(kJob.eProfession, false))
				{
					// whoever still works the plot gives it up only now that the new worker can take it
					CvUnit* const pOther = pCity->getUnitWorkingPlot(ePlot);
					if (pOther != NULL)
					{
						pCity->clearUnitWorkingPlot(ePlot);
						pOther->setProfession(NO_PROFESSION);
					}
					pUnit->setProfession(kJob.eProfession);
					pCity->AI().AI_setUnitWorkingPlot(ePlot, pUnit->getID());
				}
				else if (gPlayerLogLevel >= 1)
				{
					logWTPCity(*pCity, "WTP plan: %S cannot take the job %S in %S", pUnit->getName().GetCString(), GC.getProfessionInfo(kJob.eProfession).getDescription(), pCity->getName().GetCString());
				}
			}
			else if (pUnit->canHaveProfession(kJob.eProfession, false))
			{
				pUnit->setProfession(kJob.eProfession);
			}
			else if (gPlayerLogLevel >= 1)
			{
				logWTPCity(*pCity, "WTP plan: %S cannot take the job %S in %S", pUnit->getName().GetCString(), GC.getProfessionInfo(kJob.eProfession).getDescription(), pCity->getName().GetCString());
			}
		}
		for (int j = 0; j < iNumJobs; ++j)
		{
			if (aJobs[j].iCity == c && aJobs[j].iSeated >= 0 && aWorkers[aJobs[j].iSeated].iSeatCity == c && aWorkers[aJobs[j].iSeated].pUnit->getProfession() != NO_PROFESSION)
			{
				aWorkers[aJobs[j].iSeated].pUnit->setColonistLocked(true);
			}
		}
	}
	// d. the tasks: planned colonists go to their colony (and wait there if they are not seated), idle ones wait
	for (int w = 0; w < iNumWorkers; ++w)
	{
		WfWorker& kWorker = aWorkers[w];
		CvUnitAI& kUnit = *kWorker.pUnit;
		if (kWorker.bLearning)
		{
			continue;
		}
		if (kWorker.iJob >= 0)
		{
			const WfJob& kJob = aJobs[kWorker.iJob];
			kUnit.AI_setWfTask(CvUnitAI::WF_EXPERT_JOB, kJob.pCity->getID(), kJob.eProfession,
				kJob.pPlot != NULL ? kJob.pPlot->getX_INLINE() : -1, kJob.pPlot != NULL ? kJob.pPlot->getY_INLINE() : -1, -1, -1);
			// a citizen of another colony leaves for its job
			if (kWorker.pPopCity != NULL && kWorker.pPopCity != kJob.pCity && kWorker.pPopCity->getPopulation() > 1)
			{
				if (kWorker.pPopCity->removePopulationUnit(CREATE_ASSERT_DATA, &kUnit, false, eDefaultProfession))
				{
					kUnit.AI_setUnitAIType(UNITAI_COLONIST);
					kWorker.pAtCity = kWorker.pPopCity;
					kWorker.pPopCity = NULL;
				}
			}
		}
		else if (kWorker.iSeatCity >= 0)
		{
			kUnit.AI_clearWfTask(); // works a job meanwhile, its colony replans it every turn
		}
		else if (kWorker.pPopCity == NULL)
		{
			CvCity* const pWait = (kWorker.pAtCity != NULL) ? kWorker.pAtCity : pIdleCity;
			kUnit.AI_setWfTask(CvUnitAI::WF_WAIT, pWait->getID(), NO_PROFESSION, -1, -1, -1, -1);
			++m_iWfIdleCount;
		}
	}

	// 6a. learning: a valuable job (resource, improvement, building) without an expert, whose expert a village near the colony
	// teaches: an idle free colonist (or the regular colonist in that job) learns it; at most 2 new learners per turn
	int iNewLearners = 0;
	for (int j = 0; j < iNumJobs && iNewLearners < 2; ++j)
	{
		WfJob& kJob = aJobs[j];
		if (!kJob.bNotable || kJob.eExpertClass == NO_UNITCLASS || kJob.eExpertClass == GLOBAL_DEFINE_DEFAULT_POPULATION_UNIT
			|| kJob.eExpertClass == m_eWfExpertToBuy || (kJob.iHolder >= 0 && wfIsExpertFor(*aWorkers[kJob.iHolder].pUnit, kJob.eProfession))
			|| (kJob.iHolder < 0 && !WF_IS_FREE_JOB(j)))
		{
			continue;
		}
		CvCity* const pVillage = AI_wfTeachingVillage(kJob.eExpertClass, *kJob.pCity);
		if (pVillage == NULL)
		{
			continue;
		}
		int iBest = -1;
		int iBestScore = 0;
		for (int w = 0; w < iNumWorkers; ++w)
		{
			const WfWorker& kWorker = aWorkers[w];
			const CvUnitAI& kUnit = *kWorker.pUnit;
			if (kWorker.bLearning || kWorker.bExpert || kUnit.getUnitInfo().getLearnTime() < 0
				|| (kWorker.pPopCity == NULL && (!kUnit.isOnMap() || kUnit.isCargo()))
				|| (kWorker.iJob >= 0 && kWorker.iJob != j) || (kWorker.iSeatCity >= 0 && kWorker.iJob != j))
			{
				continue;
			}
			const CvPlot* const pFrom = (kWorker.pPopCity != NULL) ? kWorker.pPopCity->plot() : kUnit.plot();
			if (pFrom == NULL || pFrom->area() != pVillage->area())
			{
				continue;
			}
			const int iDistance = plotDistance(pFrom->getX_INLINE(), pFrom->getY_INLINE(), pVillage->getX_INLINE(), pVillage->getY_INLINE());
			if (iDistance > 7)
			{
				continue;
			}
			// idle colonists first, then the one in the job, then the nearest
			const int iScore = (kWorker.iJob < 0 ? 200 : 100) - iDistance;
			if (iScore > iBestScore)
			{
				iBestScore = iScore;
				iBest = w;
			}
		}
		if (iBest < 0)
		{
			continue;
		}
		WfWorker& kLearner = aWorkers[iBest];
		CvUnitAI& kUnit = *kLearner.pUnit;
		const bool bCountedIdle = kLearner.iJob < 0 && kLearner.iSeatCity < 0 && kLearner.pPopCity == NULL;
		if (kLearner.pPopCity != NULL)
		{
			if (kLearner.pPopCity->getPopulation() <= 1 || !kLearner.pPopCity->removePopulationUnit(CREATE_ASSERT_DATA, &kUnit, false, eDefaultProfession))
			{
				continue;
			}
			if (kJob.iSeated == iBest)
			{
				kJob.iSeated = -1;
			}
		}
		kUnit.AI_setUnitAIType(UNITAI_COLONIST);
		kUnit.AI_setWfTask(CvUnitAI::WF_LEARN, kJob.pCity->getID(), kJob.eProfession,
			kJob.pPlot != NULL ? kJob.pPlot->getX_INLINE() : -1, kJob.pPlot != NULL ? kJob.pPlot->getY_INLINE() : -1, pVillage->getX_INLINE(), pVillage->getY_INLINE());
		if (bCountedIdle)
		{
			--m_iWfIdleCount;
		}
		// the regular colonist planned for the job so far works it meanwhile (if seated) or waits for another job
		const int iOldHolder = kJob.iHolder;
		if (iOldHolder >= 0 && iOldHolder != iBest)
		{
			WfWorker& kOld = aWorkers[iOldHolder];
			kOld.iJob = -1;
			if (kOld.iSeatCity >= 0)
			{
				kOld.pUnit->AI_clearWfTask();
			}
			else
			{
				CvCity* const pWait = (kOld.pAtCity != NULL) ? kOld.pAtCity : pIdleCity;
				kOld.pUnit->AI_setWfTask(CvUnitAI::WF_WAIT, pWait->getID(), NO_PROFESSION, -1, -1, -1, -1);
				++m_iWfIdleCount;
			}
		}
		kLearner.bLearning = true;
		kLearner.iJob = j;
		kJob.iHolder = iBest;
		if (kJob.iGroup >= 0)
		{
			++aiGroupHeld[kJob.iGroup];
		}
		++iNewLearners;
		if (gPlayerLogLevel >= 1) logWTPCity(*kJob.pCity, "WTP plan: %S learns %S in %S (%d,%d) for %S in %S at %d,%d", kUnit.getName().GetCString(),
			GC.getUnitClassInfo(kJob.eExpertClass).getDescription(), pVillage->getName().GetCString(), pVillage->getX_INLINE(), pVillage->getY_INLINE(),
			GC.getProfessionInfo(kJob.eProfession).getDescription(), kJob.pCity->getName().GetCString(),
			kJob.pPlot != NULL ? kJob.pPlot->getX_INLINE() : -1, kJob.pPlot != NULL ? kJob.pPlot->getY_INLINE() : -1);
	}

	// 6b. colonists to buy: an expert for a job (instead of a regular colonist, or for a free job with food to spare), or a
	// food provider for a colony whose best jobs lack food. Value per price; bought only within a payback of 30 turns
	{
		const UnitClassTypes eFarmerClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FARMER");
		const UnitClassTypes eFishermanClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FISHERMAN");
		int iBestFoodGain = 0;
		for (int j = 0; j < iNumJobs; ++j)
		{
			const WfJob& kJob = aJobs[j];
			if (kJob.eExpertClass == NO_UNITCLASS || kJob.eExpertClass == GLOBAL_DEFINE_DEFAULT_POPULATION_UNIT
				|| (kJob.iHolder >= 0 && (aWorkers[kJob.iHolder].bLearning || wfIsExpertFor(*aWorkers[kJob.iHolder].pUnit, kJob.eProfession)))
				|| (kJob.iHolder < 0 && !WF_IS_FREE_JOB(j)))
			{
				continue;
			}
			// no purchase while an expert of this trade works in another trade or waits (the next plan gives it the job)
			bool bExpertAround = false;
			for (int w = 0; w < iNumWorkers && !bExpertAround; ++w)
			{
				const WfWorker& kWorker = aWorkers[w];
				bExpertAround = !kWorker.bLearning && wfIsExpertFor(*kWorker.pUnit, kJob.eProfession)
					&& (kWorker.iJob < 0 || aJobs[kWorker.iJob].eProfession != kJob.eProfession);
			}
			if (bExpertAround)
			{
				continue;
			}
			const UnitTypes eExpert = kCiv.getCivilizationUnits(kJob.eExpertClass);
			const int iPrice = (eExpert != NO_UNIT && canTradeWithEurope()) ? getEuropeUnitBuyPrice(eExpert) : -1;
			if (iPrice <= 0)
			{
				continue;
			}
			// raw material experts a village near the colony teaches are learned (lumberjacks excepted)
			if (kJob.pPlot != NULL && CvString(GC.getProfessionInfo(kJob.eProfession).getType()) != "PROFESSION_LUMBERJACK" && AI_wfTeachingVillage(kJob.eExpertClass, *kJob.pCity, 8) != NULL)
			{
				continue;
			}
			int iExpertType = -1;
			for (int t = 0; t < iNumTypes; ++t)
			{
				if (aeTypes[t] == eExpert)
				{
					iExpertType = t;
				}
			}
			int iExpertGold;
			int iExpertFood;
			if (iExpertType >= 0)
			{
				iExpertGold = aiGold[iExpertType * iNumJobs + j];
				iExpertFood = aiFood[iExpertType * iNumJobs + j];
			}
			else
			{
				const int iOutput = wfOutput(*kJob.pCity, kJob.eProfession, eExpert, kJob.pPlot);
				iExpertFood = wfFoodOutput(kJob.eProfession, iOutput);
				iExpertGold = kJob.bFood ? 0 : wfYieldGold(*kJob.pCity, kJob.eYield, iOutput, kJob.iSlot);
				const CvProfessionInfo& kProfession = GC.getProfessionInfo(kJob.eProfession);
				for (int k = 0; k < kProfession.getNumYieldsConsumed() && !kJob.bFood; ++k)
				{
					const YieldTypes eIn = (YieldTypes)kProfession.getYieldsConsumed(k);
					if (eIn != NO_YIELD)
					{
						iExpertGold -= wfInputGold(*kJob.pCity, kJob.eYield, eIn, iOutput);
					}
				}
			}
			const WfCity& kCity = aCities[kJob.iCity];
			// an expert for a building job only if the colony produces its raw material (its plots, planned): the stock runs
			// out (a Master Sailmaker was bought for flax from the stock and gave up its trade 8 turns later). Its output
			// counts only as far as the produced raw material goes
			const CvProfessionInfo& kJobProfession = GC.getProfessionInfo(kJob.eProfession);
			if (!kJobProfession.isWorkPlot() && kJobProfession.getNumYieldsConsumed() > 0)
			{
				const int iOutput = (iExpertType >= 0) ? aiOutput[iExpertType * iNumJobs + j] : wfOutput(*kJob.pCity, kJob.eProfession, eExpert, kJob.pPlot);
				// the regular colonist in the job leaves its raw material to the expert
				const int iHolderUse = (kJob.iHolder >= 0) ? aiOutput[WF_TYPE(kJob.iHolder, j) * iNumJobs + j] : 0;
				int iProduced = iOutput;
				for (int k = 0; k < kJobProfession.getNumYieldsConsumed(); ++k)
				{
					const YieldTypes eIn = (YieldTypes)kJobProfession.getYieldsConsumed(k);
					if (eIn != NO_YIELD)
					{
						iProduced = std::min(iProduced, kCity.aiRaw[eIn] - kCity.aiStock[eIn] - kCity.aiUse[eIn] + iHolderUse);
					}
				}
				if (iProduced < std::max(1, iOutput / 2))
				{
					continue;
				}
				if (iProduced < iOutput)
				{
					iExpertGold = wfYieldGold(*kJob.pCity, kJob.eYield, iProduced, kJob.iSlot);
					for (int k = 0; k < kJobProfession.getNumYieldsConsumed(); ++k)
					{
						const YieldTypes eIn = (YieldTypes)kJobProfession.getYieldsConsumed(k);
						if (eIn != NO_YIELD)
						{
							iExpertGold -= wfInputGold(*kJob.pCity, kJob.eYield, eIn, iProduced);
						}
					}
				}
			}
			int iGain;
			if (kJob.iHolder >= 0)
			{
				// replaces the regular colonist, who is free for another job then: +12 gold for that (somebody can always work as
				// lumberjack or fisherman; user decision)
				const int iT = WF_TYPE(kJob.iHolder, j);
				iGain = iExpertGold - aiGold[iT * iNumJobs + j] + wfFoodGold(kCity.iSurplus, iExpertFood - aiFood[iT * iNumJobs + j]) + 1200;
			}
			else if (kCity.iSurplus + iExpertFood - iConsumption >= WF_FOOD_MIN)
			{
				iGain = iExpertGold + wfFoodGold(kCity.iSurplus, iExpertFood - iConsumption);
			}
			else
			{
				continue; // short of food: a food provider first
			}
			if (iGain <= 0 || iGain * 30 < iPrice * 100)
			{
				continue;
			}
			WfOpenOpportunity kOpen;
			kOpen.iCityID = kJob.pCity->getID();
			kOpen.eProfession = kJob.eProfession;
			kOpen.iPlotIndex = kJob.iPlotKey;
			kOpen.eExpertClass = kJob.eExpertClass;
			kOpen.iValue = iGain;
			kOpen.iPrice = iPrice;
			kOpen.bAfrica = false;
			m_aWfOpenOpportunities.push_back(kOpen);
		}
		// African slaves (bought in Africa) for the cotton, tobacco, indigo and sugar plots, by value per price like the experts
		// (user 2026-10-17: not for other jobs, the unhappiness they cause does not matter to the AI, more would be too strong)
		{
			const UnitTypes eSlave = AI_africaFoodWorker();
			const int iSlavePrice = (eSlave != NO_UNIT) ? getAfricaUnitBuyPrice(eSlave) : -1;
			const UnitClassTypes eSlaveClass = (eSlave != NO_UNIT) ? (UnitClassTypes)GC.getUnitInfo(eSlave).getUnitClassType() : NO_UNITCLASS;
			for (int j = 0; j < iNumJobs && iSlavePrice > 0; ++j)
			{
				const WfJob& kJob = aJobs[j];
				if (kJob.pPlot == NULL || !(kJob.eYield == YIELD_COTTON || kJob.eYield == YIELD_TOBACCO || kJob.eYield == YIELD_INDIGO || kJob.eYield == YIELD_SUGAR))
				{
					continue;
				}
				if ((kJob.iHolder >= 0 && (aWorkers[kJob.iHolder].bLearning || aWorkers[kJob.iHolder].bExpert)) || (kJob.iHolder < 0 && !WF_IS_FREE_JOB(j)))
				{
					continue;
				}
				const int iOutput = wfOutput(*kJob.pCity, kJob.eProfession, eSlave, kJob.pPlot);
				const int iSlaveGold = wfYieldGold(*kJob.pCity, kJob.eYield, iOutput, kJob.iSlot);
				const int iSlaveFood = wfFoodOutput(kJob.eProfession, iOutput);
				const WfCity& kCity = aCities[kJob.iCity];
				int iGain;
				if (kJob.iHolder >= 0)
				{
					// replaces the regular colonist, who is free for another job then (+12 gold, as for experts)
					const int iT = WF_TYPE(kJob.iHolder, j);
					if (aWorkers[kJob.iHolder].pUnit->getUnitType() == eSlave || iSlaveGold <= aiGold[iT * iNumJobs + j])
					{
						continue; // only a slave that produces more than the colonist in the job
					}
					iGain = iSlaveGold - aiGold[iT * iNumJobs + j] + wfFoodGold(kCity.iSurplus, iSlaveFood - aiFood[iT * iNumJobs + j]) + 1200;
				}
				else if (kCity.iSurplus + iSlaveFood - iConsumption >= WF_FOOD_MIN)
				{
					iGain = iSlaveGold + wfFoodGold(kCity.iSurplus, iSlaveFood - iConsumption);
				}
				else
				{
					continue;
				}
				if (iGain <= 0 || iGain * 30 < iSlavePrice * 100)
				{
					continue;
				}
				WfOpenOpportunity kOpen;
				kOpen.iCityID = kJob.pCity->getID();
				kOpen.eProfession = kJob.eProfession;
				kOpen.iPlotIndex = kJob.iPlotKey;
				kOpen.eExpertClass = eSlaveClass;
				kOpen.iValue = iGain;
				kOpen.iPrice = iSlavePrice;
				kOpen.bAfrica = true;
				m_aWfOpenOpportunities.push_back(kOpen);
			}
		}
		// food providers: an expert farmer or fisherman for the best free food plot of a colony whose jobs lack food
		for (int c = 0; c < iNumCities; ++c)
		{
			const WfCity& kCity = aCities[c];
			if (kCity.iBlockedJob < 0)
			{
				continue;
			}
			// (kind 2: an African slave, bought in Africa, for the farm jobs)
			for (int iKind = 0; iKind < 3; ++iKind)
			{
				const UnitClassTypes eJobClass = (iKind == 1) ? eFishermanClass : eFarmerClass;
				const UnitTypes eUnit = (iKind == 2) ? AI_africaFoodWorker() : (eJobClass != NO_UNITCLASS) ? kCiv.getCivilizationUnits(eJobClass) : NO_UNIT;
				const UnitClassTypes eClass = (iKind == 2) ? ((eUnit != NO_UNIT) ? (UnitClassTypes)GC.getUnitInfo(eUnit).getUnitClassType() : NO_UNITCLASS) : eJobClass;
				const int iPrice = (iKind == 2) ? ((eUnit != NO_UNIT) ? getAfricaUnitBuyPrice(eUnit) : -1)
					: (eUnit != NO_UNIT && canTradeWithEurope()) ? getEuropeUnitBuyPrice(eUnit) : -1;
				if (iPrice <= 0)
				{
					continue;
				}
				int iBestJob = -1;
				int iBestFood = 0;
				for (int j = 0; j < iNumJobs; ++j)
				{
					const WfJob& kJob = aJobs[j];
					if (kJob.iCity != c || !kJob.bFood || kJob.eExpertClass != eJobClass || !WF_IS_FREE_JOB(j)
						|| (kJob.iPlotKey >= 0 && kJob.iPlotKey == aJobs[kCity.iBlockedJob].iPlotKey))
					{
						continue;
					}
					const int iFood = wfOutput(*kJob.pCity, kJob.eProfession, eUnit, kJob.pPlot);
					if (iFood > iBestFood)
					{
						iBestFood = iFood;
						iBestJob = j;
					}
				}
				if (iBestJob < 0 || iBestFood <= iConsumption)
				{
					continue;
				}
				// the food job itself and the blocked job an idle colonist can take then
				const int iFoodGain = wfFoodGold(kCity.iSurplus, iBestFood - iConsumption);
				const bool bUnblocks = m_iWfIdleCount > 0 && kCity.iSurplus + iBestFood - iConsumption * 2 >= WF_FOOD_MIN;
				const int iGain = iFoodGain + (bUnblocks ? kCity.iBlockedValue : 0);
				if (iGain <= 0 || iGain * 30 < iPrice * 100)
				{
					continue;
				}
				WfOpenOpportunity kOpen;
				kOpen.iCityID = kCity.pCity->getID();
				kOpen.eProfession = aJobs[iBestJob].eProfession;
				kOpen.iPlotIndex = aJobs[iBestJob].iPlotKey;
				kOpen.eExpertClass = eClass;
				kOpen.iValue = iGain;
				kOpen.iPrice = iPrice;
				kOpen.bAfrica = iKind == 2;
				m_aWfOpenOpportunities.push_back(kOpen);
				if (iGain > iBestFoodGain)
				{
					iBestFoodGain = iGain;
					m_iWfFoodCityID = kCity.pCity->getID();
					m_bWfFoodWater = iKind == 1;
				}
			}
		}
		// colonies whose jobs lack food ask the pioneers for food improvements
		for (int c = 0; c < iNumCities; ++c)
		{
			if (aCities[c].iBlockedValue > 0)
			{
				aCities[c].pCity->AI().AI_setWfFoodRequest();
			}
		}
	}

	// log: per colony the jobs with their status, the food of the plan and now, the colonists waiting
	if (gPlayerLogLevel >= 1)
	{
		for (int c = 0; c < iNumCities; ++c)
		{
			const WfCity& kCity = aCities[c];
			int iWaiting = 0;
			for (int w = 0; w < iNumWorkers; ++w)
			{
				iWaiting += (aWorkers[w].pAtCity == kCity.pCity && aWorkers[w].iSeatCity != c) ? 1 : 0;
			}
			logWTPCity(*kCity.pCity, "WTP plan turn %d: %S citizens %d, food now %+d (planned %+d with %d colonists, base %+d), colonists waiting on the plot %d, blocked by food: %d%s%S",
				iTurn, kCity.pCity->getName().GetCString(), kCity.pCity->getPopulation(), kCity.iNowSurplus, kCity.iSurplus, kCity.iHeld, kCity.iBaseFood, iWaiting,
				kCity.iBlockedValue / 100, kCity.iBlockedJob >= 0 ? " gold, best " : "", kCity.iBlockedJob >= 0 ? GC.getProfessionInfo(aJobs[kCity.iBlockedJob].eProfession).getDescription() : L"");
			for (int j = 0; j < iNumJobs; ++j)
			{
				const WfJob& kJob = aJobs[j];
				if (kJob.iCity != c || (kJob.iHolder < 0 && kJob.iSeated < 0))
				{
					continue;
				}
				const int w = (kJob.iSeated >= 0) ? kJob.iSeated : kJob.iHolder;
				const int iT = WF_TYPE(w, j);
				// where the planned colonist is if it is not here (docks, ship, village, other colony, plot) and its travel time
				CvWString szWhere;
				if (kJob.iHolder >= 0 && aiEta[kJob.iHolder * iNumCities + c] > 0)
				{
					const WfWorker& kHolder = aWorkers[kJob.iHolder];
					const CvUnit& kUnit = *kHolder.pUnit;
					if (kUnit.getUnitTravelState() == UNIT_TRAVEL_STATE_LIVE_AMONG_NATIVES)
					{
						szWhere = L"learning in a village";
					}
					else if (!kUnit.isOnMap())
					{
						szWhere = kUnit.isCargo() ? L"on a ship off the map" : L"in Europe";
					}
					else if (kHolder.pPopCity != NULL)
					{
						szWhere = CvWString::format(L"citizen of %s", kHolder.pPopCity->getName().GetCString());
					}
					else
					{
						const CvCity* const pHere = kUnit.plot()->getPlotCity();
						szWhere = CvWString::format(L"%s%s %d,%d", kUnit.isCargo() ? L"on a ship " : L"", pHere != NULL ? pHere->getName().GetCString() : L"at",
							kUnit.getX_INLINE(), kUnit.getY_INLINE());
						// the way it takes: by ship from a port (over land otherwise)
						int iPortID = -1;
						const CvPlot* const pFrom = (kHolder.pPopCity != NULL) ? kHolder.pPopCity->plot() : kUnit.plot();
						if (!kUnit.isCargo() && AI_wfTravelTurns(*pFrom, *kCity.pCity, &iPortID) != MAX_INT && iPortID >= 0 && getCity(iPortID) != NULL)
						{
							szWhere += CvWString::format(L", by ship from %s", getCity(iPortID)->getName().GetCString());
						}
					}
					szWhere = CvWString::format(L" [%s, %d turns]", szWhere.GetCString(), aiEta[kJob.iHolder * iNumCities + c]);
				}
				logWTPCity(*kCity.pCity, "WTP plan:   %S at %d,%d: %s (%S%s%S)%S, %d gold for %d units, %d food",
					GC.getProfessionInfo(kJob.eProfession).getDescription(), kJob.pPlot != NULL ? kJob.pPlot->getX_INLINE() : -1, kJob.pPlot != NULL ? kJob.pPlot->getY_INLINE() : -1,
					wfJobStatus(kJob, aWorkers), aWorkers[w].pUnit->getName().GetCString(),
					(kJob.iHolder >= 0 && kJob.iSeated >= 0 && kJob.iHolder != kJob.iSeated) ? ", planned: " : "",
					(kJob.iHolder >= 0 && kJob.iSeated >= 0 && kJob.iHolder != kJob.iSeated) ? aWorkers[kJob.iHolder].pUnit->getName().GetCString() : L"",
					szWhere.GetCString(), aiGold[iT * iNumJobs + j] / 100, aiOutput[iT * iNumJobs + j], aiFood[iT * iNumJobs + j]);
			}
		}
		int iLearners = 0;
		int iAway = 0;
		for (int w = 0; w < iNumWorkers; ++w)
		{
			iLearners += aWorkers[w].bLearning ? 1 : 0;
			iAway += (aWorkers[w].iJob >= 0 && aWorkers[w].iSeatCity < 0) ? 1 : 0;
		}
		logWTP(WTP_LOG_PLAN, getID(), "WTP plan: Player %d (%S) turn %d jobs %d, workforce %d, planned on their way or waiting %d, learning %d, idle %d, colonists to buy %d",
			getID(), getCivilizationDescription(0), iTurn, iNumJobs, iNumWorkers, iAway, iLearners, m_iWfIdleCount, (int)m_aWfOpenOpportunities.size());
	}

	// diagnosis log (WTP_Pxx_<civ>_diag.log): what the plan knew and why it decided so. Per colony the yield balance and every
	// plot with its job alternatives (values as a free colonist and as the expert) and the professions that lost the plot's
	// one "other" slot; per colonist its plan, its seat and its best alternatives; the stand-ins of this turn. Values in
	// gold: output at its value plus food at WF_FOOD_GOLD per food above the colonist's own consumption
	if (gPlayerLogLevel >= 1)
	{
		for (int c = 0; c < iNumCities; ++c)
		{
			const WfCity& kCity = aCities[c];
			const CvCity& kCvCity = *kCity.pCity;
			// raw material produced (planned plots and center plot) / from the stock / used by the planned building jobs
			CvWString szYields;
			for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
			{
				if (kCity.aiUse[eYield] > 0 || kCity.aiRaw[eYield] > kCity.aiStock[eYield])
				{
					szYields += CvWString::format(L" %s %d+%d/%d", GC.getYieldInfo(eYield).getDescription(), kCity.aiRaw[eYield] - kCity.aiStock[eYield],
						kCity.aiStock[eYield], kCity.aiUse[eYield]);
				}
			}
			logWTP(WTP_LOG_DIAG, getID(), "diag %S: food base %+d, planned %+d, now %+d; yields produced+stock/used:%S", kCvCity.getName().GetCString(),
				kCity.iBaseFood, kCity.iSurplus, kCity.iNowSurplus, szYields.GetCString());
			FOREACH(CityPlot)
			{
				if (eLoopCityPlot == CITY_HOME_PLOT)
				{
					continue;
				}
				CvPlot* const pLoopPlot = kCvCity.getCityIndexPlot(eLoopCityPlot);
				if (pLoopPlot == NULL)
				{
					continue;
				}
				CvWString szPlot = CvWString::format(L"%d,%d", pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
				if (pLoopPlot->getBonusType() != NO_BONUS)
				{
					szPlot += CvWString::format(L" %s", GC.getBonusInfo(pLoopPlot->getBonusType()).getDescription());
				}
				if (pLoopPlot->getImprovementType() != NO_IMPROVEMENT)
				{
					szPlot += CvWString::format(L" %s", GC.getImprovementInfo(pLoopPlot->getImprovementType()).getDescription());
				}
				if (pLoopPlot->getFeatureType() != NO_FEATURE)
				{
					szPlot += CvWString::format(L" %s", GC.getFeatureInfo(pLoopPlot->getFeatureType()).getDescription());
				}
				const int iKey = GC.getMap().plotNumINLINE(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE());
				CvWString szJobs;
				for (int j = 0; j < iNumJobs; ++j)
				{
					const WfJob& kJob = aJobs[j];
					if (kJob.iCity != c || kJob.iPlotKey != iKey)
					{
						continue;
					}
					const UnitTypes eExpert = (kJob.eExpertClass != NO_UNITCLASS) ? kCiv.getCivilizationUnits(kJob.eExpertClass) : NO_UNIT;
					const int iExpertOutput = (eExpert != NO_UNIT) ? wfOutput(kCvCity, kJob.eProfession, eExpert, pLoopPlot) : 0;
					szJobs += CvWString::format(L" | %s %d (%d food) %d gold, expert %d", GC.getProfessionInfo(kJob.eProfession).getDescription(),
						aiOutput[j], aiFood[j], (aiGold[j] + (aiFood[j] - iConsumption) * WF_FOOD_GOLD) / 100, iExpertOutput);
					if (kJob.iHolder >= 0)
					{
						szJobs += CvWString::format(L", planned %s", aWorkers[kJob.iHolder].pUnit->getName().GetCString());
					}
					if (kJob.iSeated >= 0)
					{
						szJobs += CvWString::format(L", worked by %s", aWorkers[kJob.iSeated].pUnit->getName().GetCString());
					}
				}
				if (szJobs.empty())
				{
					const CvCity* const pOther = pLoopPlot->getWorkingCity();
					szJobs = (pOther != NULL && pOther != &kCvCity) ? CvWString::format(L" | no job: belongs to %s", pOther->getName().GetCString())
						: !kCvCity.canWork(pLoopPlot) ? CvWString(L" | no job: cannot be worked") : CvWString(L" | no job: nothing to produce");
				}
				else
				{
					// professions without a job here: a plot offers the best food, the best other (by expert value) and the best
					// lumber profession only
					CvWString szLost;
					for (uint i = 0; i < aePlotProfessions.size(); ++i)
					{
						const ProfessionTypes eProfession = aePlotProfessions[i];
						bool bOffered = false;
						for (int j = 0; j < iNumJobs && !bOffered; ++j)
						{
							bOffered = aJobs[j].iCity == c && aJobs[j].iPlotKey == iKey && aJobs[j].eProfession == eProfession;
						}
						const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
						const YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
						if (bOffered || eYield == YIELD_FOOD || eYield == YIELD_LUMBER)
						{
							continue;
						}
						const UnitClassTypes eExpertClass = (UnitClassTypes)kProfession.LbD_getExpert();
						const UnitTypes eExpert = (eExpertClass != NO_UNITCLASS) ? kCiv.getCivilizationUnits(eExpertClass) : NO_UNIT;
						const int iOutput = wfOutput(kCvCity, eProfession, eExpert != NO_UNIT ? eExpert : eColonistUnit, pLoopPlot);
						if (iOutput > 0)
						{
							szLost += CvWString::format(L" %s %d (%d gold)", kProfession.getDescription(), iOutput,
								(wfYieldGold(kCvCity, eYield, iOutput, 0) + wfFoodOutput(eProfession, iOutput) * WF_FOOD_GOLD) / 100);
						}
					}
					if (!szLost.empty())
					{
						szJobs += L" | not offered (expert output):" + szLost;
					}
				}
				logWTP(WTP_LOG_DIAG, getID(), "diag %S plot %S%S", kCvCity.getName().GetCString(), szPlot.GetCString(), szJobs.GetCString());
			}
		}
		// colonists: plan, seat and the two best other jobs (value per travel time as the plan counts it, without pairs and
		// priorities)
		for (int w = 0; w < iNumWorkers; ++w)
		{
			const WfWorker& kWorker = aWorkers[w];
			const CvUnit& kUnit = *kWorker.pUnit;
			CvWString szWhere = (kWorker.pPopCity != NULL) ? CvWString::format(L"citizen of %s", kWorker.pPopCity->getName().GetCString())
				: (kWorker.pAtCity != NULL) ? CvWString::format(L"waits in %s", kWorker.pAtCity->getName().GetCString())
				: (kUnit.getUnitTravelState() == UNIT_TRAVEL_STATE_LIVE_AMONG_NATIVES) ? CvWString(L"in a village")
				: !kUnit.isOnMap() ? CvWString(kUnit.isCargo() ? L"on a ship off the map" : L"in Europe")
				: CvWString::format(L"%sat %d,%d", kUnit.isCargo() ? L"on a ship " : L"", kUnit.getX_INLINE(), kUnit.getY_INLINE());
			CvWString szPlan = L"no job";
			if (kWorker.iJob >= 0)
			{
				const WfJob& kJob = aJobs[kWorker.iJob];
				const int iT = WF_TYPE(w, kWorker.iJob);
				szPlan = CvWString::format(L"%s in %s %d,%d (%d gold, %d turns)%s%s", GC.getProfessionInfo(kJob.eProfession).getDescription(),
					kJob.pCity->getName().GetCString(), kJob.pPlot != NULL ? kJob.pPlot->getX_INLINE() : -1, kJob.pPlot != NULL ? kJob.pPlot->getY_INLINE() : -1,
					(aiGold[iT * iNumJobs + kWorker.iJob] + (aiFood[iT * iNumJobs + kWorker.iJob] - iConsumption) * WF_FOOD_GOLD) / 100,
					aiEta[w * iNumCities + kJob.iCity], kWorker.bReserved ? L", reserved" : L"", kWorker.bLearning ? L", learning" : L"");
			}
			CvWString szSeat;
			if (kWorker.iSeatCity >= 0)
			{
				for (int j = 0; j < iNumJobs; ++j)
				{
					if (aJobs[j].iSeated == w && j != kWorker.iJob)
					{
						szSeat = CvWString::format(L"; works %s %d,%d meanwhile", GC.getProfessionInfo(aJobs[j].eProfession).getDescription(),
							aJobs[j].pPlot != NULL ? aJobs[j].pPlot->getX_INLINE() : -1, aJobs[j].pPlot != NULL ? aJobs[j].pPlot->getY_INLINE() : -1);
					}
				}
			}
			else if (kWorker.iJob >= 0)
			{
				szSeat = L"; not seated";
			}
			int aiBest[2] = { -1, -1 };
			int aiBestValue[2] = { MIN_INT, MIN_INT };
			for (int j = 0; j < iNumJobs; ++j)
			{
				const int iT = WF_TYPE(w, j);
				const int iEta = aiEta[w * iNumCities + aJobs[j].iCity];
				if (j == kWorker.iJob || iEta >= 1000 || aiGold[iT * iNumJobs + j] <= -1000000 || aiOutput[iT * iNumJobs + j] <= 0)
				{
					continue;
				}
				const int iValue = (aiGold[iT * iNumJobs + j] + (aiFood[iT * iNumJobs + j] - iConsumption) * WF_FOOD_GOLD) * 20 / (20 + iEta);
				if (iValue > aiBestValue[0])
				{
					aiBest[1] = aiBest[0];
					aiBestValue[1] = aiBestValue[0];
					aiBest[0] = j;
					aiBestValue[0] = iValue;
				}
				else if (iValue > aiBestValue[1])
				{
					aiBest[1] = j;
					aiBestValue[1] = iValue;
				}
			}
			CvWString szOther;
			for (int k = 0; k < 2; ++k)
			{
				if (aiBest[k] < 0)
				{
					continue;
				}
				const WfJob& kJob = aJobs[aiBest[k]];
				szOther += CvWString::format(L"%s %s in %s %d,%d %d gold%s%s", k > 0 ? L";" : L"", GC.getProfessionInfo(kJob.eProfession).getDescription(),
					kJob.pCity->getName().GetCString(), kJob.pPlot != NULL ? kJob.pPlot->getX_INLINE() : -1, kJob.pPlot != NULL ? kJob.pPlot->getY_INLINE() : -1,
					aiBestValue[k] / 100, kJob.iHolder >= 0 ? L" planned for " : L" (free)",
					kJob.iHolder >= 0 ? aWorkers[kJob.iHolder].pUnit->getName().GetCString() : L"");
			}
			logWTP(WTP_LOG_DIAG, getID(), "diag colonist %S (%S): plan %S%S; best others: %S", kUnit.getName().GetCString(), szWhere.GetCString(),
				szPlan.GetCString(), szSeat.GetCString(), szOther.GetCString());
		}
		for (uint i = 0; i < aiStandWorker.size(); ++i)
		{
			const WfJob& kTo = aJobs[aiStandTo[i]];
			logWTP(WTP_LOG_DIAG, getID(), "diag %S stand-in: %S leaves %S (%d gold) for %S %d,%d (%d gold) of %S, who is %d turns away",
				kTo.pCity->getName().GetCString(), aWorkers[aiStandWorker[i]].pUnit->getName().GetCString(),
				aiStandFrom[i] >= 0 ? CvWString(GC.getProfessionInfo(aJobs[aiStandFrom[i]].eProfession).getDescription()).GetCString() : L"nothing",
				aiStandOld[i] / 100, GC.getProfessionInfo(kTo.eProfession).getDescription(), kTo.pPlot != NULL ? kTo.pPlot->getX_INLINE() : -1,
				kTo.pPlot != NULL ? kTo.pPlot->getY_INLINE() : -1, aiStandNew[i] / 100, aWorkers[kTo.iHolder].pUnit->getName().GetCString(),
				aiEta[kTo.iHolder * iNumCities + kTo.iCity]);
		}
	}
	#undef WF_TYPE
	#undef WF_ASSIGN
	#undef WF_IS_FREE_JOB
	#undef WF_GROUP_FREE
	#undef WF_SEAT_KEY
}

// WTP: a colony with a food surplus below 2 and a free food plot, if no new colonies are wanted
CvCity* CvPlayerAI::AI_bestFoodWorkerCity(bool& bWater)
{
	bWater = false;
	// WTP: the workforce plan names the colony whose jobs lack food most
	if (AI_isWorkforcePlanned())
	{
		bWater = m_bWfFoodWater;
		return getCity(m_iWfFoodCityID);
	}
	if (getNumCities() == 0 || AI_desiredCityCount() > getNumCities() + AI_totalUnitAIs(UNITAI_SETTLER))
	{
		return NULL;
	}
	CvCity* pBestCity = NULL;
	int iBestShortage = 0;
	int iLoop;
	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		const int iShortage = 2 - pCity->foodDifference();
		if (iShortage <= iBestShortage)
		{
			continue;
		}
		bool bFreeLand = false;
		bool bFreeWater = false;
		FOREACH(CityPlot)
		{
			const CvPlot* const pLoopPlot = pCity->getCityIndexPlot(eLoopCityPlot);
			if (pLoopPlot == NULL || pLoopPlot == pCity->plot() || pLoopPlot->isBeingWorked() || !pCity->canWork(pLoopPlot)
				|| pLoopPlot->calculateBestNatureYield(YIELD_FOOD, getTeam()) < 3)
			{
				continue;
			}
			if (pLoopPlot->isWater())
			{
				bFreeWater = true;
			}
			else
			{
				bFreeLand = true;
			}
		}
		if (bFreeLand || bFreeWater)
		{
			iBestShortage = iShortage;
			pBestCity = pCity;
			bWater = !bFreeLand;
		}
	}
	return pBestCity;
}

// WTP: African slaves are good farm workers; NO_UNIT if they cannot be bought
UnitTypes CvPlayerAI::AI_africaFoodWorker()
{
	if (!canTradeWithAfrica())
	{
		return NO_UNIT;
	}
	const UnitClassTypes eSlaveClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_AFRICAN_SLAVE");
	const UnitTypes eSlave = (eSlaveClass != NO_UNITCLASS) ? GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eSlaveClass) : NO_UNIT;
	if (eSlave == NO_UNIT || getAfricaUnitBuyPrice(eSlave) <= 0)
	{
		return NO_UNIT;
	}
	return eSlave;
}

// WTP: a colony needs a farm worker and a slave from Africa is cheaper than an expert farmer from Europe
bool CvPlayerAI::AI_wantsAfricaFoodWorkers()
{
	// WTP: the workforce plan buys its African slaves itself and a transport fetches them (AI_sailForEuropeBacklog)
	if (AI_isWorkforcePlanned())
	{
		return false;
	}
	// cheap checks first, this is asked by every transport choosing a port
	const UnitTypes eSlave = AI_africaFoodWorker();
	if (eSlave == NO_UNIT || !AI_canAffordCivilianPurchase(getAfricaUnitBuyPrice(eSlave)))
	{
		return false;
	}
	const UnitClassTypes eFarmerClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FARMER");
	const UnitTypes eFarmer = (eFarmerClass != NO_UNITCLASS) ? GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eFarmerClass) : NO_UNIT;
	if (eFarmer != NO_UNIT && getEuropeUnitBuyPrice(eFarmer) > 0 && getAfricaUnitBuyPrice(eSlave) >= getEuropeUnitBuyPrice(eFarmer))
	{
		return false;
	}
	bool bWater;
	const CvCity* const pCity = AI_bestFoodWorkerCity(bWater);
	return pCity != NULL && !bWater;
}

// WTP: number of own colonies the ship can enter (adjacent water it may sail that is reachable from the ocean)
int CvPlayerAI::AI_shipReachableColonies(UnitTypes eUnit) const
{
	int iCount = 0;
	int iLoop;
	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		if (AI_shipCanReachCity(eUnit, *pCity))
		{
			++iCount;
		}
	}
	return iCount;
}

// WTP: a ship type can reach a colony if it may enter one of the colony's water plots that is accessible from Europe
// (galleons and other big ships do not enter shallow coast, most ships no large rivers)
bool CvPlayerAI::AI_shipCanReachCity(UnitTypes eUnit, const CvCity& kCity)
{
	const CvUnitInfo& kUnit = GC.getUnitInfo(eUnit);
	for (int iI = 0; iI < NUM_DIRECTION_TYPES; ++iI)
	{
		const CvPlot* const pAdjacent = plotDirection(kCity.getX_INLINE(), kCity.getY_INLINE(), (DirectionTypes)iI);
		if (pAdjacent != NULL && pAdjacent->isWater() && pAdjacent->isEuropeAccessable() && !kUnit.getTerrainImpassable(pAdjacent->getTerrainType()))
		{
			return true;
		}
	}
	return false;
}

// WTP: a colony with restricted water access (shallow coast or large river only) is not reached by all of our
// transports; the transports that do reach it give it priority
bool CvPlayerAI::AI_isRestrictedWaterCity(const CvCity& kCity) const
{
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA && !AI_shipCanReachCity(pLoopUnit->getUnitType(), kCity))
		{
			return true;
		}
	}
	return false;
}

// WTP: best ship of a unit AI to buy in Europe. It must reach as many colonies as the best reaching ship of that
// unit AI (all coastal ones if possible), then the cheapest one or the one with the best value per price is taken.
UnitTypes CvPlayerAI::AI_bestShipToBuy(UnitAITypes eUnitAI, bool bCheapest, TerrainTypes eRequiredTerrain)
{
	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	int iBestReach = 0;
	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		const UnitTypes eLoopUnit = kCiv.getCivilizationUnits(eUnitClass);
		if (eLoopUnit != NO_UNIT && GC.getUnitInfo(eLoopUnit).getDefaultUnitAIType() == eUnitAI && isUnitWithinGameYearWindow(eLoopUnit)
			&& getEuropeUnitBuyPrice(eLoopUnit) > 0
			&& (eRequiredTerrain == NO_TERRAIN || !GC.getUnitInfo(eLoopUnit).getTerrainImpassable(eRequiredTerrain)))
		{
			iBestReach = std::max(iBestReach, AI_shipReachableColonies(eLoopUnit));
		}
	}

	int iMaxMoves = 1;
	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		const UnitTypes eLoopUnit = kCiv.getCivilizationUnits(eUnitClass);
		if (eLoopUnit != NO_UNIT && GC.getUnitInfo(eLoopUnit).getDefaultUnitAIType() == eUnitAI && isUnitWithinGameYearWindow(eLoopUnit)
			&& getEuropeUnitBuyPrice(eLoopUnit) > 0)
		{
			iMaxMoves = std::max(iMaxMoves, GC.getUnitInfo(eLoopUnit).getMoves());
		}
	}

	UnitTypes eBestShip = NO_UNIT;
	int iBestValue = 0;
	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		const UnitTypes eLoopUnit = kCiv.getCivilizationUnits(eUnitClass);
		if (eLoopUnit == NO_UNIT)
		{
			continue;
		}
		const CvUnitInfo& kUnitInfo = GC.getUnitInfo(eLoopUnit);
		if (kUnitInfo.getDefaultUnitAIType() != eUnitAI || !isUnitWithinGameYearWindow(eLoopUnit) || kUnitInfo.isHiddenNationality()
			|| (eRequiredTerrain != NO_TERRAIN && kUnitInfo.getTerrainImpassable(eRequiredTerrain)))
		{
			continue;
		}
		const int iPrice = getEuropeUnitBuyPrice(eLoopUnit);
		if (iPrice <= 0 || AI_shipReachableColonies(eLoopUnit) < iBestReach)
		{
			continue;
		}
		int iValue = bCheapest ? (1000000 / iPrice) : (1000 * AI_unitGoldValue(eLoopUnit, eUnitAI, NULL) / iPrice);
		// WTP: transports: the most cargo per gold, weighted with 50 % of the speed (moves relative to the fastest)
		if (!bCheapest && eUnitAI == UNITAI_TRANSPORT_SEA)
		{
			iValue = 1000000 * kUnitInfo.getCargoSpace() / iPrice * (50 + 50 * kUnitInfo.getMoves() / std::max(1, iMaxMoves)) / 100;
		}
		if (iValue > iBestValue)
		{
			iBestValue = iValue;
			eBestShip = eLoopUnit;
		}
	}
	return eBestShip;
}

// WTP: treasures can only be carried by a galleon; from 5 treasures on the map without such a ship the AI buys one
// (cheapest ship that can carry them). Returns true while saving gold for it, then nothing else is bought.
bool CvPlayerAI::AI_doTreasureShipPurchase()
{
	int iTreasures = 0;
	const UnitTypes eShip = AI_treasureShipToBuy(&iTreasures);
	if (eShip == NO_UNIT)
	{
		return false;
	}
	const int iBestPrice = getEuropeUnitBuyPrice(eShip);
	if (getGold() < iBestPrice)
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) saves for %S (%d gold, have %d): %d treasures wait for pickup",
			getID(), getCivilizationDescription(0), GC.getUnitInfo(eShip).getDescription(), iBestPrice, getGold(), iTreasures);
		return true;
	}
	CvUnit* const pUnit = buyEuropeUnit(eShip, 100);
	if (pUnit != NULL)
	{
		pUnit->AI_setUnitAIType(UNITAI_TRANSPORT_SEA);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) buys %S for %d treasures waiting for pickup",
			getID(), getCivilizationDescription(0), pUnit->getName().GetCString(), iTreasures);
	}
	return false;
}

// WTP: the ship to buy for the treasures (NO_UNIT if none is needed); while it is needed the gold is saved for it
UnitTypes CvPlayerAI::AI_treasureShipToBuy(int* piTreasures) const
{
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || !canTradeWithEurope())
	{
		return NO_UNIT;
	}
	int iTreasures = 0;
	int iSize = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->AI_getUnitAIType() == UNITAI_TREASURE && pLoopUnit->isOnMap())
		{
			++iTreasures;
			iSize = std::max(iSize, pLoopUnit->getUnitInfo().getRequiredTransportSize());
		}
	}
	if (piTreasures != NULL)
	{
		*piTreasures = iTreasures;
	}
	if (iTreasures < 5)
	{
		return NO_UNIT;
	}
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getDomainType() == DOMAIN_SEA && pLoopUnit->cargoSpace() >= iSize)
		{
			return NO_UNIT;
		}
	}
	for (int i = 0; i < getNumEuropeUnits(); ++i)
	{
		const CvUnit* const pLoopUnit = getEuropeUnit(i);
		if (pLoopUnit != NULL && pLoopUnit->getDomainType() == DOMAIN_SEA && pLoopUnit->cargoSpace() >= iSize)
		{
			return NO_UNIT;
		}
	}

	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	UnitTypes eShip = NO_UNIT;
	int iBestPrice = MAX_INT;
	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		const UnitTypes eLoopUnit = kCiv.getCivilizationUnits(eUnitClass);
		if (eLoopUnit == NO_UNIT)
		{
			continue;
		}
		const CvUnitInfo& kUnitInfo = GC.getUnitInfo(eLoopUnit);
		if (kUnitInfo.getDomainType() != DOMAIN_SEA || kUnitInfo.getDefaultUnitAIType() != UNITAI_TRANSPORT_SEA || kUnitInfo.getCargoSpace() < iSize
			|| kUnitInfo.isHiddenNationality() || !isUnitWithinGameYearWindow(eLoopUnit))
		{
			continue;
		}
		const int iPrice = getEuropeUnitBuyPrice(eLoopUnit);
		if (iPrice > 0 && iPrice < iBestPrice)
		{
			iBestPrice = iPrice;
			eShip = eLoopUnit;
		}
	}
	return eShip;
}

// WTP: native mercenaries come with the offensive unit AI; in peace they defend the colonies (and count as defenders)
void CvPlayerAI::AI_doNativeMercDefense()
{
	if (isHuman() || isNative() || isEurope() || GET_TEAM(getTeam()).getAnyWarPlanCount() > 0)
	{
		return;
	}
	const UnitTypes eMerc = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(UNITCLASS_NATIVE_MERC);
	if (eMerc == NO_UNIT)
	{
		return;
	}
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getUnitType() == eMerc && pLoopUnit->AI_getUnitAIType() == UNITAI_OFFENSIVE)
		{
			pLoopUnit->AI_setUnitAIType(UNITAI_DEFENSIVE);
			if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_MILITARY, getID(), "WTP decision: Player %d (%S) native mercenary %S defends the colonies in peace", getID(), getCivilizationDescription(0),
				pLoopUnit->getName().GetCString());
		}
	}
}

// WTP: with more than 3 pioneers above the need, one regular pioneer (never a hardy pioneer) goes to the nearest
// colony and joins it; the next one only after it has arrived
void CvPlayerAI::AI_doSurplusPioneers()
{
	if (isHuman() || isNative() || isEurope() || getNumCities() == 0)
	{
		return;
	}
	if (m_iSurplusPioneerID != -1)
	{
		const CvUnit* const pSent = getUnit(m_iSurplusPioneerID);
		if (pSent != NULL && getPopulationUnitCity(m_iSurplusPioneerID) == NULL && pSent->isOnMap() && pSent->AI_getUnitAIType() == UNITAI_WORKER
			&& getCity(m_iSurplusPioneerCityID) != NULL)
		{
			return; // still on its way
		}
		AI_clearSurplusPioneer();
	}
	const int iNeeded = AI_neededWorkers(NULL);
	const int iHave = AI_totalUnitAIs(UNITAI_WORKER);
	if (iHave <= iNeeded + 3)
	{
		return;
	}

	CvUnitAI* pBest = NULL;
	CvCity* pBestCity = NULL;
	int iBestTurns = MAX_INT;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		CvUnitAI* const pPioneer = static_cast<CvUnitAI*>(pLoopUnit);
		if (pPioneer->AI_getUnitAIType() != UNITAI_WORKER || !pPioneer->isOnMap() || pPioneer->isCargo() || !pPioneer->AI_mayPioneerJoinCity())
		{
			continue;
		}
		// nearest colony on the same landmass
		CvCity* pNearest = NULL;
		int iNearest = MAX_INT;
		int iCityLoop;
		for (CvCity* pLoopCity = firstCity(&iCityLoop); pLoopCity != NULL; pLoopCity = nextCity(&iCityLoop))
		{
			const int iDistance = plotDistance(pPioneer->getX_INLINE(), pPioneer->getY_INLINE(), pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE());
			if (pLoopCity->area() == pPioneer->area() && iDistance < iNearest)
			{
				iNearest = iDistance;
				pNearest = pLoopCity;
			}
		}
		if (pNearest == NULL || iNearest >= iBestTurns)
		{
			continue;
		}
		int iPathTurns = 0;
		if (pPioneer->atPlot(pNearest->plot()) || pPioneer->generatePath(pNearest->plot(), MOVE_NO_ENEMY_TERRITORY, true, &iPathTurns))
		{
			iBestTurns = iNearest;
			pBest = pPioneer;
			pBestCity = pNearest;
		}
	}
	if (pBest == NULL)
	{
		return;
	}
	m_iSurplusPioneerID = pBest->getID();
	m_iSurplusPioneerCityID = pBestCity->getID();
	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_SETTLER, getID(), "WTP decision: Player %d (%S) turn %d has %d pioneers, needs %d: %S goes to %S to join it", getID(), getCivilizationDescription(0),
		GC.getGameINLINE().getGameTurn(), iHave, iNeeded, pBest->getNameAndProfession().GetCString(), pBestCity->getName().GetCString());
}

CvCity* CvPlayerAI::AI_surplusPioneerCity(int iUnitID) const
{
	return (iUnitID == m_iSurplusPioneerID) ? getCity(m_iSurplusPioneerCityID) : NULL;
}

void CvPlayerAI::AI_clearSurplusPioneer()
{
	m_iSurplusPioneerID = -1;
	m_iSurplusPioneerCityID = -1;
}

// WTP: water access of a colony for ships: deep water (coast, ocean), only shallow coast, only a large river, or none
// (inland or a lake). Only water accessible from Europe counts.
int CvPlayerAI::AI_cityWaterAccess(const CvCity& kCity)
{
	bool bShallow = false;
	bool bRiver = false;
	for (int iI = 0; iI < NUM_DIRECTION_TYPES; ++iI)
	{
		const CvPlot* const pAdjacent = plotDirection(kCity.getX_INLINE(), kCity.getY_INLINE(), (DirectionTypes)iI);
		if (pAdjacent == NULL || !pAdjacent->isWater() || !pAdjacent->isEuropeAccessable())
		{
			continue;
		}
		const TerrainTypes eTerrain = pAdjacent->getTerrainType();
		if (eTerrain == TERRAIN_COAST || eTerrain == TERRAIN_OCEAN)
		{
			return WATER_ACCESS_DEEP;
		}
		if (eTerrain == TERRAIN_SHALLOW_COAST)
		{
			bShallow = true;
		}
		else if (eTerrain == TERRAIN_LARGE_RIVERS)
		{
			bRiver = true;
		}
	}
	return bShallow ? WATER_ACCESS_SHALLOW : (bRiver ? WATER_ACCESS_RIVER : WATER_ACCESS_NONE);
}

// WTP: the transport to buy. The transports that can enter large rivers have to match the colonies only reached over a
// large river, those that can enter shallow coast the colonies on shallow coast or a large river; if one of them is
// short, the next transport restores the balance (large rivers first). Otherwise the default choice.
UnitTypes CvPlayerAI::AI_transportToBuy()
{
	int iRiverColonies = 0;
	int iShallowColonies = 0;
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		const int iAccess = AI_cityWaterAccess(*pLoopCity);
		if (iAccess == WATER_ACCESS_RIVER)
		{
			++iRiverColonies;
		}
		else if (iAccess == WATER_ACCESS_SHALLOW)
		{
			++iShallowColonies;
		}
	}
	int iRiverShips = 0;
	int iShallowShips = 0;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getDomainType() == DOMAIN_SEA && pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA)
		{
			const CvUnitInfo& kInfo = pLoopUnit->getUnitInfo();
			if (!kInfo.getTerrainImpassable(TERRAIN_LARGE_RIVERS))
			{
				++iRiverShips;
			}
			if (!kInfo.getTerrainImpassable(TERRAIN_SHALLOW_COAST))
			{
				++iShallowShips;
			}
		}
	}

	UnitTypes eShip = NO_UNIT;
	if (iRiverShips < iRiverColonies)
	{
		eShip = AI_bestShipToBuy(UNITAI_TRANSPORT_SEA, false, TERRAIN_LARGE_RIVERS);
	}
	if (eShip == NO_UNIT && iShallowShips < iRiverColonies + iShallowColonies)
	{
		eShip = AI_bestShipToBuy(UNITAI_TRANSPORT_SEA, false, TERRAIN_SHALLOW_COAST);
	}
	if (eShip == NO_UNIT)
	{
		eShip = AI_bestShipToBuy(UNITAI_TRANSPORT_SEA, false);
	}
	return eShip;
}

// WTP: at least one transport per colony (inland colonies count too). Returns true while saving gold for it, then
// nothing else is bought.
bool CvPlayerAI::AI_doColonyTransportPurchase()
{
	if (getNumCities() == 0)
	{
		return false;
	}
	int iTransports = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getDomainType() == DOMAIN_SEA && pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA)
		{
			++iTransports;
		}
	}
	if (iTransports >= getNumCities())
	{
		return false;
	}
	const UnitTypes eShip = AI_transportToBuy();
	if (eShip == NO_UNIT)
	{
		return false;
	}
	const int iPrice = getEuropeUnitBuyPrice(eShip);
	if (getGold() < iPrice)
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) saves for transport %S (%d gold, have %d): %d transports for %d colonies",
			getID(), getCivilizationDescription(0), GC.getUnitInfo(eShip).getDescription(), iPrice, getGold(), iTransports, getNumCities());
		return true;
	}
	CvUnit* const pUnit = buyEuropeUnit(eShip, 100);
	if (pUnit != NULL)
	{
		pUnit->AI_setUnitAIType(UNITAI_TRANSPORT_SEA);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) buys transport %S: %d transports for %d colonies",
			getID(), getCivilizationDescription(0), pUnit->getName().GetCString(), iTransports, getNumCities());
	}
	return false;
}

// WTP: missing pioneers are equipped in Europe: free colonists and indentured servants on the docks without a task of
// the workforce plan, and experts whose specialty is useless anyway. Free colonists come first.
void CvPlayerAI::AI_doEquipPioneersInEurope()
{
	if (isHuman() || isNative() || isEurope() || AI_isStrategy(STRATEGY_MILITARY_BUILDUP))
	{
		return;
	}
	const ProfessionTypes eDefaultProfession = GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession();
	const ProfessionTypes ePioneer = AI_idealProfessionForUnitAIType(UNITAI_WORKER);
	if (ePioneer == NO_PROFESSION)
	{
		return;
	}
	// not while the AI saves for a transport, the militia or the treasure galleon (the equipment costs gold)
	if (AI_totalUnitAIs(UNITAI_WORKER) < AI_neededWorkers(NULL) && AI_isSavingGold())
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) equips no pioneer in Europe while saving gold (%d pioneers, %d needed)", getID(),
			getCivilizationDescription(0), AI_totalUnitAIs(UNITAI_WORKER), AI_neededWorkers(NULL));
		return;
	}
	std::vector<int> aiYieldAvailable(NUM_YIELD_TYPES, -1);
	while (AI_totalUnitAIs(UNITAI_WORKER) < AI_neededWorkers(NULL))
	{
		CvUnit* pBest = NULL;
		int iBestValue = 0;
		for (int i = 0; i < getNumEuropeUnits(); ++i)
		{
			CvUnit* const pUnit = getEuropeUnit(i);
			if (pUnit == NULL || pUnit->getProfession() != eDefaultProfession || !pUnit->canHaveProfession(ePioneer, false))
			{
				continue;
			}
			const CvUnitAI& kUnit = static_cast<const CvUnitAI&>(*pUnit);
			const UnitAITypes eUnitAI = kUnit.AI_getUnitAIType();
			if (kUnit.AI_getWfTask() != CvUnitAI::WF_NONE || eUnitAI == UNITAI_SETTLER || eUnitAI == UNITAI_WORKER)
			{
				continue;
			}
			int iValue = 0;
			const UnitClassTypes eClass = pUnit->getUnitClassType();
			if (eClass == UNITCLASS_COLONIST)
			{
				iValue = 3;
			}
			else if (eClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_INDENTURED_SERVANT"))
			{
				iValue = 2;
			}
			else if (kUnit.AI_getIdealProfession() != NO_PROFESSION && pUnit->canClearSpecialty() && pUnit->getUnitInfo().NBMOD_GetTeachLevel() < 3
				&& AI_isUselessExpert(kUnit, aiYieldAvailable))
			{
				iValue = 1;
			}
			if (iValue > iBestValue)
			{
				iBestValue = iValue;
				pBest = pUnit;
			}
		}
		if (pBest == NULL)
		{
			return;
		}
		changeProfessionEurope(pBest->getID(), ePioneer);
		if (pBest->getProfession() != ePioneer)
		{
			return; // not affordable
		}
		pBest->AI_setUnitAIType(UNITAI_WORKER);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) equips %S as pioneer in Europe (%d pioneers, %d needed)", getID(), getCivilizationDescription(0),
			pBest->getName().GetCString(), AI_totalUnitAIs(UNITAI_WORKER), AI_neededWorkers(NULL));
	}
}

// WTP: buy the colonist of the workforce plan with the best gain per price (see AI_doWorkforcePlan, step 6b).
// Returns true if an opportunity was chosen (bought or not affordable), false if there is none.
// WTP: more units wait in Europe than our transports take over in two trips: no colonists are bought or hurried (the
// Swedes had 15 units in Europe and 2-slot caravels only)
bool CvPlayerAI::AI_isEuropeBacklog() const
{
	int iRoom = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA)
		{
			iRoom += pLoopUnit->cargoSpace();
		}
	}
	return getNumEuropeUnits() > 2 * iRoom;
}

bool CvPlayerAI::AI_buyExpertForOpportunity()
{
	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	// (no colonist from Europe while units wait there for a ship; Africa is fetched by a ship of its own)
	const bool bEuropeBacklog = AI_isEuropeBacklog();
	// WTP: the colonist of the last workforce plan with the best gain per price (an expert for a job, or a food provider).
	// It gets the job as its task at once: that is the job's reservation, nobody buys it again while it is on its way.
	int iBest = -1;
	int iBestRatio = 0;
	for (uint i = 0; i < m_aWfOpenOpportunities.size(); ++i)
	{
		const WfOpenOpportunity& kOpp = m_aWfOpenOpportunities[i];
		const int iRatio = kOpp.iValue * 100 / std::max(1, kOpp.iPrice);
		// the same expert for the same colony at most every 15 turns (it may still be on its way)
		const std::map<int, int>::const_iterator itRecent = m_mapWfRecentBuy.find(kOpp.iCityID * 10000 + (int)kOpp.eExpertClass);
		if ((itRecent != m_mapWfRecentBuy.end() && GC.getGameINLINE().getGameTurn() - itRecent->second < 15) || (bEuropeBacklog && !kOpp.bAfrica))
		{
			continue;
		}
		if (getCity(kOpp.iCityID) != NULL && kCiv.getCivilizationUnits(kOpp.eExpertClass) != NO_UNIT && iRatio > iBestRatio)
		{
			iBestRatio = iRatio;
			iBest = i;
		}
	}
	if (iBest < 0)
	{
		if (bEuropeBacklog && gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   no colonist bought: %d units wait in Europe for a ship", getNumEuropeUnits());
		return false;
	}
	const WfOpenOpportunity kOpp = m_aWfOpenOpportunities[iBest];
	CvCity* const pCity = getCity(kOpp.iCityID);
	const UnitTypes eUnit = kCiv.getCivilizationUnits(kOpp.eExpertClass);
	const int iPrice = kOpp.bAfrica ? getAfricaUnitBuyPrice(eUnit) : getEuropeUnitBuyPrice(eUnit);
	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   %S for %S in %S%s: gain %d gold per turn, price %d (gold %d)", GC.getUnitClassInfo(kOpp.eExpertClass).getDescription(),
		GC.getProfessionInfo(kOpp.eProfession).getDescription(), pCity->getName().GetCString(), (kOpp.iPlotIndex >= 0) ? " (plot)" : "", kOpp.iValue / 100, iPrice, getGold());
	if (!AI_canAffordCivilianPurchase(iPrice))
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   cannot afford it now");
		return true;
	}
	CvUnit* const pUnit = kOpp.bAfrica ? buyAfricaUnit(eUnit, 100) : buyEuropeUnit(eUnit, 100);
	if (pUnit == NULL)
	{
		return true;
	}
	pUnit->AI_setUnitAIType(UNITAI_COLONIST);
	const CvPlot* const pPlot = (kOpp.iPlotIndex >= 0) ? GC.getMap().plotByIndexINLINE(kOpp.iPlotIndex) : NULL;
	static_cast<CvUnitAI*>(pUnit)->AI_setWfTask(CvUnitAI::WF_EXPERT_JOB, pCity->getID(), kOpp.eProfession,
		pPlot != NULL ? pPlot->getX_INLINE() : -1, pPlot != NULL ? pPlot->getY_INLINE() : -1, -1, -1);
	if (gPlayerLogLevel >= 1) logWTPCity(*pCity, "WTP plan: %S bought in %s for %S in %S at %d,%d (gain %d gold per turn, price %d)", pUnit->getName().GetCString(), kOpp.bAfrica ? "Africa" : "Europe",
		GC.getProfessionInfo(kOpp.eProfession).getDescription(), pCity->getName().GetCString(), pPlot != NULL ? pPlot->getX_INLINE() : -1, pPlot != NULL ? pPlot->getY_INLINE() : -1,
		kOpp.iValue / 100, iPrice);
	m_mapWfRecentBuy[kOpp.iCityID * 10000 + (int)kOpp.eExpertClass] = GC.getGameINLINE().getGameTurn();
	m_aWfOpenOpportunities.erase(m_aWfOpenOpportunities.begin() + iBest);
	return true;
}

// WTP: while the AI saves for a transport, the colonial militia or the treasure galleon, colonies do not hurry (the
// purchases are made in the units' turn, the hurrying in the colonies' turn of the next turn)
void CvPlayerAI::AI_setSavingGold()
{
	m_iSavingGoldTurn = GC.getGameINLINE().getGameTurn();
}

bool CvPlayerAI::AI_isSavingGold() const
{
	return m_iSavingGoldTurn >= GC.getGameINLINE().getGameTurn() - 1 || AI_treasureShipToBuy() != NO_UNIT;
}

// WTP: early game. Phase 0 lasts from the start until the first scout is bought: only the native trade ship spends
// money (and the scout is bought). Then every AI gets one of five early strategies (random for now) that buy their
// things one after the other; of the buildings only the Town Hall (Early Improvement: the Village Hall too) may be hurried,
// no immigrants and no African slaves are bought. When the purchases of the strategy are made the early game is over
// and the usual rules apply again. The gold reserve of the civilian purchases does not apply to these purchases.
bool CvPlayerAI::AI_isEarlyPhaseActive() const
{
	// the turn limit keeps a game without Europe trade (or a stalled strategy) from being locked for good
	return !isHuman() && !isNative() && !isEurope() && getParent() != NO_PLAYER && m_iEarlyPhase < 2 && GC.getGameINLINE().getGameTurn() <= 150;
}

// WTP: the Town Hall may be hurried in the early game (the Village Hall under it with Early Improvement), nothing before the first scout
bool CvPlayerAI::AI_mayHurryBuildingEarly(BuildingTypes eBuilding) const
{
	if (!AI_isEarlyPhaseActive())
	{
		return true;
	}
	if (m_iEarlyPhase == 0)
	{
		return false;
	}
	const BuildingClassTypes eClass = (BuildingClassTypes)GC.getBuildingInfo(eBuilding).getBuildingClassType();
	if (eClass == (BuildingClassTypes)GC.getInfoTypeForString("BUILDINGCLASS_TOWNHALL"))
	{
		return true;
	}
	return m_iEarlyStrategy == EARLY_IMPROVEMENT && eClass == (BuildingClassTypes)GC.getInfoTypeForString("BUILDINGCLASS_VILLAGEHALL");
}

void CvPlayerAI::AI_startEarlyStrategy()
{
	static const char* const aszNames[NUM_EARLY_STRATEGIES] = { "none", "Explorer", "Native Trader", "Ship Trader", "Early Improvement", "Opportunistic Resources" };
	// until further notice every strategy is used once among the AIs (randomly among the unused ones), all further AIs get
	// Opportunistic Resources
	bool abUsed[NUM_EARLY_STRATEGIES];
	for (int i = 0; i < NUM_EARLY_STRATEGIES; ++i)
	{
		abUsed[i] = false;
	}
	for (PlayerTypes ePlayer = FIRST_PLAYER; ePlayer < NUM_PLAYER_TYPES; ++ePlayer)
	{
		const CvPlayerAI& kOther = GET_PLAYER(ePlayer);
		if (ePlayer != getID() && kOther.isAlive() && !kOther.isHuman() && !kOther.isNative() && !kOther.isEurope() && kOther.m_iEarlyStrategy > 0 && kOther.m_iEarlyStrategy < NUM_EARLY_STRATEGIES)
		{
			abUsed[kOther.m_iEarlyStrategy] = true;
		}
	}
	std::vector<int> aiUnused;
	for (int i = 1; i < NUM_EARLY_STRATEGIES; ++i)
	{
		if (!abUsed[i])
		{
			aiUnused.push_back(i);
		}
	}
	m_iEarlyStrategy = aiUnused.empty() ? (int)EARLY_RESOURCES : aiUnused[GC.getGameINLINE().getSorenRandNum((int)aiUnused.size(), "AI early strategy")];
	m_iEarlyPhase = 1;
	m_iEarlyProgress = 0;
	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP early: Player %d (%S) turn %d: phase 0 is over (%d scouts bought), early strategy %s, gold %d",
		getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), m_iEarlyScoutsBought, aszNames[m_iEarlyStrategy], getGold());
}

void CvPlayerAI::AI_earlyEnd()
{
	m_iEarlyPhase = 2;
	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP early: Player %d (%S) turn %d: the purchases of the early strategy %d are made, the early game is over, gold %d",
		getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), m_iEarlyStrategy, getGold());
}

// WTP: cost of the equipment a profession needs on top of the colonist (bought in Europe)
int CvPlayerAI::AI_earlyEquipCost(ProfessionTypes eProfession) const
{
	int iCost = 0;
	for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
	{
		iCost += GET_PLAYER(getParent()).getYieldSellPrice(eYield) * GC.getProfessionInfo(eProfession).getYieldEquipmentAmount(eYield);
	}
	return iCost;
}

// WTP: hurry the cheapest immigrant on the docks (any kind). False if none can be hurried or the gold does not suffice (the AI saves then).
bool CvPlayerAI::AI_earlyHurryDockUnit()
{
	int iBestIndex = -1;
	int iBestCost = MAX_INT;
	for (unsigned int iIndex = 0; iIndex < getNumUnitsOnDock(); ++iIndex)
	{
		if (getDocksNextUnit(iIndex) != NO_UNIT && canHurry(HURRY_IMMIGRANT, iIndex) && getHurryGold(HURRY_IMMIGRANT, iIndex) < iBestCost)
		{
			iBestCost = getHurryGold(HURRY_IMMIGRANT, iIndex);
			iBestIndex = iIndex;
		}
	}
	if (iBestIndex < 0)
	{
		return false;
	}
	if (iBestCost > getGold())
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) saves to hurry an immigrant (%d gold, have %d)", getID(), getCivilizationDescription(0), iBestCost, getGold());
		return false;
	}
	const UnitTypes eHurried = getDocksNextUnit(iBestIndex);
	hurry(HURRY_IMMIGRANT, iBestIndex);
	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) turn %d hurries an immigrant (%S) for %d gold, gold now %d",
		getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), GC.getUnitInfo(eHurried).getDescription(), iBestCost, getGold());
	return true;
}

// WTP: a free colonist or indentured servant in Europe: the cheaper of hurrying such an immigrant on the docks and buying a free
// colonist. iExtraCost (equipment, ...) must be affordable as well, else the AI saves and nothing is bought (NULL).
CvUnit* CvPlayerAI::AI_earlyGetColonist(int iExtraCost)
{
	const UnitTypes eColonist = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits((UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_COLONIST"));
	const int iColonistPrice = (eColonist != NO_UNIT && getEuropeUnitBuyPrice(eColonist) > 0) ? getEuropeUnitBuyPrice(eColonist) : MAX_INT;
	const UnitClassTypes eServantClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_INDENTURED_SERVANT");

	int iBestIndex = -1;
	int iBestCost = MAX_INT;
	for (unsigned int iIndex = 0; iIndex < getNumUnitsOnDock(); ++iIndex)
	{
		const UnitTypes eDock = getDocksNextUnit(iIndex);
		if (eDock == NO_UNIT || !canHurry(HURRY_IMMIGRANT, iIndex))
		{
			continue;
		}
		const UnitClassTypes eClass = (UnitClassTypes)GC.getUnitInfo(eDock).getUnitClassType();
		if ((eClass == UNITCLASS_COLONIST || eClass == eServantClass) && getHurryGold(HURRY_IMMIGRANT, iIndex) < iBestCost)
		{
			iBestCost = getHurryGold(HURRY_IMMIGRANT, iIndex);
			iBestIndex = iIndex;
		}
	}

	const bool bHurry = iBestIndex >= 0 && iBestCost <= iColonistPrice;
	const int iCost = bHurry ? iBestCost : iColonistPrice;
	if (iCost == MAX_INT)
	{
		return NULL;
	}
	if (getGold() < iCost + iExtraCost)
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) saves for a colonist (%d gold plus %d for the equipment, have %d)", getID(), getCivilizationDescription(0), iCost, iExtraCost, getGold());
		return NULL;
	}
	if (!bHurry)
	{
		CvUnit* const pBought = buyEuropeUnit(eColonist, 100);
		if (pBought != NULL && gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) turn %d buys a free colonist for %d gold", getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), iCost);
		return pBought;
	}
	// the hurried immigrant is the one on the docks that was not there before
	std::vector<int> aiBefore;
	for (int i = 0; i < getNumEuropeUnits(); ++i)
	{
		aiBefore.push_back(getEuropeUnit(i)->getID());
	}
	hurry(HURRY_IMMIGRANT, iBestIndex);
	for (int i = 0; i < getNumEuropeUnits(); ++i)
	{
		CvUnit* const pUnit = getEuropeUnit(i);
		if (std::find(aiBefore.begin(), aiBefore.end(), pUnit->getID()) == aiBefore.end())
		{
			if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) turn %d hurries %S for %d gold", getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), pUnit->getName().GetCString(), iCost);
			return pUnit;
		}
	}
	return NULL;
}

// WTP: equip the unit in Europe with a profession and give it its unit AI
bool CvPlayerAI::AI_earlyEquip(CvUnit* pUnit, ProfessionTypes eProfession, UnitAITypes eUnitAI)
{
	if (pUnit == NULL || eProfession == NO_PROFESSION)
	{
		return false;
	}
	if (pUnit->getProfession() != eProfession)
	{
		changeProfessionEurope(pUnit->getID(), eProfession);
	}
	if (pUnit->getProfession() != eProfession)
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) could not equip %S as %S (gold %d)", getID(), getCivilizationDescription(0), pUnit->getName().GetCString(),
			GC.getProfessionInfo(eProfession).getDescription(), getGold());
		return false;
	}
	pUnit->AI_setUnitAIType(eUnitAI);
	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) equips %S as %S, gold now %d", getID(), getCivilizationDescription(0), pUnit->getName().GetCString(),
		GC.getProfessionInfo(eProfession).getDescription(), getGold());
	return true;
}

// WTP: the cheapest ship we can buy that is a transport (not a warship), reaches our colonies, carries at least iMinCargo
// and is too small to carry treasures (those are kept for the galleons)
UnitTypes CvPlayerAI::AI_cheapestSeaUnitToBuy(int iMinCargo) const
{
	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	const int iTreasureSize = AI_minTreasureTransportSize();
	UnitTypes eBest = NO_UNIT;
	int iBestPrice = MAX_INT;
	for (UnitClassTypes eClass = FIRST_UNITCLASS; eClass < NUM_UNITCLASS_TYPES; ++eClass)
	{
		const UnitTypes eUnit = kCiv.getCivilizationUnits(eClass);
		if (eUnit == NO_UNIT)
		{
			continue;
		}
		const CvUnitInfo& kUnit = GC.getUnitInfo(eUnit);
		const int iPrice = getEuropeUnitBuyPrice(eUnit);
		if (kUnit.getDomainType() == DOMAIN_SEA && kUnit.getDefaultUnitAIType() == UNITAI_TRANSPORT_SEA && iPrice > 0 && iPrice < iBestPrice
			&& kUnit.getCargoSpace() >= iMinCargo && kUnit.getCargoSpace() < iTreasureSize && AI_shipReachableColonies(eUnit) > 0)
		{
			iBestPrice = iPrice;
			eBest = eUnit;
		}
	}
	return eBest;
}

// WTP: Explorer: a second scout, then the cheapest ship, which only explores
bool CvPlayerAI::AI_doEarlyExplorer()
{
	if (m_iEarlyProgress == 0)
	{
		const int iBefore = m_iEarlyScoutsBought;
		if (AI_doScoutPurchase(true))
		{
			return true; // saving for the scout
		}
		if (m_iEarlyScoutsBought > iBefore || GC.getGameINLINE().getGameTurn() > 80)
		{
			m_iEarlyProgress = 1;
		}
		return true;
	}
	const UnitTypes eShip = AI_bestShipToBuy(UNITAI_TRANSPORT_SEA, true);
	if (eShip == NO_UNIT)
	{
		AI_earlyEnd();
		return true;
	}
	const int iPrice = getEuropeUnitBuyPrice(eShip);
	if (getGold() < iPrice)
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) saves for the exploring ship %S (%d gold, have %d)", getID(), getCivilizationDescription(0), GC.getUnitInfo(eShip).getDescription(), iPrice, getGold());
		return true;
	}
	CvUnit* const pShip = buyEuropeUnit(eShip, 100);
	if (pShip != NULL)
	{
		pShip->AI_setUnitAIType(UNITAI_TRANSPORT_SEA);
		pShip->AI_setUnitAIState(UNITAI_STATE_EXPLORER_SHIP);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) turn %d buys %S for %d gold, it only explores", getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), pShip->getName().GetCString(), iPrice);
		AI_earlyEnd();
	}
	return true;
}

// WTP: Native Trader: six native traders in total (free colonists or indentured servants, hurried or bought, equipped in Europe)
bool CvPlayerAI::AI_doEarlyNativeTrader()
{
	const ProfessionTypes eTrader = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_NATIVE_TRADER");
	if (m_iEarlyProgress >= 6 || eTrader == NO_PROFESSION)
	{
		AI_earlyEnd();
		return true;
	}
	CvUnit* const pUnit = AI_earlyGetColonist(AI_earlyEquipCost(eTrader));
	if (pUnit != NULL && AI_earlyEquip(pUnit, eTrader, UNITAI_TRADER))
	{
		++m_iEarlyProgress;
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP early: Player %d (%S) has equipped %d of 6 native traders", getID(), getCivilizationDescription(0), m_iEarlyProgress);
		if (m_iEarlyProgress >= 6)
		{
			AI_earlyEnd();
		}
	}
	return true;
}

// WTP: Ship Trader: a ship with at least 3 cargo spaces that trades with the villages like the first native trade ship
bool CvPlayerAI::AI_doEarlyShipTrader()
{
	const UnitTypes eShip = AI_cheapestSeaUnitToBuy(3);
	if (eShip == NO_UNIT)
	{
		AI_earlyEnd();
		return true;
	}
	const int iPrice = getEuropeUnitBuyPrice(eShip);
	if (getGold() < iPrice)
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) saves for the trade ship %S (%d gold, have %d)", getID(), getCivilizationDescription(0), GC.getUnitInfo(eShip).getDescription(), iPrice, getGold());
		return true;
	}
	CvUnit* const pShip = buyEuropeUnit(eShip, 100);
	if (pShip != NULL)
	{
		pShip->AI_setUnitAIType(UNITAI_TRANSPORT_SEA);
		pShip->AI_setUnitAIState(UNITAI_STATE_NATIVE_TRADE_BUY);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) turn %d buys %S (cargo %d) for %d gold as its second native trade ship", getID(), getCivilizationDescription(0),
			GC.getGameINLINE().getGameTurn(), pShip->getName().GetCString(), pShip->cargoSpace(), iPrice);
		AI_earlyEnd();
	}
	return true;
}

// WTP: Early Improvement: 3 immigrants hurried, 2 pioneers (colonists equipped in Europe); Village Hall and Town Hall may be hurried
bool CvPlayerAI::AI_doEarlyImprovement()
{
	if (m_iEarlyProgress < 3)
	{
		if (AI_earlyHurryDockUnit())
		{
			++m_iEarlyProgress;
		}
		return true;
	}
	if (m_iEarlyProgress < 5)
	{
		const ProfessionTypes ePioneer = AI_idealProfessionForUnitAIType(UNITAI_WORKER);
		if (ePioneer == NO_PROFESSION)
		{
			AI_earlyEnd();
			return true;
		}
		CvUnit* const pUnit = AI_earlyGetColonist(AI_earlyEquipCost(ePioneer));
		if (pUnit != NULL && AI_earlyEquip(pUnit, ePioneer, UNITAI_WORKER))
		{
			++m_iEarlyProgress;
		}
		return true;
	}
	AI_earlyEnd();
	return true;
}

// WTP: the expert unit class that works the resource: the Prospector digs gold, silver and gemstones, the Pearl Hunter
// fishes the pearls (which give gems)
static UnitClassTypes earlyExpertClassForBonus(BonusTypes eBonus)
{
	if (GC.getBonusInfo(eBonus).getType() == CvString("BONUS_PEARLS_2"))
	{
		return (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_PEARLS_HUNTER");
	}
	return (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_PROSPECTOR");
}

// WTP: value of a precious resource: gold 50, silver 30, gems and pearls (BONUS_PEARLS_2, they give gems) 25. BONUS_PEARLS is only the food
// resource "Shellfish" and counts for nothing: a colony settled for it brought nothing.
static int earlyBonusValue(BonusTypes eBonus)
{
	if (eBonus == NO_BONUS)
	{
		return 0;
	}
	const CvString szBonus = GC.getBonusInfo(eBonus).getType();
	if (szBonus == "BONUS_GOLD") return 50;
	if (szBonus == "BONUS_SILVER") return 30;
	if (szBonus == "BONUS_GEMS" || szBonus == "BONUS_PEARLS_2") return 25;
	return 0;
}

// WTP: how good a site for Opportunistic Resources is: the most precious resource on the plots around it (not on the plot
// itself), a little more for a site near our colonies. 0 if the plot is no site: it must be coastal land that ships reach,
// free of owners, and give food for two colonists (2 food, 1 with a river beside it).
int CvPlayerAI::AI_earlyResourceSiteValue(const CvPlot& kPlot) const
{
	if (kPlot.isWater() || kPlot.isOwned() || !kPlot.isCoastalLand() || !kPlot.isRevealed(getTeam(), false))
	{
		return 0;
	}
	const int iFood = kPlot.calculateNatureYield(YIELD_FOOD, getTeam(), false);
	if (iFood < 2 && !(iFood >= 1 && kPlot.isRiverSide()))
	{
		return 0;
	}
	bool bReachable = false;
	int iBest = 0;
	for (int iDX = -1; iDX <= 1; ++iDX)
	{
		for (int iDY = -1; iDY <= 1; ++iDY)
		{
			if (iDX == 0 && iDY == 0)
			{
				continue;
			}
			const CvPlot* const pAround = plotXY(kPlot.getX_INLINE(), kPlot.getY_INLINE(), iDX, iDY);
			if (pAround == NULL)
			{
				continue;
			}
			if (pAround->isWater() && pAround->isEuropeAccessable() && abs(iDX) + abs(iDY) <= 2)
			{
				bReachable = true;
			}
			if (pAround->isRevealed(getTeam(), false))
			{
				iBest = std::max(iBest, earlyBonusValue(pAround->getBonusType()));
			}
		}
	}
	if (!bReachable || iBest == 0)
	{
		return 0;
	}
	int iDistance = 50;
	int iLoop;
	for (const CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		iDistance = std::min(iDistance, plotDistance(kPlot.getX_INLINE(), kPlot.getY_INLINE(), pCity->getX_INLINE(), pCity->getY_INLINE()));
	}
	return iBest * 100 + (50 - iDistance);
}

// WTP: the best site for Opportunistic Resources on the whole map (NULL if none is known); also its resource and value
CvPlot* CvPlayerAI::AI_earlyResourceSite(BonusTypes* peBonus, int* piValue) const
{
	CvPlot* pBest = NULL;
	int iBestValue = 0;
	for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); ++iI)
	{
		CvPlot* const pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
		const int iValue = AI_earlyResourceSiteValue(*pLoopPlot);
		if (iValue > iBestValue && canFound(pLoopPlot->coord(), false))
		{
			iBestValue = iValue;
			pBest = pLoopPlot;
		}
	}
	if (pBest != NULL && peBonus != NULL)
	{
		*peBonus = NO_BONUS;
		int iBestBonus = 0;
		for (int iDX = -1; iDX <= 1; ++iDX)
		{
			for (int iDY = -1; iDY <= 1; ++iDY)
			{
				const CvPlot* const pAround = plotXY(pBest->getX_INLINE(), pBest->getY_INLINE(), iDX, iDY);
				if (pAround != NULL && (iDX != 0 || iDY != 0) && earlyBonusValue(pAround->getBonusType()) > iBestBonus)
				{
					iBestBonus = earlyBonusValue(pAround->getBonusType());
					*peBonus = pAround->getBonusType();
				}
			}
		}
	}
	if (piValue != NULL)
	{
		*piValue = iBestValue;
	}
	return pBest;
}

// WTP: when a settler is sent for Opportunistic Resources: the 7 best sites (the first is the one the settler is meant for, the
// settler itself picks by site value and travel turns) with their data
void CvPlayerAI::AI_logEarlyResourceSites(const CvUnit& kSettler) const
{
	if (gPlayerLogLevel < 1)
	{
		return;
	}
	std::vector<std::pair<int, int> > aSites; // (site value, plot index)
	for (int iI = 0; iI < GC.getMap().numPlotsINLINE(); ++iI)
	{
		CvPlot* const pLoopPlot = GC.getMap().plotByIndexINLINE(iI);
		const int iValue = AI_earlyResourceSiteValue(*pLoopPlot);
		if (iValue > 0 && canFound(pLoopPlot->coord(), false))
		{
			aSites.push_back(std::make_pair(iValue, iI));
		}
	}
	std::sort(aSites.begin(), aSites.end(), std::greater<std::pair<int, int> >());
	logWTP(WTP_LOG_PLAN, getID(), "WTP early: Player %d (%S) turn %d sends settler %d (%S) for Opportunistic Resources, %d sites known",
		getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn(), kSettler.getID(), kSettler.getName().GetCString(), (int)aSites.size());
	for (uint i = 0; i < aSites.size() && i < 7; ++i)
	{
		const CvPlot* const pPlot = GC.getMap().plotByIndexINLINE(aSites[i].second);
		BonusTypes eBonus = NO_BONUS;
		int iBestBonus = 0;
		for (int iDX = -1; iDX <= 1; ++iDX)
		{
			for (int iDY = -1; iDY <= 1; ++iDY)
			{
				const CvPlot* const pAround = plotXY(pPlot->getX_INLINE(), pPlot->getY_INLINE(), iDX, iDY);
				if (pAround != NULL && (iDX != 0 || iDY != 0) && earlyBonusValue(pAround->getBonusType()) > iBestBonus)
				{
					iBestBonus = earlyBonusValue(pAround->getBonusType());
					eBonus = pAround->getBonusType();
				}
			}
		}
		int iDistance = 1000;
		int iLoop;
		for (const CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
		{
			iDistance = std::min(iDistance, plotDistance(pPlot->getX_INLINE(), pPlot->getY_INLINE(), pCity->getX_INLINE(), pCity->getY_INLINE()));
		}
		logWTP(WTP_LOG_PLAN, getID(), "WTP early:   site #%d at %d,%d: value %d, resource %S (%d), terrain food %d%s, %d plots from our nearest colony, area %d",
			(int)i + 1, pPlot->getX_INLINE(), pPlot->getY_INLINE(), aSites[i].first, (eBonus != NO_BONUS) ? GC.getBonusInfo(eBonus).getDescription() : L"-", iBestBonus,
			pPlot->calculateNatureYield(YIELD_FOOD, getTeam(), false), pPlot->isRiverSide() ? " (river)" : "", iDistance, pPlot->area()->getID());
	}
}

bool CvPlayerAI::AI_isEarlyResourceSettler(int iUnitID) const
{
	return m_iEarlyStrategy == EARLY_RESOURCES && iUnitID >= 0 && (m_aiEarlySettlerIDs[0] == iUnitID || m_aiEarlySettlerIDs[1] == iUnitID);
}

// WTP: Opportunistic Resources: 3 immigrants hurried, then wait until turn 17; then a settler and the expert for the best precious
// resource near a coastal site go over (twice, the second pair as soon as the money is there). The settler founds at that site.
bool CvPlayerAI::AI_doEarlyResources()
{
	if (m_iEarlyProgress < 3)
	{
		if (AI_earlyHurryDockUnit())
		{
			++m_iEarlyProgress;
		}
		return true;
	}
	if (GC.getGameINLINE().getGameTurn() < 17)
	{
		return true;
	}
	if (m_iEarlyProgress >= 7)
	{
		AI_earlyEnd();
		return true;
	}

	BonusTypes eBonus = NO_BONUS;
	int iSiteValue = 0;
	const CvPlot* const pSite = AI_earlyResourceSite(&eBonus, &iSiteValue);
	if (pSite == NULL)
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_PLAN, getID(), "WTP early: Player %d (%S) turn %d knows no site next to gold, silver or gems for a colony", getID(), getCivilizationDescription(0), GC.getGameINLINE().getGameTurn());
		if (GC.getGameINLINE().getGameTurn() > 80)
		{
			AI_earlyEnd();
		}
		return true;
	}
	const ProfessionTypes eSettler = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_SETTLER");
	const UnitClassTypes eExpertClass = earlyExpertClassForBonus(eBonus);
	const UnitTypes eExpert = (eExpertClass != NO_UNITCLASS) ? GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eExpertClass) : NO_UNIT;
	if (eSettler == NO_PROFESSION || eExpert == NO_UNIT || getEuropeUnitBuyPrice(eExpert) <= 0)
	{
		AI_earlyEnd();
		return true;
	}

	const int iPair = (m_iEarlyProgress - 3) / 2; // 0 or 1
	if ((m_iEarlyProgress - 3) % 2 == 0)
	{
		// the settler, with the money for the expert kept
		CvUnit* const pSettler = AI_earlyGetColonist(AI_earlyEquipCost(eSettler) + getEuropeUnitBuyPrice(eExpert));
		if (pSettler != NULL && AI_earlyEquip(pSettler, eSettler, UNITAI_SETTLER))
		{
			m_aiEarlySettlerIDs[iPair] = pSettler->getID();
			++m_iEarlyProgress;
			AI_logEarlyResourceSites(*pSettler);
		}
	}
	else if (getGold() >= getEuropeUnitBuyPrice(eExpert))
	{
		CvUnit* const pExpert = buyEuropeUnit(eExpert, 100);
		if (pExpert != NULL)
		{
			pExpert->AI_setUnitAIType(UNITAI_COLONIST);
			++m_iEarlyProgress;
			if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP early: Player %d (%S) turn %d buys %S for the colony at the %S (pair %d), gold now %d", getID(), getCivilizationDescription(0),
				GC.getGameINLINE().getGameTurn(), pExpert->getName().GetCString(), GC.getBonusInfo(eBonus).getDescription(), iPair + 1, getGold());
		}
	}
	else
	{
		AI_setSavingGold();
	}
	if (m_iEarlyProgress >= 7)
	{
		AI_earlyEnd();
	}
	return true;
}

// WTP: called every time the AI is in Europe (with ships and once a turn): the purchases of the early game
void CvPlayerAI::AI_doEarlyPhase()
{
	const int iTurn = GC.getGameINLINE().getGameTurn();
	if (m_iEarlyPhase == 0)
	{
		if (iTurn > 150)
		{
			m_iEarlyPhase = 2; // an old game that never started the early game
			return;
		}
		if (getNumCities() == 0)
		{
			return;
		}
		const int iBefore = m_iEarlyScoutsBought;
		if (AI_doScoutPurchase(true))
		{
			return; // saving for the first scout
		}
		if (m_iEarlyScoutsBought > iBefore || iTurn > 80)
		{
			AI_startEarlyStrategy();
		}
		return;
	}
	if (m_iEarlyPhase != 1)
	{
		return;
	}
	if (iTurn > 100)
	{
		AI_earlyEnd(); // a stalled strategy (no immigrants to hurry, no money) does not lock the AI for ever
		return;
	}
	switch (m_iEarlyStrategy)
	{
	case EARLY_EXPLORER: AI_doEarlyExplorer(); break;
	case EARLY_NATIVE_TRADER: AI_doEarlyNativeTrader(); break;
	case EARLY_SHIP_TRADER: AI_doEarlyShipTrader(); break;
	case EARLY_IMPROVEMENT: AI_doEarlyImprovement(); break;
	case EARLY_RESOURCES: AI_doEarlyResources(); break;
	default: AI_earlyEnd(); break;
	}
}

// WTP: two scouts before anything else (the first one comes free with the first colony)
bool CvPlayerAI::AI_doScoutPurchase(bool bExtra)
{
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || !canTradeWithEurope())
	{
		return false;
	}
	const UnitTypes eColonist = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits((UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_COLONIST"));
	// 0. two scouts before anybody else (the first one comes free with the first colony): exploring brings gold
	// and shows the villages nearby (teachers, trade).
	// The gold reserve does not apply, the scout only has to be paid in full including its horses.
	if ((bExtra || AI_totalUnitAIs(UNITAI_SCOUT) < 2) && GC.getGameINLINE().getGameTurn() <= 80)
	{
		const ProfessionTypes eScoutProfession = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_SCOUT");
		// a free colonist with scout equipment (a seasoned scout costs too much for the AI early on)
		const UnitTypes aeScoutUnits[1] = { eColonist };
		for (int iCandidate = 0; iCandidate < 1 && eScoutProfession != NO_PROFESSION; ++iCandidate)
		{
			const UnitTypes eUnit = aeScoutUnits[iCandidate];
			if (eUnit == NO_UNIT || getEuropeUnitBuyPrice(eUnit) <= 0)
			{
				continue;
			}
			int iEquipCost = 0;
			for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
			{
				iEquipCost += GET_PLAYER(getParent()).getYieldSellPrice(eYield) * GC.getProfessionInfo(eScoutProfession).getYieldEquipmentAmount(eYield);
			}
			if (getGold() < getEuropeUnitBuyPrice(eUnit) + iEquipCost)
			{
				// WTP: the AI saves for the scout: no civilian units and no hurrying meanwhile
				AI_setSavingGold();
				if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) saves for a scout (%d gold, have %d)", getID(), getCivilizationDescription(0),
					getEuropeUnitBuyPrice(eUnit) + iEquipCost, getGold());
				return true;
			}
			CvUnit* const pScout = buyEuropeUnit(eUnit, 100);
			if (pScout != NULL)
			{
				if (pScout->getProfession() != eScoutProfession && pScout->canHaveProfession(eScoutProfession, false))
				{
					changeProfessionEurope(pScout->getID(), eScoutProfession);
				}
				pScout->AI_setUnitAIType(UNITAI_SCOUT);
				++m_iEarlyScoutsBought;
				if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) buys scout %S", getID(), getCivilizationDescription(0), pScout->getNameAndProfession().GetCString());
				break;
			}
		}
	}

	return false;
}

// WTP: buy another transport if more units wait in Europe than one load of all our transports.
// Returns true while saving gold for it, then nothing else is bought.
bool CvPlayerAI::AI_doTransportPurchase()
{
	if (getNumCities() == 0)
	{
		return false;
	}
	int iCapacity = 0;
	int iTransports = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getDomainType() == DOMAIN_SEA && pLoopUnit->AI_getUnitAIType() == UNITAI_TRANSPORT_SEA)
		{
			iCapacity += pLoopUnit->cargoSpace();
			++iTransports;
		}
	}
	const int iWaiting = getNumEuropeUnits();
	// WTP: goods pile up: at least 2 coastal colonies more than 90% full for more than 3 turns, or a colony with a storage
	// of more than 2000 more than 90% full (user decision)
	bool bGoodsPileUp = false;
	{
		const int iTurn = GC.getGameINLINE().getGameTurn();
		int iFullCoastal = 0;
		for (std::map<int, int>::const_iterator it = m_mapFullStorageSince.begin(); it != m_mapFullStorageSince.end() && !bGoodsPileUp; ++it)
		{
			const CvCity* const pCity = getCity(it->first);
			if (pCity == NULL)
			{
				continue;
			}
			if (pCity->getMaxYieldCapacity() > 2000)
			{
				bGoodsPileUp = true;
			}
			if (iTurn - it->second > 3 && pCity->isCoastal(GC.getMIN_WATER_SIZE_FOR_OCEAN()))
			{
				++iFullCoastal;
			}
		}
		bGoodsPileUp = bGoodsPileUp || iFullCoastal >= 2;
	}
	if ((iWaiting <= iCapacity && !bGoodsPileUp) || iTransports >= std::max(3, getNumCities() + 1))
	{
		return false;
	}

	const UnitTypes eShip = AI_transportToBuy();
	if (eShip == NO_UNIT)
	{
		return false;
	}
	const int iPrice = getEuropeUnitBuyPrice(eShip);
	if (getGold() < iPrice)
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) saves for transport %S (%d gold, have %d): %d units wait in Europe, capacity %d in %d transports",
			getID(), getCivilizationDescription(0), GC.getUnitInfo(eShip).getDescription(), iPrice, getGold(), iWaiting, iCapacity, iTransports);
		return true;
	}
	CvUnit* pUnit = buyEuropeUnit(eShip, 100);
	if (pUnit != NULL)
	{
		pUnit->AI_setUnitAIType(UNITAI_TRANSPORT_SEA);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) buys transport %S: %d units wait in Europe, capacity was %d in %d transports%s",
			getID(), getCivilizationDescription(0), pUnit->getName().GetCString(), iWaiting, iCapacity, iTransports, bGoodsPileUp ? ", goods pile up in the colonies" : "");
	}
	return false;
}

// WTP: two defenders per colony (one before turn 25, in peace too) before civilian units are bought; missing defenders are free colonists
// equipped as colonial militia in Europe (cheaper than town guards and light artillery). Returns true while defenders
// are missing (then no civilian units are bought).
bool CvPlayerAI::AI_doMilitiaPurchase()
{
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || !canTradeWithEurope() || getNumCities() == 0)
	{
		return false;
	}
	const int iDefenders = AI_totalUnitAIs(UNITAI_DEFENSIVE);
	const int iWanted = ((GC.getGameINLINE().getGameTurn() >= 25) ? 2 : 1) * getNumCities();
	if (iDefenders >= iWanted)
	{
		return false;
	}
	const ProfessionTypes eMilitia = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_COLONIAL_MILITIA");
	const UnitTypes eColonist = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(GLOBAL_DEFINE_DEFAULT_POPULATION_UNIT);
	if (eMilitia == NO_PROFESSION || eColonist == NO_UNIT || getEuropeUnitBuyPrice(eColonist) <= 0)
	{
		return false;
	}
	int iEquipCost = 0;
	for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
	{
		iEquipCost += GET_PLAYER(getParent()).getYieldSellPrice(eYield) * GC.getProfessionInfo(eMilitia).getYieldEquipmentAmount(eYield);
	}
	const int iCost = getEuropeUnitBuyPrice(eColonist) + iEquipCost;
	if (getGold() < iCost)
	{
		AI_setSavingGold();
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) saves for colonial militia (%d gold, have %d): %d defenders for %d colonies",
			getID(), getCivilizationDescription(0), iCost, getGold(), iDefenders, getNumCities());
		return true;
	}
	CvUnit* const pUnit = buyEuropeUnit(eColonist, 100);
	if (pUnit != NULL)
	{
		if (pUnit->canHaveProfession(eMilitia, false))
		{
			changeProfessionEurope(pUnit->getID(), eMilitia);
		}
		pUnit->AI_setUnitAIType(UNITAI_DEFENSIVE);
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) buys colonial militia %S: %d defenders for %d colonies",
			getID(), getCivilizationDescription(0), pUnit->getNameAndProfession().GetCString(), iDefenders, getNumCities());
	}
	return true;
}

// WTP: war purchases: an even mix of light artillery, veteran colonial militia, line infantry, dragoons, heavy artillery and
// free colonists equipped as colonial militia and as line infantry; the type with the fewest units is bought next (the cheaper
// one on a tie). Civilian purchases get up to 30% of the gold spent in the war: while they save gold below that share, only
// colonies without any defender get a military unit. Returns true if a unit was bought.
bool CvPlayerAI::AI_doWarMilitaryPurchase()
{
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || !canTradeWithEurope() || getNumCities() == 0)
	{
		return false;
	}
	if (GET_TEAM(getTeam()).getAnyWarPlanCount() == 0)
	{
		return false;
	}
	int iUndefended = 0;
	const int iNeeded = AI_totalDefendersNeeded(&iUndefended, NULL, true);
	if (iNeeded <= 0)
	{
		return false;
	}
	const bool bCivilianShort = 100 * m_iWarCivilianSpent < 30 * (m_iWarCivilianSpent + m_iWarMilitarySpent);
	if (bCivilianShort && m_iSavingGoldTurn == GC.getGameINLINE().getGameTurn() && iUndefended == 0)
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) war: no military purchase, civilian purchases save gold (war spending military %d, civilian %d)",
			getID(), getCivilizationDescription(0), m_iWarMilitarySpent, m_iWarCivilianSpent);
		return false;
	}

	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	const UnitTypes eColonist = kCiv.getCivilizationUnits(GLOBAL_DEFINE_DEFAULT_POPULATION_UNIT);
	enum { NUM_WAR_TYPES = 7 };
	static const char* const aszUnitClass[NUM_WAR_TYPES] = { "UNITCLASS_LIGHT_ARTILLERY", "UNITCLASS_VETERAN", "UNITCLASS_EUROPEAN_LINE_INFANTRY",
		"UNITCLASS_VETERAN_DRAGOON", "UNITCLASS_HEAVY_ARTILLERY", NULL, NULL };
	static const char* const aszProfession[NUM_WAR_TYPES] = { "PROFESSION_LIGHT_ARTILLERY", "PROFESSION_COLONIAL_MILITIA", "PROFESSION_LINE_INFANTRY",
		"PROFESSION_DRAGOON", "PROFESSION_HEAVY_ARTILLERY", "PROFESSION_COLONIAL_MILITIA", "PROFESSION_LINE_INFANTRY" };
	UnitTypes aeUnit[NUM_WAR_TYPES];
	ProfessionTypes aeProfession[NUM_WAR_TYPES];
	int aiCount[NUM_WAR_TYPES];
	for (int i = 0; i < NUM_WAR_TYPES; ++i)
	{
		aeUnit[i] = (aszUnitClass[i] != NULL) ? (UnitTypes)kCiv.getCivilizationUnits((UnitClassTypes)GC.getInfoTypeForString(aszUnitClass[i])) : eColonist;
		aeProfession[i] = (ProfessionTypes)GC.getInfoTypeForString(aszProfession[i]);
		aiCount[i] = 0;
	}

	// count our units (on the map, aboard ships and in Europe): artillery and dragoons by profession, colonial militia and
	// line infantry as veteran units or as other colonists in that profession
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		const ProfessionTypes eProfession = pLoopUnit->getProfession();
		if (eProfession == NO_PROFESSION)
			continue;
		for (int i = 0; i < 5; ++i)
		{
			if (eProfession != aeProfession[i])
				continue;
			if (i == 1 || i == 2)
			{
				++aiCount[(pLoopUnit->getUnitType() == aeUnit[i]) ? i : i + 4];
			}
			else
			{
				++aiCount[i];
			}
			break;
		}
	}

	int iBest = -1;
	int iBestPrice = 0;
	for (int i = 0; i < NUM_WAR_TYPES; ++i)
	{
		if (aeUnit[i] == NO_UNIT || aeProfession[i] == NO_PROFESSION || !isProfessionValid(aeProfession[i], aeUnit[i]))
			continue;
		int iPrice = getEuropeUnitBuyPrice(aeUnit[i]);
		if (iPrice <= 0)
			continue;
		if (i >= 5)
		{
			// the equipment is bought in Europe (see CvPlayer::changeProfessionEurope)
			for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
			{
				iPrice += GET_PLAYER(getParent()).getYieldSellPrice(eYield) * GC.getProfessionInfo(aeProfession[i]).getYieldEquipmentAmount(eYield);
			}
		}
		if (iBest < 0 || aiCount[i] < aiCount[iBest] || (aiCount[i] == aiCount[iBest] && iPrice < iBestPrice))
		{
			iBest = i;
			iBestPrice = iPrice;
		}
	}
	if (iBest < 0)
	{
		return false;
	}
	if (getGold() < iBestPrice)
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) war: saves for %S as %S (%d gold, have %d), mix %d/%d/%d/%d/%d/%d/%d",
			getID(), getCivilizationDescription(0), GC.getUnitInfo(aeUnit[iBest]).getDescription(), GC.getProfessionInfo(aeProfession[iBest]).getDescription(),
			iBestPrice, getGold(), aiCount[0], aiCount[1], aiCount[2], aiCount[3], aiCount[4], aiCount[5], aiCount[6]);
		return false;
	}

	const int iGoldBefore = getGold();
	CvUnit* const pUnit = buyEuropeUnit(aeUnit[iBest], 100);
	if (pUnit == NULL)
	{
		return false;
	}
	if (pUnit->getProfession() != aeProfession[iBest] && pUnit->canHaveProfession(aeProfession[iBest], false))
	{
		changeProfessionEurope(pUnit->getID(), aeProfession[iBest]);
	}
	// artillery and militia defend, line infantry and dragoons attack
	pUnit->AI_setUnitAIType((iBest == 0 || iBest == 1 || iBest == 5) ? UNITAI_DEFENSIVE : UNITAI_OFFENSIVE);
	m_iWarMilitarySpent += std::max(0, iGoldBefore - getGold());
	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) war: buys %S for %d gold, mix before (light art./vet. militia/line inf./dragoon/heavy art./colonist militia/colonist line inf.) %d/%d/%d/%d/%d/%d/%d, war spending military %d, civilian %d",
		getID(), getCivilizationDescription(0), pUnit->getNameAndProfession().GetCString(), iGoldBefore - getGold(),
		aiCount[0], aiCount[1], aiCount[2], aiCount[3], aiCount[4], aiCount[5], aiCount[6], m_iWarMilitarySpent, m_iWarCivilianSpent);
	return true;
}

void CvPlayerAI::AI_doCivilianPurchases()
{
	MOD_PROFILE("CvPlayerAI::AI_doCivilianPurchases");
	if (isHuman() || isNative() || isEurope() || getParent() == NO_PLAYER || !canTradeWithEurope())
	{
		return;
	}
	// defence first while at war; civilian purchases get up to 30% of the gold spent in the war
	if (GET_TEAM(getTeam()).getAnyWarPlanCount() > 0 && AI_totalDefendersNeeded(NULL, NULL, true) > 0
		&& 100 * m_iWarCivilianSpent >= 30 * (m_iWarCivilianSpent + m_iWarMilitarySpent))
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) skips civilian purchases, war and %d defenders needed (war spending military %d, civilian %d)",
			getID(), getCivilizationDescription(0), AI_totalDefendersNeeded(NULL, NULL, true), m_iWarMilitarySpent, m_iWarCivilianSpent);
		return;
	}

	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	const UnitTypes eColonist = kCiv.getCivilizationUnits((UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_COLONIST"));
	const UnitClassTypes eHardyPioneerClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_PIONEER");
	const ProfessionTypes eSettlerProfession = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_SETTLER");
	const ProfessionTypes ePioneerProfession = (ProfessionTypes)GC.getInfoTypeForString("PROFESSION_PIONEER");
	int iBought = 0;

	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision: Player %d (%S) civilian purchases: gold %d cities %d desired %d settlers %d workers %d wanted %d",
		getID(), getCivilizationDescription(0), getGold(), getNumCities(), AI_desiredCityCount(), AI_totalUnitAIs(UNITAI_SETTLER),
		AI_totalUnitAIs(UNITAI_WORKER), std::min(AI_neededWorkers(NULL), std::max(2, 2 * getNumCities())));

	// 1. settlers: a hurried immigrant or a colonist equipped as settler in Europe
	if (getNumCities() > 0 && AI_desiredCityCount() > getNumCities() + AI_totalUnitAIs(UNITAI_SETTLER))
	{
		// only a non-expert becomes a settler; otherwise a free colonist is bought
		CvUnit* const pImmigrant = AI_hurryCheapImmigrant(false, true);
		if (pImmigrant != NULL)
		{
			// WTP: the immigrant hurried for this becomes the settler (it used to join a colony as a colonist)
			pImmigrant->AI_setUnitAIType(UNITAI_SETTLER);
			if (eSettlerProfession != NO_PROFESSION && pImmigrant->getProfession() != eSettlerProfession && pImmigrant->canHaveProfession(eSettlerProfession, false))
			{
				int iEquipCost = 0;
				for (YieldTypes eYield = FIRST_YIELD; eYield < NUM_CARGO_YIELD_TYPES; ++eYield)
				{
					const int iMissing = pImmigrant->getProfessionChangeYieldRequired(eSettlerProfession, eYield);
					if (iMissing > 0)
					{
						iEquipCost += iMissing * GET_PLAYER(getParent()).getYieldSellPrice(eYield);
					}
				}
				if (getGold() >= iEquipCost)
				{
					changeProfessionEurope(pImmigrant->getID(), eSettlerProfession);
				}
			}
			if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   hurried immigrant %S becomes a settler", pImmigrant->getNameAndProfession().GetCString());
			++iBought;
		}
		else if (AI_buyCivilianUnit(eColonist, UNITAI_SETTLER, eSettlerProfession))
		{
			++iBought;
		}
	}

	// 2. pioneers: up to two per colony, not one per plot to improve;
	// at most 2 hardy pioneers, further pioneers are equipped colonists
	const int iWantedPioneers = std::min(AI_neededWorkers(NULL), std::max(2, 2 * getNumCities()));
	if (iBought < 2 && AI_totalUnitAIs(UNITAI_WORKER) < iWantedPioneers)
	{
		const UnitTypes eHardyPioneer = (eHardyPioneerClass != NO_UNITCLASS) ? kCiv.getCivilizationUnits(eHardyPioneerClass) : NO_UNIT;
		bool bDone = false;
		if (eHardyPioneer != NO_UNIT && getUnitClassCount(eHardyPioneerClass) < 2)
		{
			bDone = AI_buyCivilianUnit(eHardyPioneer, UNITAI_WORKER, ePioneerProfession);
		}
		if (!bDone)
		{
			bDone = AI_buyCivilianUnit(eColonist, UNITAI_WORKER, ePioneerProfession);
		}
		if (bDone)
		{
			++iBought;
		}
	}

	if (iBought >= 2 || getNumCities() == 0)
	{
		return;
	}

	// 2b. food: colonies short of food while no new colonies are wanted get an expert farmer or fisherman;
	// on land an African slave is bought instead when it is cheaper (a transport fetches it from Africa)
	if (iBought < 2)
	{
		bool bWater = false;
		CvCity* const pFoodCity = AI_bestFoodWorkerCity(bWater);
		if (pFoodCity != NULL)
		{
			if (AI_isWorkforcePlanned())
			{
				// WTP: the workforce plan buys farmers, fishermen and African slaves for its food jobs (AI_buyExpertForOpportunity)
			}
			else if (!bWater && AI_wantsAfricaFoodWorkers())
			{
				// (a transport sails to Africa only while at most 3 units wait in Europe, see CvUnitAI::AI_sailToPreferredPort)
				if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   %S needs food (%+d), an African slave will be bought in Africa%s", pFoodCity->getName().GetCString(),
					pFoodCity->foodDifference(), getNumEuropeUnits() > 3 ? CvString::format(" - but no transport sails there while %d units wait in Europe", getNumEuropeUnits()).c_str() : "");
				// WTP: the slave is the food provider of that colony: no farmer is bought for it as well
				const UnitClassTypes eFarmerClass = (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FARMER");
				for (int i = (int)m_aWfOpenOpportunities.size() - 1; i >= 0; --i)
				{
					if (m_aWfOpenOpportunities[i].iCityID == pFoodCity->getID() && m_aWfOpenOpportunities[i].eExpertClass == eFarmerClass)
					{
						m_aWfOpenOpportunities.erase(m_aWfOpenOpportunities.begin() + i);
					}
				}
			}
			else if (AI_isWorkforcePlanned())
			{
				// WTP: the workforce plan buys farmers and fishermen for its food jobs (AI_buyExpertForOpportunity)
			}
			else
			{
				const UnitClassTypes eFoodClass = (UnitClassTypes)GC.getInfoTypeForString(bWater ? "UNITCLASS_FISHERMAN" : "UNITCLASS_FARMER");
				const UnitTypes eFoodExpert = (eFoodClass != NO_UNITCLASS) ? kCiv.getCivilizationUnits(eFoodClass) : NO_UNIT;
				if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   %S needs food (%+d), buying %S", pFoodCity->getName().GetCString(), pFoodCity->foodDifference(),
					eFoodExpert != NO_UNIT ? GC.getUnitInfo(eFoodExpert).getDescription() : L"nothing");
				if (AI_buyCivilianUnit(eFoodExpert, UNITAI_COLONIST, NO_PROFESSION))
				{
					++iBought;
				}
			}
		}
	}

	if (iBought >= 2)
	{
		return;
	}

	// 3. sometimes hurry an immigrant instead of buying an expert (not while colonists wait for a job)
	const bool bIdleColonists = AI_isWorkforcePlanned() && m_iWfIdleCount >= std::max(1, getNumCities() / 2);
	if (!bIdleColonists && !AI_isEuropeBacklog() && GC.getGameINLINE().getSorenRandNum(100, "AI civilian hurry instead of expert") < 25)
	{
		if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   random immigrant hurry instead of expert");
		AI_hurryCheapImmigrant(false);
		return;
	}

	// 4. the colonists the workforce plan wants (best gain per price); the colonial AI buys no other experts
	if (AI_buyExpertForOpportunity() || AI_isWorkforcePlanned())
	{
		return;
	}

	// 4b. experts: replace non-experts in their profession or fill free building slots that have enough input;
	// raw material experts (except lumber) are learned from nearby native villages instead of bought
	UnitClassTypes eBestClass = NO_UNITCLASS;
	int iBestPriority = 0;
	bool bLearnFromNatives = false;
	int iLoop;
	for (CvCity* pLoopCity = firstCity(&iLoop); pLoopCity != NULL; pLoopCity = nextCity(&iLoop))
	{
		for (ProfessionTypes eProfession = FIRST_PROFESSION; eProfession < NUM_PROFESSION_TYPES; ++eProfession)
		{
			UnitClassTypes eExpertClass = NO_UNITCLASS;
			const int iPriority = AI_civilianExpertPriority(eProfession, eExpertClass);
			if (iPriority <= iBestPriority || !kCiv.isValidProfession(eProfession))
			{
				continue;
			}
			const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
			const CvString szType = kProfession.getType();
			if (szType == "PROFESSION_JUDGE" && pLoopCity->getCityCrime() <= pLoopCity->getCityLaw())
			{
				continue;
			}

			int iUsed = 0;
			int iNonExperts = 0;
			for (int i = 0; i < pLoopCity->getPopulation(); ++i)
			{
				const CvUnit* const pCitizen = pLoopCity->getPopulationUnitByIndex(i);
				if (pCitizen != NULL && pCitizen->getProfession() == eProfession)
				{
					++iUsed;
					if (pCitizen->getUnitClassType() != eExpertClass)
					{
						++iNonExperts;
					}
				}
			}

			bool bDemand = iNonExperts > 0;
			if (!bDemand && !kProfession.isWorkPlot() && pLoopCity->getNumProfessionBuildingSlots(eProfession) > iUsed)
			{
				const YieldTypes eInput = (YieldTypes)kProfession.getYieldsConsumed(0);
				bDemand = (eInput == NO_YIELD) || (pLoopCity->getRawYieldProduced(eInput) + pLoopCity->getYieldStored(eInput) - pLoopCity->getRawYieldConsumed(eInput) >= 3);
			}
			if (!bDemand)
			{
				continue;
			}

			// can the expert be learned from a native village within 8 plots?
			bool bLearnable = false;
			if (kProfession.isWorkPlot() && szType != "PROFESSION_LUMBERJACK")
			{
				for (int iPlayer = 0; iPlayer < MAX_PLAYERS && !bLearnable; ++iPlayer)
				{
					CvPlayer& kNative = GET_PLAYER((PlayerTypes)iPlayer);
					if (!kNative.isAlive() || !kNative.isNative())
					{
						continue;
					}
					int iNativeLoop;
					for (CvCity* pVillage = kNative.firstCity(&iNativeLoop); pVillage != NULL; pVillage = kNative.nextCity(&iNativeLoop))
					{
						if (pVillage->getTeachUnitClass() == eExpertClass && plotDistance(pVillage->getX_INLINE(), pVillage->getY_INLINE(), pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE()) <= 8)
						{
							bLearnable = true;
							break;
						}
					}
				}
			}
			if (bLearnable)
			{
				bLearnFromNatives = true;
				continue;
			}

			iBestPriority = iPriority;
			eBestClass = eExpertClass;
		}
	}

	if (gPlayerLogLevel >= 1) logWTP(WTP_LOG_BUY, getID(), "WTP decision:   best expert %S priority %d, learnable from natives %d",
		eBestClass != NO_UNITCLASS ? GC.getUnitClassInfo(eBestClass).getDescription() : L"none", iBestPriority, (int)bLearnFromNatives);
	if (eBestClass != NO_UNITCLASS)
	{
		AI_buyCivilianUnit(kCiv.getCivilizationUnits(eBestClass), UNITAI_COLONIST, NO_PROFESSION);
	}
	else if (bLearnFromNatives)
	{
		// a cheap immigrant can learn the profession from the natives
		AI_hurryCheapImmigrant(false);
	}
}

// TAC - AI More Immigrants - koma13 - START
void CvPlayerAI::AI_hurryDocks()
{
	if (GC.getAI_HURRY_IMMIGRANTS() <= 0)
	{
		return;
	}

	// WTP: not while units wait in Europe for a ship
	if (AI_isWorkforcePlanned() && AI_isEuropeBacklog())
	{
		return;
	}

	if (isHuman())
	{
		return;
	}

	if (getParent() == NO_PLAYER)
	{
		return;
	}

	if (!canTradeWithEurope())
	{
		return;
	}

	// WTP: no more immigrants while colonists wait for a job
	if (AI_isWorkforcePlanned() && m_iWfIdleCount >= std::max(1, getNumCities() / 2))
	{
		return;
	}

	for (unsigned int iIndex = 0; iIndex < this->getNumUnitsOnDock(); ++iIndex)
	{
		UnitTypes eLoopUnit = getDocksNextUnit(iIndex);

		if (eLoopUnit != NO_UNIT)
		{
			CvUnitInfo& kUnitInfo = GC.getUnitInfo(eLoopUnit);
			if (kUnitInfo.isNoBadGoodies())
			{
				if (canHurry(HURRY_IMMIGRANT, iIndex))
				{
					hurry(HURRY_IMMIGRANT, iIndex);
				}
			}
		}
	}
}
// TAC - AI More Immigrants - koma13 - END

// TAC - AI Assault Sea - koma13 - START
bool CvPlayerAI::AI_prepareAssaultSea()
{
	if (isNative() || (getParent() == NO_PLAYER))
	{
		return false;
	}
	if (GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAITrainPercent() > 75)
	{
		return false;
	}

	for (int iI = 0; iI < MAX_PLAYERS; iI++)
	{
		CvPlayerAI& kLoopPlayer = GET_PLAYER((PlayerTypes)iI);

		if (kLoopPlayer.isAlive())
		{
			if (!kLoopPlayer.isNative())
			{
				if (iI != getID() && (getTeam() != kLoopPlayer.getTeam()))
				{
					if (GET_TEAM(getTeam()).isHasMet(kLoopPlayer.getTeam()))
					{
						int iLoop;
						for (CvCity* pLoopCity = kLoopPlayer.firstCity(&iLoop); pLoopCity != NULL; pLoopCity = kLoopPlayer.nextCity(&iLoop))
						{
							if (pLoopCity->plot()->isRevealed(getTeam(), false))
							{
								if (!AI_isPrimaryArea(pLoopCity->area()))
								{
									return true;
								}
							}
						}
					}
				}
			}
		}
	}
	return false;
}
// TAC - AI Assault Sea - koma13 - END
// TAC - AI Revolution - koma13 - START
int CvPlayerAI::AI_getLastWave() const
{
	return m_iLastWave;
}

void CvPlayerAI::AI_setLastWave(int iTurn)
{
	FAssert(iTurn >= 0);
	FAssert(iTurn <= GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getWaveTurns());
	m_iLastWave = iTurn;
}

int CvPlayerAI::AI_getWaveIndex() const
{
	return m_iWaveIndex;
}

void CvPlayerAI::AI_setWaveIndex(int iNum)
{
	FAssert(iNum >= -1);
	FAssert(iNum <= GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getNumWaves());
	m_iWaveIndex = iNum;
}
// TAC - AI Revolution - koma13 - END
// TAC - AI City Sites - koma13 - START
bool CvPlayerAI::AI_isLandLocked(bool bIgnoreCultureLock) const
{
	int iLoop;
	for(CvArea* pLoopArea = GC.getMap().firstArea(&iLoop); pLoopArea != NULL; pLoopArea = GC.getMap().nextArea(&iLoop))
	{
		if (!pLoopArea->isWater())
		{
			if (pLoopArea->getCitiesPerPlayer(getID()) > 0)
			{
				if (pLoopArea->getBestFoundValue(getID()) > (bIgnoreCultureLock ? 1 : 1500))
				{
					return false;
				}
			}
		}
	}

	return true;
}
// TAC - AI City Sites - koma13 - END

// Returns the best price that we can get for the yield considering the export markets available to us
// TODO: Consider natives, fellow Europeans, internal market, Port Royal
int CvPlayerAI::AI_getYieldBestExportPrice(YieldTypes eYield) const
{
	FAssert(!isNative());
	FAssert(GC.getYieldInfo(eYield).isCargo());

	PlayerTypes eParent = getParent();

	// This will allow the king to consider his own market for export
	if (getParent() == NO_PLAYER)
	{
		eParent = getID();
	}

	const CvPlayer& kParent = GET_PLAYER(eParent);

	// Return the max price we would get by selling in the currently AI accessibly ports
	// WTP, ray, Africa really is a bad decision here for AI. - It is too rare that AI should sail to Africa.
	// Especially if we only use this to determin the best price of a Yield on CityPlot
	// return std::max(kParent.getYieldBuyPrice(eYield), kParent.getYieldAfricaBuyPrice(eYield));
	return kParent.getYieldBuyPrice(eYield);
}

// TAC - AI City Sites - koma13 - END
// TAC - AI City Sites - koma13 - END

// Returns true if we should prefer to to hurry a unit rather than buying an equivalent unit
bool CvPlayerAI::AI_shouldHurryUnit() const
{
	// Note: Hurrying is generally better in the early game since the AI economy is not yet sophisticated
	// enough to be able to properly employ specialists.
	// Hurrying is also preferable until the cost of hurrying approaches the cost of a free colonist
	int iEuropeMinBuyCost = INT_MAX;

	for (UnitClassTypes eUnitClass = FIRST_UNITCLASS; eUnitClass < NUM_UNITCLASS_TYPES; ++eUnitClass)
	{
		const UnitTypes eLoopUnit = GC.getCivilizationInfo(getCivilizationType()).getCivilizationUnits(eUnitClass);
		if (eLoopUnit != NO_UNIT)
		{
			const CvUnitInfo& kUnitInfo = GC.getUnitInfo(eLoopUnit);

			// Find the cheapest European unit suitable for any profession that is for sale which cannot escape
			if (kUnitInfo.getDefaultUnitAIType() == UNITAI_COLONIST && kUnitInfo.getEuropeCost() > 0 && !kUnitInfo.LbD_canEscape())
			{
				if (getEuropeUnitBuyPrice(eLoopUnit) < iEuropeMinBuyCost)
				{
					iEuropeMinBuyCost = getEuropeUnitBuyPrice(eLoopUnit);
				}
			}
		}
	}

	// If we didn't find a single candidate then something is really wrong!
	FAssert(iEuropeMinBuyCost != INT_MAX);

	int iEuropeMinHurryCost = INT_MAX;

	// Find the cost of hurrying the cheapest unit suitable for any profession that cannot escape
	for (unsigned int iIndex = 0; iIndex < CivEffect().getNumUnitsOnDock(); ++iIndex)
	{
		const int iHurryCost = getHurryGold(HURRY_IMMIGRANT, iIndex);
		if (iHurryCost < iEuropeMinHurryCost)
		{
			iEuropeMinHurryCost = iHurryCost;
		}
	}

	FAssert(iEuropeMinHurryCost != INT_MAX);
	return (iEuropeMinHurryCost < iEuropeMinBuyCost);
}

// Returns the index of the best unit (as in purchase price of an equivalent unit) to hurry
int CvPlayerAI::AI_getBestDockUnit() const
{
	int iBestIndex = -1;
	int iBestAdvantage = 0;
	const int iDockCount = CivEffect().getNumUnitsOnDock();

	// TODO: This needs to be a while loop since the indices are not stable
	for (int iIndex = 0; iIndex < iDockCount; ++iIndex)
	{
		const UnitTypes eLoopUnit = getDocksNextUnit(iIndex);

		if (eLoopUnit == NO_UNIT)
		{
			continue;
		}

		const int iHurryCost = getHurryGold(HURRY_IMMIGRANT, iIndex);

		const int iUnitCost = getEuropeUnitBuyPrice(eLoopUnit);

		// Note: unit cost may be negative for units that are available for purchasing though
		// the absolute value will still indicate the value of the unit
		const int iHurryCostAdvantage = std::abs(iUnitCost) - iHurryCost;

		if (iHurryCostAdvantage > iBestAdvantage)
		{
			iBestAdvantage = iHurryCostAdvantage;
			iBestIndex = iIndex;
		}
	}

	if (iBestIndex == -1)
	{
		return 0;
	}

	return iBestIndex;
}

// Determine the units on the docks that have the largest cost\value difference and hurry them
void CvPlayerAI::AI_hurryBestDockUnits(int iHurryCount)
{
	if (AI_canHurryDockUnit() && iHurryCount > 0)
	{
		// Attempt to fill the rest of our cargo holds with hurried colonists
		for (int iIndex = 0; iIndex < iHurryCount; iIndex++)
		{
			const int index = AI_getBestDockUnit();

			// Note: Will be false if we can't afford it
			const bool bCanHurry = canHurry(HURRY_IMMIGRANT, iIndex);

			if (bCanHurry)
			{
				hurry(HURRY_IMMIGRANT, iIndex);
			}
			else
			{
				return;
			}
		}
	}
}

bool CvPlayerAI::AI_canHurryDockUnit() const
{
	FAssert(!isHuman());

	if (GC.getAI_HURRY_IMMIGRANTS() <= 0)
	{
		return false;
	}

	if (getParent() == NO_PLAYER)
	{
		return false;
	}

	if (!canTradeWithEurope())
	{
		return false;
	}

	return true;
}

// Returns the effective combat multiplier(*100) that only applies vs. the king.
// Adjusts the estimated combat strength by  take into account traits (e.g. determined) and the AI bonus vs. its king
// Note that 100 is the baseline so this function returns the additional modifier to be added. A return value of 150 indicates that our strength should be inflated by 50%
int CvPlayerAI::AI_getColonialMilitaryModifier() const
{
	FAssert(!isHuman());

	// Note: NBMOD_GetColonialMilitaryValue already takes the rebel modifier into account
	int iModifier = getRebelCombatPercent();
	iModifier += GC.getHandicapInfo(GC.getGameINLINE().getHandicapType()).getAIKingCombatModifier();
	return iModifier;
}

// Estimates the unemployment rate in our empire
int CvPlayerAI::AI_estimateUnemploymentCount() const
{
	int cnt = 0;
	int iLoop;

	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		// Only count colonists that are
		// 1) On the map (so not inside a city or on the docs)
		// 2) Not being transported
		// 3) Not executing a mission(acitvity)
		if (pLoopUnit->AI_getUnitAIType() == UNITAI_COLONIST && pLoopUnit->isOnMap_() && !pLoopUnit->isCargo() &&
			pLoopUnit->getGroup()->getActivityType() != ACTIVITY_MISSION)
		{
			cnt++;
		}
	}

	logBBAI("CvPlayerAI::AI_estimateUnemploymentCount for Player %S: %d", getCivilizationDescription(),
		cnt);

	return cnt;
}

// WTP: the first colony needs a landmass of more than 40 plots to expand on, at least 4 usable land plots next
// to the city center and must not be founded within 5 plots of another European's colony
// WTP: whether the plot belongs to the radius of a colony of another non-native player
bool CvPlayerAI::AI_isInForeignColonyRadius(const CvPlot& kPlot) const
{
	for (int iDX = -2; iDX <= 2; ++iDX)
	{
		for (int iDY = -2; iDY <= 2; ++iDY)
		{
			const CvPlot* const pLoopPlot = plotXY(kPlot.getX_INLINE(), kPlot.getY_INLINE(), iDX, iDY);
			const CvCity* const pCity = (pLoopPlot != NULL) ? pLoopPlot->getPlotCity() : NULL;
			if (pCity != NULL && pCity->getOwnerINLINE() != getID() && !pCity->isNative() && pCity->getCityPlotIndex(&kPlot) != NO_CITY_PLOT)
			{
				return true;
			}
		}
	}
	return false;
}

bool CvPlayerAI::AI_isFirstColonySiteAllowed(const CvPlot& kPlot) const
{
	// WTP: the longer we look for a first site, the more the soft rules are relaxed (the coast rule always stays):
	// from 10 turns on map edge 5 -> 2 and foreign colonies 5 -> 3 plots, from 20 turns landmass 40 -> 20 and usable land 4 -> 3
	const int iSearchTurns = GC.getGameINLINE().getGameTurn() - GC.getGameINLINE().getStartTurn();
	const bool bRelaxed = iSearchTurns >= 10;
	const bool bVeryRelaxed = iSearchTurns >= 20;

	if (kPlot.area()->getNumTiles() <= (bVeryRelaxed ? 20 : 40))
	{
		return false;
	}

	// the first colony is the port to Europe: it needs real coast, not only a large river, a lake or shallow coast
	if (!kPlot.hasDeepWaterCoast())
	{
		return false;
	}

	// not within 5 plots of the map edge (wrapping directions have no edge)
	const CvMap& kMap = GC.getMap();
	const int iEdgeDistance = bRelaxed ? 2 : 5;
	if (!kMap.isWrapXINLINE() && (kPlot.getX_INLINE() < iEdgeDistance || kPlot.getX_INLINE() >= kMap.getGridWidth() - iEdgeDistance))
	{
		return false;
	}
	if (!kMap.isWrapYINLINE() && (kPlot.getY_INLINE() < iEdgeDistance || kPlot.getY_INLINE() >= kMap.getGridHeight() - iEdgeDistance))
	{
		return false;
	}

	// at least 4 usable land plots next to the city center
	int iUsableLand = 0;
	for (int iI = 0; iI < NUM_DIRECTION_TYPES; ++iI)
	{
		const CvPlot* const pLoopPlot = plotDirection(kPlot.getX_INLINE(), kPlot.getY_INLINE(), (DirectionTypes)iI);
		// WTP: land of native villages is usable, only plots in the radius of another European colony are not
		if (pLoopPlot != NULL && !pLoopPlot->isWater() && !pLoopPlot->isImpassable() && !AI_isInForeignColonyRadius(*pLoopPlot)
			&& (!pLoopPlot->isOwned() || pLoopPlot->getOwnerINLINE() == getID() || GET_PLAYER(pLoopPlot->getOwnerINLINE()).isNative()))
		{
			++iUsableLand;
		}
	}
	if (iUsableLand < (bVeryRelaxed ? 3 : 4))
	{
		return false;
	}

	// WTP: the first colonies of the Europeans are at least 12 plots apart (8 after 15 turns of searching)
	const int iMinDistance = AI_firstColonyMinDistance();
	for (int iPlayer = 0; iPlayer < MAX_PLAYERS; ++iPlayer)
	{
		const CvPlayer& kOther = GET_PLAYER((PlayerTypes)iPlayer);
		if (iPlayer == getID() || !kOther.isAlive() || kOther.isNative())
		{
			continue;
		}
		int iLoop;
		for (const CvCity* pLoopCity = kOther.firstCity(&iLoop); pLoopCity != NULL; pLoopCity = kOther.nextCity(&iLoop))
		{
			if (plotDistance(kPlot.getX_INLINE(), kPlot.getY_INLINE(), pLoopCity->getX_INLINE(), pLoopCity->getY_INLINE()) < iMinDistance)
			{
				return false;
			}
		}
	}
	return true;
}

// WTP: minimum distance between the first colonies of the Europeans: 12 plots, 8 after 15 turns of searching
int CvPlayerAI::AI_firstColonyMinDistance()
{
	return (GC.getGameINLINE().getGameTurn() - GC.getGameINLINE().getStartTurn() >= 15) ? 8 : 12;
}

int CvPlayerAI::AI_firstCityFoundValue(const CvPlot& kPlot)
{
	PROFILE_FUNC();

	if (isNative() || getParent() == NO_PLAYER)
		return 0;

	if (getNumCities() != 0)
		return 0;

	// Do not settle the first city on an island
	if (kPlot.area()->isIsland())
		return 0;

	const TeamTypes eTeam = getTeam();
	const CvPlayer& kParent = GET_PLAYER(getParent());

	if (!canFound(Coordinates(kPlot.getX_INLINE(), kPlot.getY_INLINE())))
		return 0;

	if (!AI_isFirstColonySiteAllowed(kPlot))
		return 0;

	if (!kPlot.isCoastalLand(GC.getMIN_WATER_SIZE_FOR_OCEAN()))
		return 0;
	if (kPlot.getNearestEurope() == NO_EUROPE)
		return 0;

	// Hard reject if an enemy city is in the BFC (WTP: hostile camps do not matter anymore)
	for (int i = 0; i < NUM_CITY_PLOTS_2_PLOTS; ++i)
	{
		CvPlot* p = plotCity(kPlot.getX_INLINE(), kPlot.getY_INLINE(), i);
		if (p == NULL) continue;

		// Enemy city inside BFC (WTP: native villages are allowed)
		if (p->getPlotCity() != NULL && p->getTeam() != eTeam && !p->getPlotCity()->isNative())
			return 0;
	}

	// Approximate ship movement turns from Europe entry to this site.
	// (If you want the exact ship path, pass the transport instead.)
	int sailTurns = 0;
	{
		int tiles = kPlot.getDistanceToOcean();
		if (tiles < 0) tiles = 20;
		sailTurns = (tiles + 3) / 4;
		if (sailTurns > 20) sailTurns = 20;
	}

	// ---------- Production scan over NON-center tiles ----------
	const int foodPerPop = GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION;

	int bestFood1 = 0;
	int bestFood2 = 0;
	bool hasLumber = false;

	long long ringWeightedProduction = 0LL;

	for (int i = 0; i < NUM_CITY_PLOTS; ++i)
	{
		if (i == CITY_HOME_PLOT) continue;

		CvPlot* p = plotCity(kPlot.getX_INLINE(), kPlot.getY_INLINE(), i);
		if (p == NULL) continue;

		// Top-2 FOOD
		const int yFood = p->calculateBestNatureYield(YIELD_FOOD, eTeam);
		if (yFood > bestFood1) { bestFood2 = bestFood1; bestFood1 = yFood; }
		else if (yFood > bestFood2) { bestFood2 = yFood; }

		// LUMBER presence
		if (!hasLumber)
		{
			if (p->calculateBestNatureYield(YIELD_LUMBER, eTeam) > 0)
				hasLumber = true;
			else
			{
				const FeatureTypes f = p->getFeatureType();
				if (f != NO_FEATURE && GC.getFeatureInfo(f).getYieldChange(YIELD_LUMBER) > 0)
					hasLumber = true;
			}
		}

		// Best weighted yield for this plot
		long long bestWeightedThisPlot = 0LL;

		for (int iy = 0; iy < NUM_YIELD_TYPES; ++iy)
		{
			const YieldTypes eY = (YieldTypes)iy;
			const int yNat = p->calculateBestNatureYield(eY, eTeam);
			if (yNat <= 0) continue;

			long long weight = 0LL;
			if (eY == YIELD_FOOD || eY == YIELD_LUMBER)
			{
				const int v = AI_yieldValue(eY);
				if (v > 0) weight = (long long)yNat * (long long)v;
			}
			else
			{
				const int iSell = kParent.getYieldBuyPrice(eY);
				if (iSell > 0)
					weight = (long long)yNat * (long long)iSell;
			}

			if (weight > bestWeightedThisPlot)
				bestWeightedThisPlot = weight;
		}

		ringWeightedProduction += bestWeightedThisPlot;
	}

	// Gates
	if (!hasLumber)
		return 0;

	const int cityFood = bestFood1 + bestFood2;
	if (cityFood < 2 * foodPerPop)
		return 0;

	// ---------- Scoring ----------
	long long val = 100000LL;

	// Add ring production (scaled)
	val += ringWeightedProduction * 12LL;

	// Extra food beyond minimum
	int surplus = cityFood - (2 * foodPerPop);
	if (surplus > 0)
	{
		if (surplus > 6) surplus = 6;
		val += surplus * 50LL * 100LL;
	}

	// Europe distance penalty only (no settler position or reveal-based penalties anymore)
	const int wSail = 5;
	long long denom = 100LL;
	denom += (long long)wSail * (long long)sailTurns * 5LL;

	if (denom < 50LL)   denom = 50LL;
	if (denom > 5000LL) denom = 5000LL;

	val = (val * 1000LL) / denom;

	if (val < 1LL) val = 1LL;
	if (val > 2147483647LL) val = 2147483647LL;
	return static_cast<int>(val);
}

// WTP: automatic job assignment for a human player's colonist (AUTOMATE_ASSIGN_JOB). The travel times of the workforce plan are
// made once per turn for the human player (the AI makes them at the start of its plan)
void CvPlayerAI::AI_wfEnsureTravel(const CvUnit& kColonist)
{
	if (m_iWfTravelTurn != GC.getGameINLINE().getGameTurn())
	{
		AI_wfUpdateTravel(kColonist);
		m_iWfTravelTurn = GC.getGameINLINE().getGameTurn();
	}
}

// a transport a colonist may board on its way: the AI's transports; of a human player only the automated ones
bool CvPlayerAI::AI_isWfShip(const CvUnit& kShip)
{
	if (kShip.AI_getUnitAIType() != UNITAI_TRANSPORT_SEA && kShip.AI_getUnitAIType() != UNITAI_TRANSPORT_COAST)
	{
		return false;
	}
	return !kShip.isHuman() || (kShip.getGroup() != NULL && kShip.getGroup()->getAutomateType() == AUTOMATE_TRANSPORT_FULL);
}

bool CvPlayerAI::AI_hasWfShip() const
{
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		if (pLoopUnit->getDomainType() == DOMAIN_SEA && AI_isWfShip(*pLoopUnit))
		{
			return true;
		}
	}
	return false;
}

namespace
{
	// colonists that never work in buildings (as in the workforce plan); failed traders only as carpenters, disillusioned
	// missionaries only in the church
	bool wfMayWorkInBuilding(const CvUnit& kUnit, YieldTypes eYield)
	{
		const UnitClassTypes eClass = (UnitClassTypes)kUnit.getUnitInfo().getUnitClassType();
		// (an indentured servant may study at school)
		if (eYield == YIELD_EDUCATION && eClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_INDENTURED_SERVANT"))
		{
			return true;
		}
		const char* const aszFieldOnly[6] = { "UNITCLASS_INDENTURED_SERVANT", "UNITCLASS_MULATTO", "UNITCLASS_MESTIZZO",
			"UNITCLASS_AFRICAN_SLAVE", "UNITCLASS_NATIVE_SLAVE", "UNITCLASS_CONVERTED_NATIVE" };
		for (int k = 0; k < 6; ++k)
		{
			if (eClass == (UnitClassTypes)GC.getInfoTypeForString(aszFieldOnly[k]))
			{
				return false;
			}
		}
		if (eClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FAILED_TRADER"))
		{
			return eYield == YIELD_HAMMERS;
		}
		if (eClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_FAILED_MISSIONARY"))
		{
			return eYield == YIELD_CROSSES;
		}
		return true;
	}

	// the unhappiness balance of a colony (unhappiness minus happiness) with iNewColonists more citizens: more unhappiness
	// from the population and from missing defenders, the other factors as now
	int wfUnhappinessWith(const CvCity& kCity, int iNewColonists)
	{
		const int iPop = kCity.getPopulation();
		const int iNewPop = iPop + iNewColonists;
		const int iMinPop = GC.getMIN_POP_NEG_HAPPINESS();
		int iDelta = std::max(0, iNewPop - iMinPop) - std::max(0, iPop - iMinPop);
		const int iDivisor = std::max(1, GC.getPOP_DIVISOR_DEFENSE_UNHAPPINESS());
		int iDefenders = 0;
		const CvPlot* const pPlot = kCity.plot();
		for (int i = 0; i < pPlot->getNumUnits(); ++i)
		{
			const CvUnit* const pLoopUnit = pPlot->getUnitByIndex(i);
			if (pLoopUnit != NULL && pLoopUnit->getDomainType() == DOMAIN_LAND && pLoopUnit->canAttack() && pLoopUnit->getOwnerINLINE() == kCity.getOwnerINLINE())
			{
				++iDefenders;
			}
		}
		iDelta += std::max(0, iNewPop / iDivisor - iDefenders) - std::max(0, iPop / iDivisor - iDefenders);
		return kCity.getCityUnHappiness() - kCity.getCityHappiness() + iDelta;
	}

	// the unit type whose output counts: an expert outside its trade works like a free colonist (as in the workforce plan)
	UnitTypes wfWorkingType(const CvUnitAI& kUnit, ProfessionTypes eProfession, UnitTypes eColonist)
	{
		if (kUnit.AI_getIdealProfession() != NO_PROFESSION && !wfIsExpertFor(kUnit, eProfession))
		{
			return eColonist;
		}
		return kUnit.getUnitType();
	}

	// gold x100 per turn a unit earns in a job (output at the Europe price, the input deducted, food 4 gold); -1: no output
	// gold x100 of the output of a human player's colonist (job automation): learning at school 10 gold for a free colonist,
	// 6 for an indentured servant, nothing for others; health 1 gold if the colony gains 2 health already; law 1 gold from 15
	// law on; bells 9 gold; happiness 15 to 20 gold in an unhappy colony, 8 in a very happy one; everything else as in the workforce plan (wfYieldGold)
	int wfHumanYieldGold(const CvCity& kCity, const CvUnitAI& kUnit, YieldTypes eYield, int iAmount, int iSlot)
	{
		switch (eYield)
		{
		case YIELD_EDUCATION:
			{
				const UnitClassTypes eClass = (UnitClassTypes)kUnit.getUnitInfo().getUnitClassType();
				if (eClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_COLONIST"))
				{
					return iAmount * 1000;
				}
				if (eClass == (UnitClassTypes)GC.getInfoTypeForString("UNITCLASS_INDENTURED_SERVANT"))
				{
					return iAmount * 600;
				}
				return 0;
			}
		case YIELD_HEALTH:
			if (kCity.getCityHealthChange() >= 2)
			{
				return iAmount * 100;
			}
			break;
		case YIELD_LAW:
			if (kCity.getCityLaw() >= 15)
			{
				return iAmount * 100;
			}
			break;
		case YIELD_BELLS:
			return iAmount * 900;
		case YIELD_HAPPINESS:
			{
				// (prostitutes) 15 gold in a colony with less than 1 happiness above its unhappiness, rising to 20 gold with more
				// than 3 unhappiness above its happiness; 8 gold with more than 10 happiness above its unhappiness
				// (with the colonists of the job automation on their way there: their population, the happiness of those who will
				// make it)
				int iNewCitizens = 0;
				int iHappinessOnWay = 0;
				const CvPlayerAI& kOwner = GET_PLAYER(kCity.getOwnerINLINE());
				int iLoop;
				for (CvUnit* pLoopUnit = kOwner.firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = kOwner.nextUnit(&iLoop))
				{
					const CvUnitAI& kOther = static_cast<const CvUnitAI&>(*pLoopUnit);
					const ProfessionTypes eOtherJob = kOther.AI_getWfProfession();
					if (&kOther == &kUnit || kOther.AI_getWfTask() != CvUnitAI::WF_ASSIGN || kOther.AI_getWfCityID() != kCity.getID() || eOtherJob == NO_PROFESSION)
					{
						continue;
					}
					if (kOther.AI_getWfVillageX() < 0)
					{
						++iNewCitizens;
					}
					if ((YieldTypes)GC.getProfessionInfo(eOtherJob).getYieldsProduced(0) == YIELD_HAPPINESS)
					{
						iHappinessOnWay += wfOutput(kCity, eOtherJob, pLoopUnit->getUnitType(), NULL);
					}
				}
				const int iBalance = iHappinessOnWay - wfUnhappinessWith(kCity, iNewCitizens);
				if (iBalance < 1)
				{
					return iAmount * std::min(2000, 1500 + 125 * -iBalance);
				}
				if (iBalance > 10)
				{
					return iAmount * 800;
				}
			}
			break;
		default:
			break;
		}
		return wfYieldGold(kCity, eYield, iAmount, iSlot);
	}

	int wfHumanJobValue(const CvCity& kCity, const CvUnitAI& kUnit, ProfessionTypes eProfession, const CvPlot* pPlot, int iSlot, UnitTypes eColonist, int* piFood)
	{
		const UnitTypes eType = wfWorkingType(kUnit, eProfession, eColonist);
		const int iOutput = wfOutput(kCity, eProfession, eType, pPlot);
		if (iOutput <= 0)
		{
			return -1;
		}
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
		const YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
		const int iFood = wfFoodOutput(eProfession, iOutput);
		if (piFood != NULL)
		{
			*piFood = iFood;
		}
		int iGold = (eYield == YIELD_FOOD) ? 0 : wfHumanYieldGold(kCity, kUnit, eYield, iOutput, iSlot);
		for (int k = 0; k < kProfession.getNumYieldsConsumed(); ++k)
		{
			const YieldTypes eInput = (YieldTypes)kProfession.getYieldsConsumed(k);
			if (eInput != NO_YIELD)
			{
				iGold -= wfInputGold(kCity, eYield, eInput, iOutput);
			}
		}
		return std::max(0, iGold) + iFood * WF_FOOD_GOLD;
	}
}

// WTP: colonists with the job automation on their way to kCity as new citizens (not replacing anybody)
int CvPlayerAI::AI_humanIncomingCitizens(const CvCity& kCity) const
{
	int iCount = 0;
	int iLoop;
	for (CvUnit* pLoopUnit = firstUnit(&iLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iLoop))
	{
		const CvUnitAI& kOther = static_cast<const CvUnitAI&>(*pLoopUnit);
		if (kOther.AI_getWfTask() == CvUnitAI::WF_ASSIGN && kOther.AI_getWfCityID() == kCity.getID() && kOther.AI_getWfProfession() != NO_PROFESSION
			&& kOther.AI_getWfVillageX() < 0)
		{
			++iCount;
		}
	}
	return iCount;
}

// WTP: the unhappiness balance of kCity (unhappiness minus happiness) with iNewCitizens more citizens
int CvPlayerAI::AI_humanUnhappinessWith(const CvCity& kCity, int iNewCitizens) const
{
	return wfUnhappinessWith(kCity, iNewCitizens);
}

namespace
{
	bool humanJobBetter(const CvPlayerAI::HumanJob& kA, const CvPlayerAI::HumanJob& kB)
	{
		return kA.iScore > kB.iScore;
	}
}

// WTP: the best job for a human player's colonist (see AI_humanFindJobs); returns false if there is none
bool CvPlayerAI::AI_humanFindJob(const CvUnit& kUnit, int& iCityID, ProfessionTypes& eBestProfession, int& iX, int& iY, int& iDisplaceID, int& iTurns, int& iGold)
{
	std::vector<HumanJob> aJobs;
	AI_humanFindJobs(kUnit, aJobs, 1);
	if (aJobs.empty())
	{
		return false;
	}
	iCityID = aJobs[0].iCityID;
	eBestProfession = aJobs[0].eProfession;
	iX = aJobs[0].iX;
	iY = aJobs[0].iY;
	iDisplaceID = aJobs[0].iDisplaceID;
	iTurns = aJobs[0].iTurns;
	iGold = aJobs[0].iGold;
	return true;
}

// WTP: the best jobs for a human player's colonist (the best per colony and profession, best first, at most iMaxJobs): a free
// plot or building slot in one of our colonies, or the job of a regular colonist in the expert's own trade (that colonist
// makes room). Regular colonists only within 12 turns; the colony keeps 2 food to spare unless the job produces food; a
// building job only if the colony produces its raw material. A new citizen only if the colony's unhappiness stays at most
// 2 above its happiness with all colonists on their way there. Value per turn (gold x100, wfHumanJobValue), less for a long way.
// paFoodJobs: for slaves, indentured servants, petty criminals and expert farmers and fishermen also the best food job (net
// 3 food or more) in each colony short of food (less than +4 food with those on their way, storage below half the growth
// threshold), the 3 best of them; such jobs are marked in aJobs too
void CvPlayerAI::AI_humanFindJobs(const CvUnit& kUnit, std::vector<HumanJob>& aJobs, int iMaxJobs, std::vector<HumanJob>* paFoodJobs)
{
	const CvUnitAI& kUnitAI = static_cast<const CvUnitAI&>(kUnit);
	const CvCivilizationInfo& kCiv = GC.getCivilizationInfo(getCivilizationType());
	const UnitTypes eColonist = kCiv.getCivilizationUnits(GLOBAL_DEFINE_DEFAULT_POPULATION_UNIT);
	const int iConsumption = GLOBAL_DEFINE_FOOD_CONSUMPTION_PER_POPULATION;
	const bool bNoShip = !AI_hasWfShip();
	AI_wfEnsureTravel(kUnit);

	// the best job per (colony, profession)
	std::map<std::pair<int, int>, HumanJob> mapJobs;
	// colonies short of food, and whether this worker is offered food jobs there
	std::set<int> aFoodShortCities;
	const UnitClassTypes eUnitClass = (UnitClassTypes)kUnit.getUnitInfo().getUnitClassType();
	const char* const aszFoodWorkers[7] = { "UNITCLASS_AFRICAN_SLAVE", "UNITCLASS_NATIVE_SLAVE", "UNITCLASS_INDENTURED_SERVANT", "UNITCLASS_CRIMINAL",
		"UNITCLASS_FARMER", "UNITCLASS_RICE_FARMER", "UNITCLASS_FISHERMAN" };
	bool bFoodWorker = false;
	for (int k = 0; k < 7 && !bFoodWorker; ++k)
	{
		bFoodWorker = eUnitClass == (UnitClassTypes)GC.getInfoTypeForString(aszFoodWorkers[k]);
	}

	// the jobs other colonists with the job automation are on their way to: their plots, their building slots (per colony and
	// building) and the colonists they replace
	std::set<std::pair<int, int> > aReservedPlots; // (city ID, plot number)
	std::map<std::pair<int, int>, int> mapReservedSlots; // (city ID, special building) -> colonists on their way
	std::set<int> aReplaced; // unit IDs
	std::map<int, int> mapIncoming; // city ID -> new citizens on their way (not replacing anybody)
	{
		int iUnitLoop;
		for (CvUnit* pLoopUnit = firstUnit(&iUnitLoop); pLoopUnit != NULL; pLoopUnit = nextUnit(&iUnitLoop))
		{
			const CvUnitAI& kOther = static_cast<const CvUnitAI&>(*pLoopUnit);
			if (pLoopUnit == &kUnit || kOther.AI_getWfTask() != CvUnitAI::WF_ASSIGN || kOther.AI_getWfProfession() == NO_PROFESSION)
			{
				continue;
			}
			if (kOther.AI_getWfPlotX() >= 0)
			{
				aReservedPlots.insert(std::make_pair(kOther.AI_getWfCityID(), GC.getMap().plotNumINLINE(kOther.AI_getWfPlotX(), kOther.AI_getWfPlotY())));
			}
			else
			{
				++mapReservedSlots[std::make_pair(kOther.AI_getWfCityID(), GC.getProfessionInfo(kOther.AI_getWfProfession()).getSpecialBuilding())];
			}
			if (kOther.AI_getWfVillageX() >= 0)
			{
				aReplaced.insert(kOther.AI_getWfVillageX());
			}
			else
			{
				++mapIncoming[kOther.AI_getWfCityID()];
			}
		}
	}

	std::vector<ProfessionTypes> aePlot;
	std::vector<ProfessionTypes> aeBuilding;
	for (ProfessionTypes eProfession = FIRST_PROFESSION; eProfession < NUM_PROFESSION_TYPES; ++eProfession)
	{
		const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
		if (!kProfession.isCitizen() || !kCiv.isValidProfession(eProfession) || (YieldTypes)kProfession.getYieldsProduced(0) == NO_YIELD
			|| !isProfessionValid(eProfession, kUnit.getUnitType()))
		{
			continue;
		}
		(kProfession.isWorkPlot() ? aePlot : aeBuilding).push_back(eProfession);
	}

	int iLoop;
	for (CvCity* pCity = firstCity(&iLoop); pCity != NULL; pCity = nextCity(&iLoop))
	{
		if (AI_isWfUnreachable(kUnit.getID(), pCity->getID()))
		{
			continue;
		}
		int iPort = -1;
		// (also by ship and on foot from another colony: a colony inland on another coast)
		const int iCityTurns = AI_wfTravelTurns(*kUnit.plot(), *pCity, &iPort, bNoShip, true);
		if (iCityTurns == MAX_INT)
		{
			continue;
		}
		const int iSpare = pCity->foodDifference() - iConsumption;
		// a new citizen only if the colony's unhappiness stays at most 2 above its happiness (with those on their way)
		static const int WF_MAX_UNHAPPINESS_BALANCE = 2;
		const std::map<int, int>::const_iterator itIncoming = mapIncoming.find(pCity->getID());
		const int iIncoming = (itIncoming != mapIncoming.end()) ? itIncoming->second : 0;
		const bool bCrowded = wfUnhappinessWith(*pCity, 1 + iIncoming) > WF_MAX_UNHAPPINESS_BALANCE;
		// short of food: less than +4 food (with those on their way) and the storage below half the growth threshold
		if (bFoodWorker && pCity->foodDifference() - iConsumption * iIncoming < 4 && pCity->getFood() * 2 < pCity->growthThreshold())
		{
			aFoodShortCities.insert(pCity->getID());
		}

		// plots
		FOREACH(CityPlot)
		{
			if (eLoopCityPlot == CITY_HOME_PLOT)
			{
				continue;
			}
			CvPlot* const pLoopPlot = pCity->getCityIndexPlot(eLoopCityPlot);
			if (pLoopPlot == NULL || (pLoopPlot->getWorkingCity() != pCity && pLoopPlot->getWorkingCity() != NULL))
			{
				continue;
			}
			CvUnit* const pHolder = pCity->getUnitWorkingPlot(eLoopCityPlot);
			if (aReservedPlots.count(std::make_pair(pCity->getID(), GC.getMap().plotNumINLINE(pLoopPlot->getX_INLINE(), pLoopPlot->getY_INLINE()))) > 0
				|| (pHolder != NULL && aReplaced.count(pHolder->getID()) > 0))
			{
				continue;
			}
			if (pHolder == NULL && (bCrowded || !pCity->canWork(pLoopPlot)))
			{
				continue;
			}
			for (uint i = 0; i < aePlot.size(); ++i)
			{
				const ProfessionTypes eProfession = aePlot[i];
				const bool bExpert = wfIsExpertFor(kUnitAI, eProfession);
				if ((iCityTurns > 12 && !bExpert) || (pHolder != NULL && (!bExpert || pHolder->getProfession() != eProfession
					|| wfIsExpertFor(static_cast<const CvUnitAI&>(*pHolder), eProfession))))
				{
					continue;
				}
				int iFood = 0;
				const int iValue = wfHumanJobValue(*pCity, kUnitAI, eProfession, pLoopPlot, 0, eColonist, &iFood);
				if (iValue <= 0 || (iFood <= 0 && iSpare < 2 && pHolder == NULL))
				{
					continue;
				}
				// the expert takes the job of a regular colonist only if it earns more there
				if (pHolder != NULL && iValue <= wfHumanJobValue(*pCity, static_cast<const CvUnitAI&>(*pHolder), eProfession, pLoopPlot, 0, eColonist, NULL))
				{
					continue;
				}
				const int iScore = iValue * 20 / (20 + iCityTurns);
				const std::pair<int, int> kKey(pCity->getID(), (int)eProfession);
				const std::map<std::pair<int, int>, HumanJob>::const_iterator itJob = mapJobs.find(kKey);
				if (iScore > 0 && (itJob == mapJobs.end() || iScore > itJob->second.iScore))
				{
					HumanJob kJob;
					kJob.iScore = iScore;
					kJob.iCityID = pCity->getID();
					kJob.eProfession = eProfession;
					kJob.iX = pLoopPlot->getX_INLINE();
					kJob.iY = pLoopPlot->getY_INLINE();
					kJob.iDisplaceID = (pHolder != NULL) ? pHolder->getID() : -1;
					kJob.iTurns = iCityTurns;
					kJob.iGold = iValue / 100;
					kJob.iFood = iFood;
					kJob.bFoodShortage = false;
					mapJobs[kKey] = kJob;
				}
			}
		}

		// buildings
		for (uint i = 0; i < aeBuilding.size(); ++i)
		{
			const ProfessionTypes eProfession = aeBuilding[i];
			const CvProfessionInfo& kProfession = GC.getProfessionInfo(eProfession);
			const YieldTypes eYield = (YieldTypes)kProfession.getYieldsProduced(0);
			const bool bExpert = wfIsExpertFor(kUnitAI, eProfession);
			// (an expert never studies)
			if ((iCityTurns > 12 && !bExpert) || !wfMayWorkInBuilding(kUnit, eYield) || (eYield == YIELD_EDUCATION && wfIsExpert(kUnitAI)))
			{
				continue;
			}
			// the slots of the building are shared by all its professions (coffee, cocoa and peanut roasters, ...)
			const int iSlots = pCity->getNumProfessionBuildingSlots(eProfession);
			const int iSpecial = kProfession.getSpecialBuilding();
			const std::map<std::pair<int, int>, int>::const_iterator itReserved = mapReservedSlots.find(std::make_pair(pCity->getID(), iSpecial));
			int iUsed = (itReserved != mapReservedSlots.end()) ? itReserved->second : 0;
			CvUnit* pRegular = NULL;
			for (int iPop = 0; iPop < pCity->getPopulation(); ++iPop)
			{
				CvUnit* const pCitizen = pCity->getPopulationUnitByIndex(iPop);
				if (pCitizen != NULL && pCitizen->getProfession() != NO_PROFESSION && GC.getProfessionInfo(pCitizen->getProfession()).getSpecialBuilding() == iSpecial
					&& !GC.getProfessionInfo(pCitizen->getProfession()).isWorkPlot())
				{
					++iUsed;
					if (pRegular == NULL && pCitizen->getProfession() == eProfession && !wfIsExpertFor(static_cast<const CvUnitAI&>(*pCitizen), eProfession)
						&& aReplaced.count(pCitizen->getID()) == 0)
					{
						pRegular = pCitizen;
					}
				}
			}
			CvUnit* const pHolder = (iUsed < iSlots) ? NULL : (bExpert ? pRegular : NULL);
			if (iSlots <= 0 || (iUsed >= iSlots && pHolder == NULL) || (bCrowded && pHolder == NULL))
			{
				continue;
			}
			// the raw material must be produced in the colony (the stock runs out)
			const int iOutput = wfOutput(*pCity, eProfession, wfWorkingType(kUnitAI, eProfession, eColonist), NULL);
			bool bInput = true;
			for (int k = 0; k < kProfession.getNumYieldsConsumed() && bInput; ++k)
			{
				const YieldTypes eInput = (YieldTypes)kProfession.getYieldsConsumed(k);
				bInput = eInput == NO_YIELD || pCity->calculateNetYield(eInput) + (pHolder != NULL ? iOutput : 0) >= std::max(1, iOutput / 2);
			}
			if (!bInput)
			{
				continue;
			}
			int iFood = 0;
			const int iSlot = (pHolder != NULL) ? iUsed - 1 : iUsed;
			const int iValue = wfHumanJobValue(*pCity, kUnitAI, eProfession, NULL, iSlot, eColonist, &iFood);
			if (iValue <= 0 || (iFood <= 0 && iSpare < 2 && pHolder == NULL))
			{
				continue;
			}
			if (pHolder != NULL && iValue <= wfHumanJobValue(*pCity, static_cast<const CvUnitAI&>(*pHolder), eProfession, NULL, iSlot, eColonist, NULL))
			{
				continue;
			}
			const int iScore = iValue * 20 / (20 + iCityTurns);
			const std::pair<int, int> kKey(pCity->getID(), (int)eProfession);
			const std::map<std::pair<int, int>, HumanJob>::const_iterator itJob = mapJobs.find(kKey);
			if (iScore > 0 && (itJob == mapJobs.end() || iScore > itJob->second.iScore))
			{
				HumanJob kJob;
				kJob.iScore = iScore;
				kJob.iCityID = pCity->getID();
				kJob.eProfession = eProfession;
				kJob.iX = -1;
				kJob.iY = -1;
				kJob.iDisplaceID = (pHolder != NULL) ? pHolder->getID() : -1;
				kJob.iTurns = iCityTurns;
				kJob.iGold = iValue / 100;
				kJob.iFood = iFood;
				kJob.bFoodShortage = false;
				mapJobs[kKey] = kJob;
			}
		}
	}
	// food jobs in colonies short of food: net 3 food or more (a new citizen eats too), the best per colony
	std::map<int, HumanJob> mapFoodJobs;
	for (std::map<std::pair<int, int>, HumanJob>::iterator it = mapJobs.begin(); it != mapJobs.end(); ++it)
	{
		HumanJob& kJob = it->second;
		const int iNetFood = kJob.iFood - ((kJob.iDisplaceID < 0) ? iConsumption : 0);
		if (aFoodShortCities.count(kJob.iCityID) > 0 && iNetFood >= 3)
		{
			kJob.bFoodShortage = true;
			const std::map<int, HumanJob>::const_iterator itFood = mapFoodJobs.find(kJob.iCityID);
			if (itFood == mapFoodJobs.end() || kJob.iScore > itFood->second.iScore)
			{
				mapFoodJobs[kJob.iCityID] = kJob;
			}
		}
	}
	if (paFoodJobs != NULL)
	{
		paFoodJobs->clear();
		for (std::map<int, HumanJob>::const_iterator it = mapFoodJobs.begin(); it != mapFoodJobs.end(); ++it)
		{
			paFoodJobs->push_back(it->second);
		}
		std::stable_sort(paFoodJobs->begin(), paFoodJobs->end(), humanJobBetter);
		if (paFoodJobs->size() > 3)
		{
			paFoodJobs->resize(3);
		}
	}
	aJobs.clear();
	for (std::map<std::pair<int, int>, HumanJob>::const_iterator it = mapJobs.begin(); it != mapJobs.end(); ++it)
	{
		aJobs.push_back(it->second);
	}
	std::stable_sort(aJobs.begin(), aJobs.end(), humanJobBetter);
	// at most the 2 best student jobs (learning is worth so much that they would fill the list)
	int iStudentJobs = 0;
	for (uint i = 0; i < aJobs.size(); )
	{
		if ((YieldTypes)GC.getProfessionInfo(aJobs[i].eProfession).getYieldsProduced(0) == YIELD_EDUCATION && ++iStudentJobs > 2)
		{
			aJobs.erase(aJobs.begin() + i);
		}
		else
		{
			++i;
		}
	}
	if ((int)aJobs.size() > iMaxJobs)
	{
		aJobs.resize(iMaxJobs);
	}
}

// WTP: the colonist has arrived in the colony: it joins it in its job (a regular colonist it replaces is put on the colony
// plot, awake) and is locked there. Returns false if the job is not free any more or the colonist cannot join
bool CvPlayerAI::AI_humanSeat(CvUnit& kUnit, CvCity& kCity, ProfessionTypes eProfession, int iX, int iY, int iDisplaceID)
{
	if (!kUnit.canJoinCity(kCity.plot(), false, true))
	{
		return false;
	}
	const ProfessionTypes eDefaultProfession = GC.getCivilizationInfo(getCivilizationType()).getDefaultProfession();
	CvPlot* const pPlot = (iX >= 0) ? GC.getMap().plotINLINE(iX, iY) : NULL;
	const CityPlotTypes ePlot = (pPlot != NULL) ? kCity.getCityPlotIndex(pPlot) : NO_CITY_PLOT;
	if (pPlot != NULL && ePlot == NO_CITY_PLOT)
	{
		return false;
	}
	CvUnit* pDisplaced = NULL;
	if (pPlot != NULL)
	{
		CvUnit* const pHolder = kCity.getUnitWorkingPlot(ePlot);
		if (pHolder != NULL && pHolder->getID() != iDisplaceID)
		{
			return false;
		}
		if (pHolder == NULL && !kCity.canWork(pPlot))
		{
			return false;
		}
		pDisplaced = pHolder;
	}
	else
	{
		// the slots of the building are shared by all its professions
		const int iSpecial = GC.getProfessionInfo(eProfession).getSpecialBuilding();
		int iUsed = 0;
		for (int iPop = 0; iPop < kCity.getPopulation(); ++iPop)
		{
			CvUnit* const pCitizen = kCity.getPopulationUnitByIndex(iPop);
			if (pCitizen != NULL && pCitizen->getProfession() != NO_PROFESSION && GC.getProfessionInfo(pCitizen->getProfession()).getSpecialBuilding() == iSpecial
				&& !GC.getProfessionInfo(pCitizen->getProfession()).isWorkPlot())
			{
				++iUsed;
				if (pCitizen->getID() == iDisplaceID && pCitizen->getProfession() == eProfession)
				{
					pDisplaced = pCitizen;
				}
			}
		}
		if (iUsed >= kCity.getNumProfessionBuildingSlots(eProfession) && pDisplaced == NULL)
		{
			return false;
		}
	}

	kCity.addPopulationUnit(&kUnit, NO_PROFESSION);
	if (pDisplaced != NULL)
	{
		if (pPlot != NULL)
		{
			kCity.clearUnitWorkingPlot(ePlot);
		}
		pDisplaced->setColonistLocked(false);
		pDisplaced->setProfession(NO_PROFESSION);
		if (kCity.removePopulationUnit(CREATE_ASSERT_DATA, pDisplaced, false, eDefaultProfession))
		{
			pDisplaced->AI_setUnitAIType(UNITAI_COLONIST);
			CvWString szBuffer = gDLL->getText("TXT_KEY_AUTO_ASSIGN_DISPLACED", pDisplaced->getNameKey(), kCity.getNameKey(), kUnit.getNameKey());
			gDLL->UI().addPlayerMessage(getID(), true, GC.getEVENT_MESSAGE_TIME(), szBuffer, "AS2D_POSITIVE_DINK", MESSAGE_TYPE_INFO, pDisplaced->getButton(), COLOR_WHITE, kCity.getX_INLINE(), kCity.getY_INLINE());
		}
	}
	if (kUnit.canHaveProfession(eProfession, false))
	{
		kUnit.setProfession(eProfession);
		if (pPlot != NULL)
		{
			kCity.AI().AI_setUnitWorkingPlot(ePlot, kUnit.getID());
		}
	}
	kUnit.setColonistLocked(true);
	CvWString szBuffer = gDLL->getText("TXT_KEY_AUTO_ASSIGN_JOINED", kUnit.getNameKey(), GC.getProfessionInfo(eProfession).getTextKeyWide(), kCity.getNameKey());
	gDLL->UI().addPlayerMessage(getID(), true, GC.getEVENT_MESSAGE_TIME(), szBuffer, "AS2D_POSITIVE_DINK", MESSAGE_TYPE_INFO, kUnit.getButton(), COLOR_GREEN, kCity.getX_INLINE(), kCity.getY_INLINE());
	return true;
}
