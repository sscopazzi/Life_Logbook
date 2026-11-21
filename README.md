# Life Logger
### ▶ [Try the system demo](https://sscopazzi.github.io/Life_Logger/lifelogger-demo.html)
The demo requires no hardware and no sign-up. It runs in your browser with auto-filled made-up data and any changes you make stay in that browser.

---

### ▶ [Buy the system here](https://aspectwave.io/)

Everything I design and sell is **100% open source**; this means, if you would like, you can order the parts yourself and build your own! If you would instead prefer to purchase a ready-built system, you can do so from my website above.

---

### Life Logger is a small physical box that keeps your daily checklist, work orders, calendar, notes, contacts, time, money and location track organized and logged. And it keeps all of it completely to itself with zero outside access to your data.

The demo above is a website so you can try it. The real Life Logger is not a website. It is a battery-powered box with its own screen, a knob and a button. It makes its own Wi-Fi hotspot and serves its dashboard to your phone or laptop over that hotspot (or by passthrough with your home Wi-Fi).

- **Standalone.** No account, no cloud, no app, no subscription and no outside servers. Everything is stored on the box and its SD card. It works with no internet at all. Backup feature to export all your data so you can safely restore after loss.
- **Private.** Nobody can see your data unless you give them the device's password and let them connect.
- **Portable.** Built for life on the go: take it to sea, on vacation, bring it home, to work, and it picks up where it left off.

If you give the box your home Wi-Fi, it uses that only to set its clock and to be reachable at http://lifelogger.local when it's in range of your Wi-Fi, so you don't have to connect directly to the box when you're at home. The only outside requests are the map tiles on the Map tab (street, satellite or topo). Your own phone or laptop fetches them when that device has internet; without internet, the track and your saved spots are drawn on a plain grid.

## The dashboard

The tabs are grouped by what you're doing: **Today**, **Upkeep**, **Business & money** and **Out & about**, with Settings at the right. Rename the groups, drag tabs between them, hide what you don't use, and set the divider's width and colour in Settings.

Everywhere:
- **Search everything** (🔍, or press `/`): work orders, jobs, invoices, clients, vendors, notes, people, assets, stock, events, time and spots in one box.
- **Quick add** (the ▾ next to + Work order): a note, event, purchase with a receipt photo, the job timer, a person or a spot, from any tab.
- **Undo and redo:** Ctrl+Z / Ctrl+Y undo and redo saved changes; the last five are listed too.
- **Suggestions as you type:** every name, place, vendor and category box remembers what you've used ("ada" and Tab fills in "Adafruit"). Settings › Saved lists shows every list to add to or tidy up.

| | |
|---|---|
| **Checklist** | Nested items you can drag to reorder. Ticking an item ticks everything under it, on the web page and on the box. Items can link to work-order steps. Archive a day, then rename, edit or delete it later. |
| **Calendar** | Day view or one to four months. Your events (multi-day and repeating), work orders, routine dates, subscriptions, bills, card due dates, rent and loan payments, warranty and calibration dates. Search by title, place or notes. Imports and exports `.ics` (Google Calendar), and an import can be taken back out in one go. |
| **Notes** | You write in the formatted view: bold, italic, underline, strikethrough, headings, lists, tick boxes and left / centre / right / justified paragraphs, with a word count. Pin notes, show one on the box's screen, colour-code them, and file them under categories with `#word` or `#[two words]`. Link a note to a work order, routine, asset or event; set a reminder; send its lines to the checklist. Archive, search, go back to an earlier version, or save it as a Word or PDF file. Imports Google Keep. |
| **People** | Your contacts: phones, emails, address, organization, tags and notes, plus the work orders and notes that mention each person. Imports Google Contacts (CSV or vCard) and a phone's `.vcf`; exports `.vcf`. |
| **Work Orders** | Overdue, due-soon, open and recently finished work orders, with reminders for bills, credit cards, rent, expiring documents, calibrations, low stock and backups. One-off, routine and business jobs, each with description, steps, findings, previous findings, parts and cost, people and time, attachments, and a full log. Start a job timer from any open work order. |
| **Assets** | **Assets** (car, water heater, instruments… with manuals, warranties, calibration dates, meters and service history) and **Stock** (parts on the shelf by category, with a low-stock list: receive them and they go into Spending once; type a part's name on a work order to use one; a warning at the last one, because one is none). |
| **Expiring** | Licenses, certifications, passports and insurance with your own reminder schedule, plus asset warranties and instrument calibrations. |
| **Jobs** | For paid work: **clients**, **proposals** (steps priced flat, hourly or each, terms, an optional deposit), **invoices** (the same layout, tax, discount, payments, paid deposits credited automatically), the **jobs** themselves, and **Time**: a start / pause / stop timer by job or by your own projects (development, admin…), each line editable in place, with billable time going straight onto an invoice. Save any proposal or invoice as a PDF or Word file with your logo. |
| **Money** | Spreadsheet-style **Balances** (type what each account shows; it gives you **Available**), **Projection** (Available carried forward month by month, with charts), **Owed** (loans and IOUs both ways) and **Rent & Lease** (rent you pay or collect). Every line is typed straight into the table. |
| **Purchasing** | Spending by category, one card per category: type a purchase into its first line, attach a receipt photo (the phone's camera), tick the categories that are business. Subscriptions, bills on their due day, and vendors with their contact details, receipts and invoices paid, and everything bought from them. |
| **Reports** | Any date range: net worth, what you have and owe, spending, work orders, time worked by project and job, business mileage at the IRS rate for each trip's date, and a **business summary** (income by client, business expenses, mileage, net, hours) that becomes your year-end summary. Save as a PDF file or export CSV. |
| **Map** | Where the box has been while it had a GPS fix, each day as its own layer, over a street, satellite or topo map. **Trips** found in the track (or typed in): tick Business, add a purpose, and they add up in Reports. Trips that start or end at home are named Home. Saved spots with notes, pins, colours and lists; import Google Maps places and Timeline; export GPX and KML. |
| **Weather** | Inside and outside temperature and humidity from small wireless sensor nodes (ESP-NOW), which you can rename. Gaps stay gaps; nothing is filled in. |
| **Settings** | Wi-Fi, clock (24 h or am/pm), box password, business and personal info (addresses in the usual shipping-form fields, and which one is home), saved lists, storage, dashboard display (light or dark, calendar day numbers, tabs and their groups), routine groups and schedule, backups and the full searchable history. Choose the box's screens and their order, and the lines and units on its LIFE LOGGER and MAP screens. |

## The physical box

- **Controller:** Adafruit ESP32-S2 Feather.
- **Screen:** 128×128 grayscale OLED (Adafruit 4741). The home screen, LIFE LOGGER, shows your pick of status lines (time, Wi-Fi, GPS, battery and time left, box temperature, climate…), any timers and alarms, and a menu of the other screens in the order you choose: CHECKLIST, TODAY, NOTES, WORK ORDERS, JOB TIMER, EXPIRING, MAP (speed, distance, position, and a compass rose with your course while moving), your BUSINESS and PERSONAL cards (to hold up when someone asks for your details) and JOIN (how to connect, and which phones and computers have the dashboard open). Long lines wrap with a hyphen; a highlighted line too long for the screen slides sideways.
- **Controls:**
  - a clicking rotary knob (Adafruit 5880) that moves one row per click; a press opens the screen picked in the menu, ticks a checklist item, opens a note, work order or event, starts / pauses / stops the job timer, or on MAP ticks **Work trip** (tick it in the driveway and that drive goes to the dashboard as a work trip) or saves a spot; holding it turns the screen off;
  - a lit button: back, and it silences alarms. Its light glows dim while the screen sleeps.
- **Buzzer:** a piezo (Adafruit 1739) for kitchen timers, "find my box", and a BNWAS-style move reminder that sounds every few minutes until you walk over and press the button (put it in the other room and force yourself to stand up and walk every x minutes).
- **Also inside:** GPS, a real-time clock and an SD card, all running on battery.

The demo shows the box as it is with the coming firmware update: the menu on the home screen, JOB TIMER, the Work trip tick, connected devices on JOIN, EXPIRING, the personal card, the compass rose and hyphenated wrapping arrive on real boxes with that release.

## Updating a box

Updates are a single `.lbk` file (the dashboard and the firmware together). Upload it in **Settings › Firmware** with the box password; the box restarts and the page reloads.

**All future firmware updates are 100% free** and will be released on this GitHub page.

## Building from source

I am in the process of finishing the build manual. Please check back, or contact me via my website below/above.

- **Arduino ESP32 core:** 3.3.x
- **Board:** `esp32:esp32:adafruit_feather_esp32s2`
- **Partition scheme:** Default 4MB with spiffs

Nothing personal is compiled in. A new box starts with a public hotspot ("Life Logger", password shown on its JOIN screen); on first boot it asks for its own box password, your own hotspot password, (optionally) your home Wi-Fi, and its time zone, which starts as your phone's.

---

Made by Aspect Wave LLC - [Website](https://aspectwave.io/)
