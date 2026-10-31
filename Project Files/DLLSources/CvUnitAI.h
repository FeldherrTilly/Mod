#pragma once

// unitAI.h

#ifndef CIV4_UNIT_AI_H
#define CIV4_UNIT_AI_H

#include "CvUnit.h"

namespace
{
	struct SailToHelper
	{
		SailToHelper(UnitTravelStates unitTravelState_, AutomateTypes automateType_, MissionAITypes missionAI_) :
			unitTravelState(unitTravelState_), automateType(automateType_), missionAI(missionAI_) {}

		const UnitTravelStates unitTravelState;
		const AutomateTypes automateType;
		const MissionAITypes missionAI;
	};

	const SailToHelper kSailToEurope(UNIT_TRAVEL_STATE_TO_EUROPE, AUTOMATE_SAIL_TO_EUROPE, MISSIONAI_SAIL_TO_EUROPE);
	const SailToHelper kSailToAfrica(UNIT_TRAVEL_STATE_TO_AFRICA, AUTOMATE_SAIL_TO_AFRICA, MISSIONAI_SAIL_TO_AFRICA);
	const SailToHelper kSailToPortRoyal(UNIT_TRAVEL_STATE_TO_PORT_ROYAL, AUTOMATE_SAIL_TO_PORT_ROYAL, MISSIONAI_SAIL_TO_PORT_ROYAL);
}

class CvCity;
class CvSelectionGroupAI;

class CvUnitAI : public CvUnit
{

public:

	CvUnitAI();
	virtual ~CvUnitAI();

	void AI_init(int iBirthmark);
	void AI_uninit();
	void AI_reset();

	bool AI_update();
	bool AI_europeUpdate();
	bool AI_follow();

	void AI_upgrade();

	void AI_promote();

	int AI_groupFirstVal();
	int AI_groupSecondVal();

	int AI_attackOdds(const CvPlot* pPlot, bool bPotentialEnemy) const;

	bool AI_bestCityBuild(CvCity* pCity, CvPlot** ppBestPlot = NULL, BuildTypes* peBestBuild = NULL, CvPlot* pIgnorePlot = NULL, CvUnit* pUnit = NULL);

	bool AI_isCityAIType() const;

	int AI_getBirthmark() const;
	void AI_setBirthmark(int iNewValue);

	UnitAITypes AI_getUnitAIType() const;
	void AI_setUnitAIType(UnitAITypes eNewValue);
	CvSelectionGroupAI const* AI_getGroup() const; // advc.003u
	CvSelectionGroupAI* AI_getGroup(); // advc.003u
	UnitAIStates AI_getUnitAIState() const;
	void AI_setUnitAIState(UnitAIStates eNewValue);

	int AI_sacrificeValue(const CvPlot* pPlot) const;

	CvPlot* AI_determineDestination(CvPlot** ppMissionPlot, MissionTypes* peMission, MissionAITypes* peMissionAI);

	bool AI_moveFromTransport(CvPlot* pHintPlot);
	bool AI_attackFromTransport(CvPlot* pHintPlot, int iLowOddsThreshold, int iHighOddsThreshold);
	int AI_getMovePriority() const;
	void AI_setMovePriority(int iNewValue);
	bool AI_hasAIChanged(int iNumTurns) const;
	int AI_getLastAIChangeTurn() const;

	void AI_doInitialMovePriority();
	void AI_doFound();
	
	ProfessionTypes AI_getOldProfession() const;
	void AI_setOldProfession(ProfessionTypes eProfession);
	ProfessionTypes AI_getIdealProfession() const;

	bool AI_loadAdjacent(CvPlot* pPlot, bool bTestCity);
	bool AI_allowedToJoin(const CvCity& kCity) const;
	bool AI_mayPioneerJoinCity() const;

	void read(CvSavegameReader reader);
	void write(CvSavegameWriter writer);

	//TAC Whaling, ray
	bool AI_transportReturnToPort(bool bUnload, CvCity* pCity);
	bool AI_moveToCity(bool bUnload, CvCity* pCity);
	//End TAC Whaling, ray
	bool AI_africa();

	bool is(UnitAIStates) const;
	bool is(UnitAITypes) const;

	// WTP: workforce plan task of a colonist (set by CvPlayerAI::AI_doWorkforcePlan, saved)
	enum WorkforceTask
	{
		WF_NONE = 0,
		WF_EXPERT_JOB = 1, // work profession eProfession in city (on plot X/Y, or in a building if X < 0)
		WF_RETRAIN = 2,    // give up the specialty, then learn in the village and do the job
		WF_LEARN = 3,      // learn in the village at VillageX/Y, then do the job
		WF_JOIN_CITY = 4,  // move to the city and join it (it has food to spare; the plan keeps this task)
		WF_FOOD_LEARN = 5, // learn a food expert in the village at VillageX/Y, then work the food job in the city (kept)
		WF_WAIT = 6,       // no job: go to the city and wait on its plot (the workforce plan seats it when a job is free)
		WF_ASSIGN = 7,     // human player's colonist (AUTOMATE_ASSIGN_JOB): go to the city and join it in the job (VillageX: the unit it replaces, VillageY: plot number where the ship puts it ashore)
		WF_TRAVEL = 8,     // human player's land unit (AUTOMATE_TRAVEL): go to plot X/Y (city: CityID), by ship to VillageX/Y if set
	};
	int AI_getWfTask() const { return m_iWfTask; }
	int AI_getWfCityID() const { return m_iWfCityID; }
	ProfessionTypes AI_getWfProfession() const { return m_eWfProfession; }
	int AI_getWfPlotX() const { return m_iWfPlotX; }
	int AI_getWfPlotY() const { return m_iWfPlotY; }
	int AI_getWfVillageX() const { return m_iWfVillageX; }
	int AI_getWfVillageY() const { return m_iWfVillageY; }
	void AI_setWfTask(int iTask, int iCityID, ProfessionTypes eProfession, int iPlotX, int iPlotY, int iVillageX, int iVillageY);
	void AI_clearWfTask();
	void AI_copyWfTask(const CvUnitAI& kOther);
	bool AI_isWfJob(const CvCity& kCity, ProfessionTypes eProfession, const CvPlot* pPlot) const;
	bool AI_doWfTask();

	// WTP: transport to a city or plot (human player's land unit): it walks or takes an automated transport
	void AI_startTravel(int iX, int iY);
	bool AI_doTravel();
	enum TravelStep
	{
		TRAVEL_ON_WAY,
		TRAVEL_ARRIVED,
		TRAVEL_NO_WAY,
	};
	TravelStep AI_travelStep(CvPlot& kTarget, bool bAnnounceWalk);
	CvPlot* AI_travelDrop() const;
	void AI_setTravelDrop(const CvPlot* pDrop);
	CvPlot* AI_travelPickup() const;
	void AI_setTravelPickup(const CvPlot* pPickup);
	int AI_travelShipApproach(const CvUnit& kShip, const CvPlot& kPickup) const;
	void AI_travelChooseShip();
	void AI_travelBoardMessage(const CvUnit& kShip);
	bool AI_travelPlan(const CvPlot& kTarget, CvPlot*& pPickup, CvPlot*& pDrop, int& iShipID, int& iTurns);
	int AI_getTravelShipID() const { return m_iTravelShipID; }

	// WTP: pioneer plan: a human player's pioneer builds the improvements the player marked (AUTOMATE_PIONEER_PLAN)
	bool AI_doPioneerPlan();
	// WTP: private pioneer: a hardy pioneer the player paid for builds one improvement and leaves (AUTOMATE_PRIVATE_BUILD);
	// the player cannot command it
	BuildTypes AI_getPrivateBuild() const { return (BuildTypes)m_iPrivateBuild; }
	void AI_setPrivateBuild(BuildTypes eBuild) { m_iPrivateBuild = (int)eBuild; }
	void AI_doPrivateBuild();
	// WTP: patrol of a human player's military unit or warship (AUTOMATE_PATROL) around the plot where it was switched on
	void AI_setPatrolCenter(const CvPlot* pPlot);
	bool AI_doPatrol();
	bool AI_isPatrolTarget(const CvUnit& kTarget, const CvPlot& kPlot) const;
	// WTP: treasure automation of a human player: a treasure (AUTOMATE_TREASURE) moves like the AI's treasures, a galleon or
	// West Indiaman (AUTOMATE_TREASURE_SHIP) fetches the treasures waiting for a ship like the AI's transports and takes them
	// to Europe; the ship keeps the role while the automation of its group is lost on the way to or from Europe
	bool AI_isTreasureShip() const { return m_bTreasureShip; }
	void AI_setTreasureShip(bool bNewValue) { m_bTreasureShip = bNewValue; }
	void AI_treasureShipMove();
	bool AI_treasureShipEurope();

	// WTP: native trade automation of a human player's ship: it trades with the villages around its home colony (the nearest
	// colony when the automation was switched on) and comes back there from Europe
	void AI_startNativeTrade();
	// WTP: a transport that has done nothing in Europe this turn: logs its state and runs its Europe routine
	void AI_europeIdleFallback();
	bool AI_isHumanNativeTrade() const;
	bool AI_hasNativeTradeAutomation() const; // a human player's native trade ship (home colony and trade state)
	bool AI_isNativeVillageTargeted(const CvCity& kVillage) const;
	CvCity* AI_nativeTradeHomeCity() const;
	bool AI_nativeTradeInHomeArea(const CvCity& kVillage) const;
	bool AI_humanNativeTradeEurope();
	CvCity* AI_nativeTradeFindHome() const;
	void AI_clearNativeTradeHome() { m_iNativeTradeHomeCityID = -1; }
	// WTP: native supply automation of a human player's coastal ship (AUTOMATE_NATIVE_SUPPLY): it buys goods in the villages
	// around its home colony (the nearest colony when switched on), brings them there and starts again; no trips to Europe
	void AI_startNativeSupply();
	bool AI_isNativeSupplyShip() const;
	void AI_nativeSupplyMove();
	static const int NATIVE_SUPPLY_TRIP_TURNS = 10; // WTP: the native supply ship brings what it has home this many turns after it left
	// WTP: automatic job assignment of a human player's colonist
	bool AI_startAutoAssign(int iCityID = -1, ProfessionTypes eProfession = NO_PROFESSION);
	bool AI_doAutoAssign();
	bool AI_transportAutoAssignColonists();

protected:

	void AI_resetSavedData();

	int m_iBirthmark;
	int m_iMovePriority;
	int m_iLastAIChangeTurn;

	UnitAITypes m_eUnitAIType;

	UnitAIStates m_eUnitAIState;
	ProfessionTypes m_eOldProfession;

	int m_iAutomatedAbortTurn;
	// WTP: workforce plan task
	int m_iWfTask;
	int m_iWfCityID;
	ProfessionTypes m_eWfProfession;
	int m_iWfPlotX;
	int m_iWfPlotY;
	int m_iWfVillageX;
	int m_iWfVillageY;
	int m_iTravelShipID; // WTP: transport to a city or plot: the ship that takes the unit (-1: it walks)
	int m_iTravelPickup; // WTP: transport to a city or plot: plot number of the port where the ship picks it up (-1: none)
	int m_iPatrolCenter; // WTP: patrol: plot number of the plot it patrols around (-1: none)
	int m_iPrivateBuild; // WTP: private pioneer: the build it was paid for (NO_BUILD: none)
	bool m_bTreasureShip; // WTP: treasure automation (human): the ship fetches treasures
	int m_iNativeTradeBuyTurn; // WTP: native trade ship: turn of its last purchase at a village
	int m_iNativeTradeArriveTurn; // WTP: native trade ship: turn of its first move on the map after Europe (-1: not yet)
	int m_iNativeTradeSlotsFullTurn; // WTP: native trade ship: turn all its cargo slots were taken (-1: not yet)
	int m_iNativeTradeHomeCityID; // WTP: native trade automation (human): the colony it trades around (-1: none)

	mutable ProfessionTypes m_eIdealProfessionCache;

	// < JAnimals Mod Start >
	void AI_animalMove();
	bool AI_animalAttack();
	bool AI_animalLandPatrol();
	bool AI_animalSeaPatrol();
	// < JAnimals Mod End >
	void AI_FleeingMove(); // R&R, ray, Fleeing Units
	void AI_colonistMove();
	void AI_settlerMove();
	void AI_workerMove();
	void AI_missionaryMove();
	void AI_nativeTraderMove(); // WTP, ray, Native Trade Posts - START
	void AI_scoutMove();
	void AI_treasureMove();
	void AI_yieldUhMove();
	void AI_generalMove();
	
	void AI_offensiveMove();
	// TAC - AI Attack City - koma13 - START
	void AI_attackCityMove();
	// TAC - AI Attack City - koma13 - END

	void AI_defensiveMove();
	void AI_counterMove();
	
	void AI_defensiveBraveMove();
	void AI_offensiveBraveMove();
	void AI_counterBraveMove();
	
	void AI_transportMove();
	void AI_transportMoveRoutes();
	void AI_transportMoveFull();
	void AI_pirateMove();

	void AI_imperialShipMove();
	void AI_imperialSoldierMove();
	void AI_imperialMountedMove();
	void AI_imperialCannonMove();
	//TAC Whaling, ray
	void AI_workerSeaMove();
	//End TAC Whaling, ray
	void AI_transportSeaMove();
	void AI_imperialTransportMove(); // WTP
	void AI_logShipTarget();
	void AI_logSettler();
	bool AI_sailForAfricaUnits();
	static const char* s_szWtpShipReason; // WTP: why a transport chose its target this turn (ship log)
	void AI_transportCoastMove();
	void AI_assaultSeaMove();
	void AI_escortSeaMove();	// TAC - AI Escort Sea - koma13
	void AI_combatSeaMove();
	bool AI_joinGreatAdmiral();

	void AI_exploreMove();
	void AI_exploreSeaMove();

	void AI_networkAutomated();
	void AI_cityAutomated();

	// R&R, ray, Natives raiding party - START
	bool AI_travelToEuropeColonyRaidTarget(int iMinAttitude, int iMaxAttitude, int iRange = -1, int iFlags = 0);
	// R&R, ray, Natives raiding party - END

	bool AI_travelToEuropeColony(int iMinAttitude, int iMaxAttitude, int iRange = -1);
	bool AI_guardHomeColony();
	bool AI_findNewHomeColony();
	bool AI_breakOversizedNativeDefensiveGroup();

	bool AI_europeBuyNativeYields();
	bool AI_europeBuyYields();
	bool AI_europe();
	bool AI_europeAssaultSea();

	int AI_getCostDifferenceFreeVsSlave() const;

	bool AI_sailToPreferredPort(bool bMove = true);
	bool AI_sailToEurope(bool bMove = true);
	CvPlot* AI_europeTreasureTarget(CvPlot*& pArrival, bool& bFar);
	bool AI_treasureTransportMove(); // WTP: fetching treasures and taking them to Europe (AI transports, human treasure ships)
	bool AI_sailToAfrica(bool bMove = true); /*** TRIANGLETRADE 10/28/08 by DPII ***/
	bool AI_sailToPortRoyal(bool bMove = true); // R&R, ray, Port Royal

	CvPlot* findNearbyOceanPlot(const CvPlot& kPlot) const;	// TAC - AI Improved Naval AI - koma13

	bool AI_travelToPort(int iMinPercent = 25, int iMaxPath = MAX_INT);

	int AI_loadYieldValue(CvCity* pCity, YieldTypes eYield, int iAmount, int iTravelTurns = 0);
	int AI_unloadYieldValue(CvCity* pCity, YieldTypes eYield, int iAmount, int iTravelTurns = 0);

	bool AI_collectGoods();

	bool AI_deliverUnits(UnitAITypes eUnitAI = NO_UNITAI);
	
	CvPlot* AI_bestDestinationPlot(bool bIgnoreDanger = false) const;	// TAC - AI Improved Naval AI - koma13

	bool AI_loadUnits(UnitAITypes eUnitAI, MissionAITypes eMissionAI);
	
	bool AI_wakeCargo(UnitAITypes eUnitAI, int iPriority);

	int AI_promotionValue(PromotionTypes ePromotion);

	bool AI_seaPatrol();

	bool AI_moveTowardsOcean(int iRange);

	// TAC - AI Improved Naval AI - koma13 - START
	bool AI_moveTowardsDanger(int iMaxPath = MAX_INT);
	// TAC - AI Improved Naval AI - koma13 - END

	bool AI_moveTowardsVictimCity();
	bool AI_spreadReligion();

	bool AI_spreadTradePosts(); // WTP, ray, Native Trade Posts - START
	bool AI_tradePostVoyage();

	bool AI_learn(int iRange = 8);
	bool AI_routeCityNear(int iMaxDistance);
	bool AI_wfImproveStages(); // WTP
	void AI_logPioneer(const char* szReason); // WTP

	bool AI_requestPickup(int iMaxPath = MAX_INT);
	void AI_wakeTransportForPickup(const CvPlot* pPickupPlot); // WTP
	bool AI_respondToPickup(int iMaxPath = MAX_INT, UnitAITypes eUnitAI = NO_UNITAI, bool bAlongTheWay = false);

	bool AI_pickupAdjacantUnits();

	bool AI_continueMission(int iAbortDistance, MissionAITypes eValidMissionAI, int iFlags = 0, bool bStepwise = true);
	bool AI_breakAutomation();

	bool AI_unloadWhereNeeded(int iMaxPath = MAX_INT);

	bool AI_betterJob();
	bool AI_upgradeProfession();
	bool AI_changeUnitAIType(int iMinMultiplier = 0);

	bool AI_advance(bool bAttack);
	bool AI_loiter(int iMinDistance, int iMaxDistance, bool bAttack);
	bool AI_retreat(int iMaxDistance);
	bool AI_counter(int iTether);
	bool AI_shouldRun();

	bool AI_shadow(UnitAITypes eUnitAI, int iMax = -1, int iMaxRatio = -1, bool bWithCargoOnly = true);

	// K-Mod. I've created AI_omniGroup with the intention of using it to phase out AI_group and AI_groupMergeRange.
	bool AI_omniGroup(UnitAITypes eUnitAI, int iMaxGroup = -1, int iMaxOwnUnitAI = -1,
		bool bStackOfDoom = false, int iFlags = 0 /*NO_MOVEMENT_FLAGS*/,
		int iMaxPath = -1, bool bMergeGroups = true, bool bSafeOnly = true,
		bool bIgnoreFaster = false, bool bIgnoreOwnUnitType = false,
		bool bBiggerOnly = true, int iMinUnitAI = -1, bool bWithCargoOnly = false,
		bool bIgnoreBusyTransports = false);// TAC - AI Assault Sea - koma13, jdog5000(BBAI) - START
	//bool AI_group(UnitAITypes eUnitAI, int iMaxGroup = -1, int iMaxOwnUnitAI = -1, int iMinUnitAI = -1, bool bIgnoreFaster = false, bool bIgnoreOwnUnitType = false, bool bStackOfDoom = false, int iMaxPath = MAX_INT, bool bAllowRegrouping = false);
	bool AI_group(UnitAITypes eUnitAI, int iMaxGroup = -1, int iMaxOwnUnitAI = -1, int iMinUnitAI = -1, bool bIgnoreFaster = false, bool bIgnoreOwnUnitType = false, bool bStackOfDoom = false, int iMaxPath = MAX_INT, bool bAllowRegrouping = false, bool bWithCargoOnly = false, bool bInCityOnly = false, MissionAITypes eIgnoreMissionAIType = NO_MISSIONAI);
	// TAC - AI Assault Sea - koma13, jdog5000(BBAI) - END

	bool AI_load(UnitAITypes eUnitAI, MissionAITypes eMissionAI, UnitAITypes eTransportedUnitAI = NO_UNITAI, int iMinCargo = -1, int iMinCargoSpace = -1, int iMaxCargoSpace = -1, int iMaxCargoOurUnitAI = -1, int iFlags = 0, int iMaxPath = MAX_INT);
	bool AI_guardCityBestDefender();
	bool AI_guardCityMinDefender();
	bool AI_guardCity(bool bAll = false, int iMaxPath = MAX_INT);
	bool AI_guardCityCounter(int iMaxPath = MAX_INT);
	int AI_getPlotDefendersNeeded(CvPlot* pPlot, int iExtra);
	bool AI_guardFort(bool bSearch = true);
	// Super Forts begin *AI_defense*
	bool AI_guardFortMinDefender(bool bSearch = true);
	// Super Forts end
	bool AI_guardCitySite();
	bool AI_chokeDefend();
	bool AI_heal(int iDamagePercent = 0, int iMaxPath = MAX_INT);
	bool AI_afterAttack();
	bool AI_lead(std::vector<UnitAITypes>& aeAIUnitTypes);
	bool AI_protect(int iOddsThreshold);
	bool AI_bravePatrol();
	bool AI_patrol();
	bool AI_defend();
	bool AI_safety();
	bool AI_hide();
	bool AI_goody(bool bIgnoreCity = false);
	bool AI_goodyRange(int iRange, bool bIgnoreCity = false, int iMaxPathTurns = MAX_INT);

	bool AI_isValidExplore(CvPlot* pPlot) const;
	int AI_explorePlotValue(CvPlot* pPlot, bool bImportantOnly = false) const;
	bool AI_explore(bool bFavorOpenBorders = false);
	bool AI_exploreRange(int iRange);
	bool AI_exploreFromShip(int iMaxPath);
	bool AI_exploreCoast(int iRange);
	bool AI_exploreOcean(int iRange);
	bool AI_exploreDeep();
	bool AI_exploreOpenBorders(int iRange);
	//TAC Whaling, ray
	bool AI_exploreHighSeas(int iRange);
	bool AI_moveToWhale(); //koma13
	bool AI_exploreRessource();
	int AI_exploreRessourcePlotValue(CvPlot* pPlot);
	bool AI_gatherResource();
	//End TAC Whaling, ray
	bool AI_targetCity(int iFlags = 0);
	bool AI_targetCityNative(int iFlags = 0);

	// R&R, ray, Natives raiding party - START
	CvCity* AI_nativePickRaidTargetCity(int iFlags = 0, int iMaxPath = MAX_INT);
	bool AI_nativeGoToRaidTargetCity(int iFlags = 0, int iMaxPath = MAX_INT, CvCity* pTargetCity = NULL);
	// R&R, ray, Natives raiding party - END

	bool AI_extortCity();

	// TAC - AI Attack City - koma13, jdog5000(BBAI) - START
	CvCity* AI_pickTargetCity(int iFlags = 0, int iMaxPath = MAX_INT, bool bHuntBarbs = false);
	bool AI_goToTargetCity(int iFlags = 0, int iMaxPath = MAX_INT, CvCity* pTargetCity = NULL);
	bool AI_pillageAroundCity(CvCity* pTargetCity, int iBonusValueThreshold = 0, int iMaxPathTurns = MAX_INT);
	// TAC - AI Attack City - koma13, jdog5000(BBAI) - END

	bool AI_bombardCity();
	// WTP: siege
	bool AI_canBesiege(const CvCity* pCity) const;
	bool AI_siegeBlockade();
	bool AI_siegeSupply();
public:
	int AI_siegeArtilleryCount() const;
protected:
	bool AI_cityAttack(int iRange, int iOddsThreshold, bool bFollow = false);
	// TAC - AI Attack City - koma13 - START
	//bool AI_anyAttack(int iRange, int iOddsThreshold, int iMinStack = 0, bool bFollow = false);
	bool AI_anyAttack(int iRange, int iOddsThreshold, int iMinStack = 0, bool bFollow = false, bool bAllowCities = true);
	// TAC - AI Attack City - koma13 - END
	bool AI_adjacentSeaAttack(int iOddsThreshold); // WTP, Schmiddie, Naval AI adjacent attack
	bool AI_smartAttack(int iRange, int iLowOddsThreshold, int iHighOddsThreshold, CvPlot* pHintPlot);
	bool AI_leaveAttack(int iRange, int iThreshold, int iStrengthThreshold);
	bool AI_defendTeritory(int iThreshold, int iFlags, int iMaxPathTurns, bool bLocal = false); // K-Mod
	bool AI_stackVsStack(int iSearchRange, int iAttackThreshold, int iRiskThreshold, int iFlags); // K-Mod
	bool AI_siegeMuster(); // WTP: siege
	bool AI_seaBombardRange(int iMaxRange);
	bool AI_blockade(int iRange);
	bool AI_pillage();
	bool AI_pillageRange(int iRange, bool bSafe = false);
	bool AI_maraud(int iRange);
	bool AI_hostileShuffle();
	bool AI_wanderAroundAimlessly();
	int AI_foundValue(CvPlot* pPlot);
	bool AI_found(int iMinValue = 1, bool bFirstCity = false);
	bool AI_foundRange(int iRange, bool bFollow = false);
	bool AI_joinCityBrave();
	bool AI_joinCity(int iMaxPath = MAX_INT);
	bool AI_joinOptimalCity();
	bool AI_joinCityDefender();
	bool AI_yieldDestination(int iMaxPath = MAX_INT);
	bool AI_yieldNativeDestination(int iMaxPath = MAX_INT);
	bool AI_tradeWithCity();
	// WTP: native trade ship, deals with the native village on this plot
	bool AI_isNativeTradeShip() const;
	bool AI_knowsMusketVillage() const;
	static const int NATIVE_TRADE_MUSKET_GOLD = 2500; // WTP: a tribe with more gold gets muskets from the native trade ship
	static const int NATIVE_TRADE_MUSKET_MIN_PROFIT = 500; // WTP: a village gets the muskets only if it pays this much more than they cost in Europe
	static const int NATIVE_TRADE_SELL_TURNS = 15; // WTP: the native trade ship takes unsold goods back to Europe this many turns after its arrival
	static const int NATIVE_TRADE_ARRIVAL_TURNS = 8; // WTP: the native trade ship comes from Europe near a village it reaches within this many turns
	CvPlot* AI_nativeTradeArrivalPlot(bool bMuskets, CvCity*& pVillage, int& iVillageTurns);
	int AI_nativeMusketGold(PlayerTypes eNative) const;
	int AI_nativeMusketMinPrice(int iAmount) const;
	int AI_nativeTradeMusketsAboard() const;
	int AI_nativeTradeCargoCount(bool bGoods, bool* pbMuskets) const;
	void AI_nativeTradeShipMove();
	void AI_explorerShipMove();
	bool AI_nativeTradeEurope();
	bool AI_crossFromEurope(CvPlot* pArrival = NULL);
	int AI_nativeTradeSell(CvWString* pszDeals = NULL);
	int AI_nativeTradeBuy(CvWString* pszDeals = NULL);
	bool AI_canTradeWithVillage(const CvCity& kCity) const;
	CvCity* AI_nativeTradeBuyVillage(bool bDraw = true);
	CvCity* AI_nativeTradeSellVillage(const CvCity* pFavourite, bool bMuskets);
	
	// TAC - AI Assault Sea - koma13, jdog5000(BBAI) - START
	bool AI_assaultSeaTransport(bool bNative = false);
	bool AI_assaultSeaReinforce(bool bNative = false);
	// TAC - AI Assault Sea - koma13, jdog5000(BBAI) - END
	
	bool AI_settlerSeaFerry();
	bool AI_specialSeaTransportMissionary();
	bool AI_connectPlot(CvPlot* pPlot, int iRange = 0);
	bool AI_improveCity(CvCity* pCity);
	bool AI_improveLocalPlot(int iRange, CvCity* pIgnoreCity);
	bool AI_nextCityToImprove(CvCity* pCity);
	bool AI_fortTerritory(bool bCanal);
	bool AI_improvePlot(CvPlot* pPlot, BuildTypes eBuild);
	BuildTypes AI_betterPlotBuild(CvPlot* pPlot, BuildTypes eBuild);
	bool AI_routeCity();
	bool AI_routeTerritory(bool bImprovementOnly = false);
	bool AI_travelToUpgradeCity();
	bool AI_retreatToCity(bool bPrimary = false, int iMaxPath = MAX_INT, bool bAvoidDanger = false);
	
	bool AI_retreatFromDanger();	// TAC - AI Improved Naval AI - koma13

	bool AI_treasureRetreat(int iMaxPathTurns = MAX_INT);
	
	// TAC - AI Assault Sea - koma13, jdog5000(BBAI) - START
	//bool AI_pickup(UnitAITypes eUnitAI);
	bool AI_pickup(UnitAITypes eUnitAI, int iMaxPath = MAX_INT);
	// TAC - AI Assault Sea - koma13, jdog5000(BBAI) - END
	
	bool AI_moveToStagingCity();

	bool AI_disembark(bool bEnemyCity = true);
	bool AI_imperialSeaAssault();

	bool AI_followBombard();

	bool AI_potentialEnemy(TeamTypes eTeam, const CvPlot* pPlot = NULL);

	bool AI_defendPlot(CvPlot* pPlot);
	int AI_pillageValue(CvPlot* pPlot);
	bool AI_canPillage(CvPlot& kPlot) const;

	int AI_searchRange(int iRange) const;
	bool AI_plotValid(const CvPlot* pPlot) const;

	int AI_finalOddsThreshold(CvPlot* pPlot, int iOddsThreshold);
	// K-Mod: attack odds weighted by the worth of the units, wounds, the defensive terrain and a last city defender
	int AI_getWeightedOdds(CvPlot* pPlot, bool bPotentialEnemy = false);
	int AI_combatWorth(const CvUnit& kUnit) const;
	bool AI_isWeightedOdds() const;
	int AI_attackOddsToCompare(CvPlot* pPlot);
	void AI_logAttackDecision(const char* szFunction, const CvPlot& kTarget, int iOdds) const; // WTP: war AI log

	int AI_stackOfDoomExtra();

	bool AI_stackAttackCity(int iRange, int iPowerThreshold, bool bFollow = true);
	bool AI_moveIntoCity(int iRange);

	bool AI_groupMergeRange(UnitAITypes eUnitAI, int iRange, bool bBiggerOnly = true, bool bAllowRegrouping = false, bool bIgnoreFaster = false);

	// R&R, ray, Natives raiding party - START
	bool AI_nativeRaidTreasureUnit();
	// R&R, ray, Natives raiding party - END

	bool AI_nativeRaidFort(); // R&R, ray, Monasteries and Forts

	bool AI_poach();
	bool AI_choke(int iRange = 1);

	bool AI_solveBlockageProblem(CvPlot* pDestPlot, bool bDeclareWar);

	int AI_calculatePlotWorkersNeeded(CvPlot* pPlot, BuildTypes eBuild);

	bool AI_canGroupWithAIType(UnitAITypes eUnitAI) const;
	bool AI_allowGroup(const CvUnit* pUnit, UnitAITypes eUnitAI) const;
	
	bool AI_isOnMission();
	
	bool AI_isObsoleteTradeShip();

	bool AI_sailTo(const SailToHelper& sth, bool bMove, bool bIgnoreDanger = true);

	void AI_sellYieldUnits(TradeLocationTypes eLocation, bool bTreasuresOnly = false);
	void AI_unloadUnits(TradeLocationTypes eLocation);
	void AI_automateSailTo(const SailToHelper& sth);
	void AI_transportSettler();
	void AI_unloadPassengers(bool bAll);
	bool AI_wfTravelByShip(const CvCity& kTarget, CvCity* pPreferredPort);
	bool AI_canCarryTo(const CvUnit& kPassenger) const; // WTP
	bool AI_isAfricaWorthy(YieldTypes eYield) const; // WTP
	bool AI_collectGoodsForAfrica(); // WTP
	bool AI_isSoldier() const;
	CvCity* AI_transportDestinationCity() const;
	bool AI_pickupStranded(UnitAITypes eUnitAI = NO_UNITAI, int iMaxPath = MAX_INT);

	// added so under cheat mode we can call protected functions for testing
	friend class CvGameTextMgr;

};

inline bool CvUnitAI::is(UnitAIStates eUnitAIState) const
{
	return m_eUnitAIState == eUnitAIState;
}

inline bool CvUnitAI::is(UnitAITypes eUnitAI) const
{
	return m_eUnitAIType == eUnitAI;
}

#endif
