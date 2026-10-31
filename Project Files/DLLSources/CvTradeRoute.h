#pragma once

#ifndef CIV4_TRADE_ROUTE_H
#define CIV4_TRADE_ROUTE_H

class CvSavegameReader;
class CvSavegameWriter;
class CvTradeRoute
{
public:
	static const int EUROPE_CITY_ID = -1;
	static const int ANYWHERE_CITY_ID = -2;

	CvTradeRoute();
	~CvTradeRoute();

	void init(const IDInfo& kSourceCity, const IDInfo& kDestinationCity, YieldTypes eYield);

	int getID() const;
	void setID(int iId);

	const IDInfo& getSourceCity() const;
	void setSourceCity(const IDInfo& kCity);
	const wchar* getSourceCityNameKey() const;

	const IDInfo& getDestinationCity() const;
	void setDestinationCity(const IDInfo& kCity);
	const wchar* getDestinationCityNameKey() const;

	YieldTypes getYield() const;
	void setYield(YieldTypes eYield);

	bool checkValid(PlayerTypes ePlayer) const;

	void read(CvSavegameReader reader);
	void write(CvSavegameWriter writer);

	void resetSavedData();

	// Custom_House_Mod Start
	int getBestPortCityID(PlayerTypes ePlayer) const;
	// Custom_House_Mod End

	int getDistanceBetweenCities() const;
	int getImportAmount() const;

	// WTP: a reserved route is served only by the transports it is assigned to, never by Automate Transport (full)
	bool isReserved() const;
	void setReserved(bool bNewValue);
	// WTP: a private route is made in the Trade Routes screen for transports: it leaves the export and import settings of
	// the colonies alone, unloads without import limit and is never served by Automate Transport (full)
	bool isPrivate() const;
	void setPrivate(bool bNewValue);

protected:
	int m_iId;
	IDInfo m_kSourceCity;
	IDInfo m_kDestinationCity;
	YieldTypes m_eYield;
	bool m_bReserved;
	bool m_bPrivate;

	void setActiveDirty();
};


#endif  // CIV4_TRADE_ROUTE_H
