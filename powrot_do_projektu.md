# CYD GPS — notatki po polsku

Prędkościomierz, licznik, rejestrator tras GPX i pomiar przyspieszenia na Cheap Yellow Display
(ESP32-2432S028, wersja z 2 gniazdami USB) z modułem GPS sprzedawanym jako NEO-8M (w praktyce klon
AT6558, 115200 bd). Szczegóły techniczne: [README.md](README.md) (po angielsku).

## Jak wrócić do projektu

W terminalu, w folderze projektu:

```sh
claude --continue   # ostatnia rozmowa w tym folderze
claude --resume     # lista wcześniejszych rozmów do wyboru
claude              # nowa rozmowa (notatki o projekcie i sprzęcie są zapamiętane)
```

## Gdzie co jest

| Co | Gdzie |
|---|---|
| Kod | ten folder (git, gałąź `main`) |
| Wydania, które CYD instaluje sam | GitHub: `honzo90/cyd-gps`, zakładka Releases |
| Numer wersji | `include/fw_version.h` |
| Testy bez płytki | `test/host/` |
| Strona WWW na CYD | `src/web.cpp`, widok przejazdów `src/web/rides.js` |
| Logika bez Arduino (testowana) | `lib/` (licznik, GPX, pomiar, kierunek, obrót ekranu) |

## Najczęstsze polecenia

```sh
sh test/host/run.sh               # wszystkie testy (C + JavaScript)
pio run                           # zbudowanie wszystkich wersji
pio run -e cyd2usb -t upload      # wgranie przez USB
pio run -e cyd2usb_sim -t upload  # wersja z symulowaną jazdą (bez prawdziwego GPS)
sh tools/release.sh 1.0.16        # nowe wydanie na GitHub (testy, build, tag, push)
```

Po wydaniu CYD w sieci domowej sam znajdzie nową wersję (co 6 h albo przycisk „Sprawdz
aktualizacje” na ekranie WI-FI); instalacja przytrzymaniem przycisku „Instaluj …”. Jeśli nowa wersja
nie uruchomi sieci w 15 s, CYD wraca do poprzedniej.

## Ekrany CYD (przesuwanie w lewo/prawo)

1. **Prędkościomierz** — duże cyfry, pasek, kierunek jazdy (strzałka + N/NE/E…, od 5 km/h; moduł
   nie ma kompasu, kierunek liczy GPS z ruchu).
2. **POMIAR** — 0–50, 0–100, 0–200, 100–200 km/h, 100 m, 1/4 mili, plus nachylenie drogi
   (pod górę / z górki, z wysokości GPS, orientacyjnie ±1–2 %, od 50 m przejazdu; przy rekordach na
   stronie widać, na jakiej drodze padły). Stań na 1 s, ruszaj — czas startuje
   sam. „Restart” zeruje bieżący pomiar; przytrzymanie tytułu „POMIAR” kasuje najlepsze wyniki.
   GPS pracuje wtedy z częstotliwością 10 Hz. Tylko na zamkniętym torze lub placu.
3. **Licznik** — trip, całkowity, maks., średnia, czas jazdy; przytrzymanie „Zeruj trip” zaczyna nowy
   plik GPX.
4. **GPS** — pozycja, wysokość, satelity, HDOP, czas, siła sygnału i częstotliwość pomiarów.
5. **WI-FI** — sieć / hotspot, adres strony, aktualizacje, **QR** (przytrzymaj: logowanie telefonem
   i hasło), **⟳** obrót ekranu o 90°.

## Telefon bez domowej sieci (w terenie)

1. Na CYD ekran WI-FI: widać „Hotspot: CYD-GPS-…” i adres `http://192.168.4.1`.
2. Przytrzymaj **QR** — pokaże kod i hasło urządzenia (to samo hasło jest hasłem hotspotu).
3. W telefonie połącz się z siecią `CYD-GPS-…`; przy komunikacie „brak internetu” zostań połączony.
4. Zeskanuj kod QR (ważny 2 minuty) albo otwórz `http://192.168.4.1` (użytkownik `cyd`).

Bez internetu mapa w „Przejazdach” się nie wczyta — strona pokazuje wtedy samą linię trasy.

## Komendy przez USB (115200 bd)

| Komenda | Działanie |
|---|---|
| `?` | pomoc |
| `n` | podgląd surowych danych NMEA z GPS (wł./wył.) |
| `L` | lista plików GPX na karcie |
| `D nazwa.gpx` | usunięcie pliku GPX (nie tego, który jest właśnie zapisywany) |
| `T` | wyzerowanie całego licznika (trip i całkowity) |
| `R` | obrót ekranu o 90° |
| `B` | dziennik restartów: liczba uruchomień i przyczyny 8 ostatnich |

## Dziennik restartów — jak czytać

Widoczny przez `B` i na stronie (karta „Aktualizacja firmware”, na dole).

| Przyczyna | Znaczenie |
|---|---|
| zasilanie wł. | CYD dostał prąd: włączenie, podłączenie kabla — albo **chwilowa utrata zasilania** |
| BROWNOUT | napięcie spadło za nisko (słaby zasilacz / kabel przy szczycie poboru prądu) |
| BLAD PROGRAMU, watchdog | błąd w firmware — do zbadania w kodzie |
| restart programowy | celowy, np. po instalacji aktualizacji |

Każde podłączenie do komputera też dodaje „zasilanie wł.” (otwarcie portu USB potrafi zrestartować
CYD) — warto pamiętać, ile razy włączało się go samemu.

## Otwarte sprawy

- **Restarty na powerbanku.** Dziennik pokazał wyłącznie „zasilanie wł.” — CYD traci prąd, firmware
  się nie wywraca. Do sprawdzenia: poruszanie wtykiem USB-C w gnieździe CYD (czy resetuje), inny
  krótki kabel, inny powerbank lub jego tryb niskiego prądu. Potem znów odczytać dziennik (`B`).
- **Pierwszy prawdziwy pomiar** na ekranie POMIAR (na zamkniętym placu) — sprawdzony dotąd tylko
  testami i symulacją.
- **Kierunek jazdy** — sprawdzony w symulacji, jeszcze nie w prawdziwym ruchu.

## Zasady projektu

- Repozytorium jest **publiczne**: żadnych haseł, nazw sieci ani prawdziwych tras GPX (zawierają
  lokalizację). Testy używają wyłącznie tras wygenerowanych.
- Hasło urządzenia jest generowane na CYD i widoczne tylko na jego ekranie (przycisk QR).
- Aktualizacje pobierane wyłącznie przez HTTPS ze sprawdzaniem certyfikatu.
