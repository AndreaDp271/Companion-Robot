# Desktop Companion - feste: nei giorni di festa il robot fa ogni tanto un'animazione a tema
# (alternata con quella del meteo) e ti fa gli auguri.
from datetime import date, timedelta

# festa -> nome nel menu
FEAST_TEXT = {
    "capodanno": "Capodanno",
    "befana": "Befana",
    "sanvalentino": "San Valentino",
    "carnevale": "Carnevale",
    "pasqua": "Pasqua",
    "italia": "25 aprile / 2 giugno",
    "ferragosto": "Ferragosto",
    "halloween": "Halloween",
    "natale": "Natale",
}


def easter(year):
    """Domenica di Pasqua (calendario gregoriano, algoritmo di Meeus)."""
    a, b, c = year % 19, year // 100, year % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    month = (h + l - 7 * m + 114) // 31
    day = (h + l - 7 * m + 114) % 31 + 1
    return date(year, month, day)


def feast_today(today=None):
    """La festa di oggi (chiave di FEAST_TEXT) o None."""
    t = today or date.today()
    md = (t.month, t.day)
    if md in ((12, 31), (1, 1)):
        return "capodanno"
    if md == (1, 6):
        return "befana"
    if md == (2, 14):
        return "sanvalentino"
    e = easter(t.year)
    if e - timedelta(days=52) <= t <= e - timedelta(days=47):  # dal giovedì al martedì grasso
        return "carnevale"
    if t in (e, e + timedelta(days=1)):  # Pasqua e Pasquetta
        return "pasqua"
    if md in ((4, 25), (6, 2)):
        return "italia"
    if md == (8, 15):
        return "ferragosto"
    if md == (10, 31):
        return "halloween"
    if (12, 8) <= md <= (12, 30) or (1, 2) <= md <= (1, 5):  # periodo di Natale
        return "natale"
    return None
