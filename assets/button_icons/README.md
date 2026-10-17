# Button icons

The Xbox, Steam Deck and Switch button icons in these sheets come from
**[Xelu's Free Controller & Key Prompts](https://thoseawesomeguys.com/prompts/)** by Nicolae "Xelu"
Berbece of [Those Awesome Guys](https://thoseawesomeguys.com/). Thank you for making them free for
everyone! They are in the public domain under
[Creative Commons 0 (CC0)](https://creativecommons.org/publicdomain/zero/1.0/).

Each sheet (`xbox.png`, `deck.png`, `switch.png`) holds only those icons, scaled to fit the places
the game's PlayStation icons take in its fonts. It holds no pixels from the game.
`tools/button_icons.py` builds the sheets from the unpacked pack, and `scripts/buttons.py` puts them
in the fonts when the game starts (the in-game menu's **Button icons**, `button_icons=` in
`bbport.ini`).

The icons go by the button's position, so the bottom face button (the game's confirm, ✕ on
PlayStation) shows A on Xbox and the Steam Deck, and B on Switch.
