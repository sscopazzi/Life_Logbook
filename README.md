# Logbook

## ▶ Try the demo: **[sscopazzi.github.io/Life_Logbook/logbook-demo.html](https://sscopazzi.github.io/Life_Logbook/logbook-demo.html)**

No hardware, no sign-up. The demo runs in your browser with made-up data, and your changes stay in that browser.

---

**Logbook is a small physical box that keeps your house, your work orders and your money in order, and it keeps all of it to itself.**

The demo above is a website so you can try it. The real Logbook is not a website. It is a battery-powered box with its own screen, a knob and a button. It makes its own Wi-Fi hotspot and serves its dashboard to your phone or laptop over that hotspot.

- **Standalone.** No account, no cloud, no app, no subscription and no outside servers. Everything is stored on the box and its SD card. It works with no internet at all.
- **Private.** On its own hotspot, nobody can see your data without the hotspot password. On your home Wi-Fi (http://logbook.local) it asks for the box password you choose when you set it up, with a "remember this device" option.
- **Portable.** Built for life on rotation: take it to sea, bring it home, and it picks up where it left off.

If you give the box your home Wi-Fi, it uses that only to set its clock and to be reachable at http://logbook.local when it's in range of your Wi-Fi, so you don't have to connect directly to the box when you're at home. The only outside requests are the map tiles on the Map tab (street, satellite or topo). Your own phone or laptop fetches them when that device has internet; without internet, the track and your saved spots are drawn on a plain grid.

## What it does

| | |
|---|---|
| **Work Orders** | Overdue, due-soon, open and recently finished work orders, with reminders for bills, credit cards, rent, expiring documents, low stock and backups. |
| **Work orders** | One-off, routine and business jobs, each with description, steps, findings, previous findings, parts and cost, people and time, attachments, and a full log. Tick "show on the box" and it's listed on the box's screen until it's closed. |
| **Jobs** | For paid work: **clients**, **proposals** (a paragraph about the work, steps priced flat or as rate × hours, terms, an optional deposit), **invoices** (lines, tax, discount, payments, paid deposits credited automatically) and the **jobs** themselves with their steps. Proposal → deposit → work order → invoice, or straight from an invoice to a work order and back. Numbers use your business's short code (AB-P1, AB-I1, AB-J1, business work orders AB-1). Save any proposal or invoice as a PDF or Word file, with your logo and details, no print dialog. |
| **Daily checklist** | Nested items you can drag to reorder. Ticking an item ticks everything under it, on the web page and on the box. Items can link to work-order steps. Archive a day, then rename, edit or delete it later. |
| **Notes** | Anything worth keeping. Buttons for bold, headings, lists and tick boxes. Pin notes to the top, tick "box" to read a note on the box's own screen, color-code their borders, and put `#word` or `#[two words]` anywhere in a note to file it under a category. Link a note to a work order, routine, asset or calendar event and it shows there too; set a reminder and it shows on Work Orders and the calendar; send its lines to today's checklist. Archive old notes and search them later, go back to an earlier version, or save a note as a Word or PDF file. |
| **Calendar** | Your events (multi-day and repeating), work orders, routine dates, subscriptions, bills, card due dates, rent and loan payments. Imports and exports `.ics` (Google Calendar). |
| **Assets** | **Things** (car, water heater, HVAC… with manuals, warranties, meters and service history), **Stock** (parts you keep on the shelf: receive them, and they go into Spending once; type a part's name on a work order's Equipment tab to use one; a warning when you're down to the last one, because one is none), **Owed** (loans and IOUs both ways), **Rent & lease** (rent you pay, split or not, and rent you collect), **Balances** and **Projection** (below). |
| **Expiring** | Licenses, certifications, passports and insurance, with your own reminder schedule, plus asset warranty end dates. |
| **Purchasing** | Spending by category (amount, vendor, note; click any entry to change it), subscriptions, bills on their due day (each one lands in Spending on its due date, where you put in what you actually paid) and vendors, with every purchase from each. |
| **Reports** | Pick any date range. Charts of net worth, what you have, what's owed, spending by category and month, work orders, and how long the box was on and off. Save as a PDF file (no print dialog) or export CSV. |
| **Weather** | Inside and outside temperature and humidity from small wireless sensor nodes (ESP-NOW), which you can rename. Gaps stay gaps; nothing is filled in. |
| **Map** | Where the box has been while it had a GPS fix, each day (midnight to midnight, in the box's time zone) as its own layer, over a street, satellite or topo map. Saved spots: press the knob on the box's MAP screen to save where you are, or add them here, with notes, pin types, colours and lists; import your Google Maps places (Takeout, My Maps KML). Export GPX and KML. |
| **Setup** | Wi-Fi, clock (24 h or am/pm), box password, your business details and logo, storage, light or dark mode, routine groups, the routine schedule, backups and the full searchable history. Hide the tabs you don't use, put them in your own order and pick the one Logbook opens on; nothing is deleted. Choose the box's screens and their order, and the lines and units on its LOGBOOK and MAP screens. |

### Money, the way a spreadsheet does it

- **Balances:** set up your accounts and credit cards with your own names. Whenever you like, type what each account shows. Logbook adds up the accounts you tick, takes off money you've set aside (a buffer, money you're holding for someone) and what you owe on cards, and gives you **Available**.
- **Projection:** carries Available forward month by month. Bills, subscriptions, rent and loan payment plans fill in on the 1st of each month. Add your own lines, including pay by the day (rate × days). Green is money in, red is money out, blue is money to savings, and every amount shows its + or − sign.

## The physical box

- **Controller:** Adafruit ESP32-S2 Feather.
- **Screen:** 128×128 grayscale OLED (Adafruit 4741). Its screens, in the order you choose: LOGBOOK (your pick of time, Wi-Fi, GPS, battery and time left, box temperature, climate…, plus timers and expiring documents), CHECKLIST, TODAY, NOTES, WORK ORDERS, MAP (speed, distance, position in your units and format) and JOIN. A highlighted line too long for the screen slides sideways so you can read it.
- **Controls:**
  - a clicking rotary knob (Adafruit 5880) that moves one row per click; a press ticks the highlighted checklist item, opens a note or work order, or saves a spot on MAP; holding it turns the screen off;
  - a lit button to change screens and silence alarms. Its light glows dim while the screen sleeps.
- **Buzzer:** a piezo (Adafruit 1739) for kitchen timers, "find my box", and a BNWAS-style move reminder that sounds every few minutes until you walk over and press the button.
- **Also inside:** GPS, a real-time clock and an SD card, all running on battery.

## Updating a box

Updates are a single `.lbk` file, holding the dashboard and the firmware together. Upload it in **Setup › Firmware** with the box password; the box restarts and the page reloads.

## Building from source

- **Arduino ESP32 core:** 3.3.x
- **Board:** `esp32:esp32:adafruit_feather_esp32s2`
- **Partition scheme:** Default 4MB with spiffs

Nothing personal is compiled in. A new box starts with a public hotspot ("Logbook", password shown on its JOIN screen); on first boot it asks for its own box password, your own hotspot password, (optionally) your home Wi-Fi, and its time zone, which starts as your phone's.

---

Made by Aspect Wave LLC.
