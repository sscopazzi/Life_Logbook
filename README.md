# Logbook

## ▶ Try the demo: **[sscopazzi.github.io/Life_Logbook/logbook-demo.html](https://sscopazzi.github.io/Life_Logbook/logbook-demo.html)**

No hardware, no sign-up. The demo runs in your browser with made-up data, and your changes stay in that browser.

---

**Logbook is a small physical box that keeps your house, your work orders and your money in order, and it keeps all of it to itself.**

The demo above is a website so you can try it. The real Logbook is not a website. It is a battery-powered box with its own screen, a knob and a button. It makes its own Wi-Fi hotspot and serves its dashboard to your phone or laptop over that hotspot.

- **Standalone.** No account, no cloud, no app, no subscription and no outside servers. Everything is stored on the box and its SD card. It works with no internet at all.
- **Private.** Nobody can see your data unless you give them the hotspot password and let them connect.
- **Portable.** Built for life on rotation: take it to sea, bring it home, and it picks up where it left off.

If you give the box your home Wi-Fi, it uses that only to set its clock and to be reachable at http://logbook.local when it's in range of your Wi-Fi, so you don't have to connect directly to the box when you're at home. The one outside request is the street map on the Track tab. Your own phone or laptop fetches it when that device has internet; without internet, the track is drawn on a plain grid.

## What it does

| | |
|---|---|
| **Board** | Overdue, due-soon, open and recently finished work orders, with reminders for bills, credit cards, rent, expiring documents and backups. |
| **Work orders** | One-off and routine jobs, each with description, steps, findings, previous findings, parts and cost, people and time, attachments, and a full log. |
| **Daily checklist** | Nested items you can drag to reorder. Ticking an item ticks everything under it, on the web page and on the box. Items can link to work-order steps. Archive a day, then rename, edit or delete it later. |
| **Notes** | Anything worth keeping. Pin notes to the top, color-code their borders, and put `#word` or `#[two words]` anywhere in a note to file it under a category. Archive old notes and search them later. Export a note to Word or print it / save it as a PDF. |
| **Calendar** | Your events (multi-day and repeating), work orders, routine dates, subscriptions, bills, card due dates, rent and loan payments. Imports and exports `.ics` (Google Calendar). |
| **Assets** | **Things** (car, water heater, HVAC… with manuals, warranties, meters and service history), **Owed** (loans and IOUs both ways), **Rent & lease** (rent you pay, split or not, and rent you collect), **Balances** and **Projection** (below). |
| **Expiring** | Licenses, certifications, passports and insurance, with your own reminder schedule, plus asset warranty end dates. |
| **Purchasing** | Spending by category, subscriptions, bills and vendors. |
| **Reports** | Pick any date range. Charts of net worth, what you have, what's owed, spending by category and month, work orders, and how long the box was on and off. Print, save as PDF, or export CSV. |
| **Climate** | Inside and outside temperature and humidity from small wireless sensor nodes (ESP-NOW). Gaps stay gaps; nothing is filled in. |
| **Track** | Where the box has been while it had a GPS fix. Export GPX and KML. |
| **Setup** | Wi-Fi, clock, storage, light or dark mode, routine groups, the routine schedule, backups and the full searchable history. Hide the tabs you don't use (money, climate, track…); nothing is deleted, and you can bring them back any time. |

### Money, the way a spreadsheet does it

- **Balances:** set up your accounts and credit cards with your own names. Whenever you like, type what each account shows. Logbook adds up the accounts you tick, takes off money you've set aside (a buffer, money you're holding for someone) and what you owe on cards, and gives you **Available**.
- **Projection:** carries Available forward month by month. Bills, subscriptions, rent and loan payment plans fill in on the 1st of each month. Add your own lines, including pay by the day (rate × days). Green is money in, red is money out, blue is money to savings, and every amount shows its + or − sign.

## The physical box

- **Controller:** Adafruit ESP32-S2 Feather.
- **Screen:** 128×128 grayscale OLED (Adafruit 4741) showing the time, Wi-Fi, GPS, climate, timers and expiring documents.
- **Controls:**
  - a clicking rotary knob (Adafruit 5880) that moves one row per click, ticks the highlighted checklist item when pressed, and turns the screen off when held;
  - a lit button to change screens and silence alarms. Its light glows dim while the screen sleeps.
- **Buzzer:** a piezo (Adafruit 1739) for kitchen timers, "find my box", and a BNWAS-style move reminder that sounds every few minutes until you walk over and press the button.
- **Also inside:** GPS, a real-time clock and an SD card, all running on battery.

## Updating a box

Updates are a single `.lbk` file, holding the dashboard and the firmware together. Upload it in **Setup › Firmware** with the box's update password; the box restarts and the page reloads.

## Building from source

- **Arduino ESP32 core:** 3.3.x
- **Board:** `esp32:esp32:adafruit_feather_esp32s2`
- **Partition scheme:** Default 4MB with spiffs

Wi-Fi details and the update password go in a local `secrets.h`. Keep that file out of the repository.

---

Made by Aspect Wave LLC.
