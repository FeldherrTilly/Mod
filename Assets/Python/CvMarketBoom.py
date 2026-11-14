## Market boom: at the start of the game three random cash crops from at least two climate zones (and the
## goods made from them) become fashionable in Europe and Port Royal. The raw good is lifted to a fixed price
## for every European colony, the goods made from it rise as well (capped); all of them slowly drift back
## to equilibrium through the normal price correction.

from CvPythonExtensions import *

gc = CyGlobalContext()
localText = CyTranslator()

TROPICAL, TEMPERATE, ARID, COLD = range(4)

DYE_GOODS = ("YIELD_COLOURED_CLOTH", "YIELD_COLOURED_WOOL_CLOTH", "YIELD_FESTIVE_CLOTHES", "YIELD_EVERYDAY_CLOTHES")

# price the raw good is lifted to; PLUS_2 raises it by 2 instead (cochineal already starts high)
STAPLE, COMMON, HILL, RARE, PLUS_2 = 6, 7, 8, 9, -2

# the goods made from it rise by as much as the raw good, but by no more than this
MAX_PRODUCT_RISE = 3

# (raw yield, price, climate zone, goods made from it that rise as well)
BOOM_GOODS = (
    ("YIELD_SUGAR", COMMON, TROPICAL, ("YIELD_RUM", "YIELD_CHOCOLATE")),
    ("YIELD_COFFEE_BERRIES", COMMON, TROPICAL, ("YIELD_COFFEE",)),
    ("YIELD_COCOA_FRUITS", COMMON, TROPICAL, ("YIELD_COCOA", "YIELD_CHOCOLATE")),
    ("YIELD_KAUTSCHUK", RARE, TROPICAL, ()),
    ("YIELD_LOGWOOD", COMMON, TROPICAL, DYE_GOODS),
    ("YIELD_RICE", STAPLE, TROPICAL, ("YIELD_BAKERY_GOODS",)),
    ("YIELD_COCA_LEAVES", HILL, TROPICAL, ()),
    ("YIELD_YERBA_LEAVES", HILL, TROPICAL, ("YIELD_YERBA_TEA",)),
    ("YIELD_FRUITS", STAPLE, TROPICAL, ("YIELD_HOOCH",)),
    ("YIELD_TOBACCO", COMMON, TEMPERATE, ("YIELD_CIGARS",)),
    ("YIELD_COTTON", COMMON, TEMPERATE, ("YIELD_CLOTH", "YIELD_COLOURED_CLOTH", "YIELD_FESTIVE_CLOTHES")),
    ("YIELD_INDIGO", COMMON, TEMPERATE, DYE_GOODS),
    ("YIELD_PEANUTS", COMMON, TEMPERATE, ("YIELD_ROASTED_PEANUTS",)),
    ("YIELD_BARLEY", STAPLE, TEMPERATE, ("YIELD_BEER", "YIELD_BAKERY_GOODS")),
    ("YIELD_GRAPES", COMMON, TEMPERATE, ("YIELD_WINE",)),
    ("YIELD_OLIVES", COMMON, TEMPERATE, ("YIELD_OLIVE_OIL",)),
    ("YIELD_RED_PEPPER", HILL, TEMPERATE, ("YIELD_SPICES",)),
    ("YIELD_WILD_FEATHERS", COMMON, TEMPERATE, ("YIELD_GOAT_HIDE_BOOTS", "YIELD_PADDED_LEATHER_COATS", "YIELD_PADDED_FURNITURE")),
    ("YIELD_RAW_SALT", COMMON, ARID, ("YIELD_SALT",)),
    ("YIELD_COCHINEAL", PLUS_2, ARID, DYE_GOODS),
    ("YIELD_CASSAVA", STAPLE, ARID, ("YIELD_BAKERY_GOODS",)),
    ("YIELD_VANILLA_PODS", HILL, ARID, ("YIELD_VANILLA",)),
    ("YIELD_FUR", STAPLE, COLD, ("YIELD_COATS",)),
    ("YIELD_PREMIUM_FUR", RARE, COLD, ("YIELD_PREMIUM_COATS",)),
    ("YIELD_MAPLE_SIRUP", HILL, COLD, ()),
    ("YIELD_RAPE", STAPLE, COLD, ("YIELD_RAPE_OIL",)),
    ("YIELD_VALUABLE_WOOD", HILL, COLD, ("YIELD_FURNITURE", "YIELD_PADDED_FURNITURE", "YIELD_HOUSEHOLD_GOODS")),
    ("YIELD_HARDWOOD", COMMON, COLD, ()),
    ("YIELD_PEAT", STAPLE, COLD, ("YIELD_POTTERY",)),
)


def checkMarketBoom():
    "Called from onGameStart, so the boom is announced in the first turn"
    if CyGame().getElapsedGameTurns() != 0:
        return

    game = CyGame()
    first = BOOM_GOODS[game.getSorenRandNum(len(BOOM_GOODS), "Market boom 1")]
    others = [g for g in BOOM_GOODS if g[2] != first[2]]
    second = others[game.getSorenRandNum(len(others), "Market boom 2")]
    rest = [g for g in BOOM_GOODS if g != first and g != second]
    third = rest[game.getSorenRandNum(len(rest), "Market boom 3")]
    booms = (first, second, third)

    kings = []
    for iPlayer in range(gc.getMAX_PLAYERS()):
        player = gc.getPlayer(iPlayer)
        if not player.isAlive() or player.isNative() or player.isEurope():
            continue
        eKing = player.getParent()
        if eKing < 0 or eKing in kings or not gc.getPlayer(eKing).isEurope():
            continue
        kings.append(eKing)
        king = gc.getPlayer(eKing)
        # Europe prices are capped at the xml maximum; only newer DLLs offer a setter that may exceed it
        if hasattr(king, "setYieldBuyPriceAboveMax"):
            _raise(booms, king.getYieldBuyPrice, king.setYieldBuyPriceAboveMax)
        else:
            _raise(booms, king.getYieldBuyPrice, king.setYieldBuyPrice)
        _raise(booms, king.getYieldPortRoyalBuyPriceNoModifier, king.setYieldPortRoyalBuyPrice)

    for iPlayer in range(gc.getMAX_PLAYERS()):
        player = gc.getPlayer(iPlayer)
        if player.isAlive() and player.isHuman() and player.getParent() in kings:
            _showBoom(iPlayer, gc.getPlayer(player.getParent()), booms)


def _raise(booms, getPrice, setPrice):
    "Lift the raw goods of one market; each product rises once, by the larger amount if both booms share it"
    changes = {}
    for szRaw, iPrice, iZone, products in booms:
        eRaw = gc.getInfoTypeForString(szRaw)
        if iPrice == PLUS_2:
            iChange = 2
        else:
            iChange = max(0, iPrice - getPrice(eRaw))
        for szYield in (szRaw,) + products:
            eYield = gc.getInfoTypeForString(szYield)
            if eYield >= 0:
                if szYield != szRaw:
                    iChange = min(iChange, MAX_PRODUCT_RISE)
                changes[eYield] = max(changes.get(eYield, 0), iChange)
    for eYield, iChange in changes.items():
        if iChange > 0:
            setPrice(eYield, getPrice(eYield) + iChange, False)


def _showBoom(iPlayer, king, booms):
    szText = localText.getText("TXT_KEY_MARKET_BOOM_TITLE", ())
    szNames = []
    for szRaw, iPrice, iZone, products in booms:
        info = gc.getYieldInfo(gc.getInfoTypeForString(szRaw))
        szNames.append(info.getDescription())
        szText += "\n\n" + localText.getText("TXT_KEY_MARKET_BOOM_" + szRaw[len("YIELD_"):], ())
        for szYield in (szRaw,) + products:
            eYield = gc.getInfoTypeForString(szYield)
            kYield = gc.getYieldInfo(eYield)
            szText += "\n" + localText.getText("TXT_KEY_MARKET_BOOM_PRICE", (kYield.getDescription(), kYield.getChar(),
                king.getYieldBuyPrice(eYield), king.getYieldPortRoyalBuyPriceNoModifier(eYield)))
    szText += "\n\n" + localText.getText("TXT_KEY_MARKET_BOOM_FOOTER", ())

    popupInfo = CyPopupInfo()
    popupInfo.setButtonPopupType(ButtonPopupTypes.BUTTONPOPUP_TEXT)
    popupInfo.setText(szText)
    popupInfo.addPopup(iPlayer)

    CyInterface().addMessage(iPlayer, True, 10, localText.getText("TXT_KEY_MARKET_BOOM_MESSAGE", (szNames[0], szNames[1], szNames[2])),
                             "", 0, "", ColorTypes(8), -1, -1, False, False)
