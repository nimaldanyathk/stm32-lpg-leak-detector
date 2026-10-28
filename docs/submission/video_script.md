# Demo video: script and shot list

Smart LPG Leak Detector on STM32, 23CSE304 Embedded Systems case study.
Author: Nimal Danyath K (nimaldanyathk).

Target length: about 5 minutes at a calm 150 words per minute (about 4 minutes 45 without the optional LPG with steam scene). Read a little faster, or cut the LPG with steam scene and shorten scene 2, to land under 4 minutes. Landscape 1080p. Record all the video first, record the voice-over separately in a quiet room, then put them together in the edit.

Words in square brackets are numbers you measure on the day. Fill them in before recording the voice-over, or cut that sentence. Do not claim numbers you have not measured.

---

## 1. Before you record

Hardware

- Board powered from the 5 V wall adapter, not a laptop port. Sensors burned in (24 to 48 h once) and powered for at least 10 minutes before the first take.
- Warm-up and clean-air calibration done. Dashboard shows SAFE. Kitchen ventilated, no cooking for 30 minutes before.
- HC-05 paired with the phone (PIN 1234 or 0000). Other Bluetooth terminal apps force-stopped. Chrome has the Nearby devices permission. The https link is open in a Chrome tab: https://nimaldanyathk.github.io/stm32-lpg-leak-detector/phone_app/gas_monitor.html
- Laptop ready with a terminal in `ml/` and some recorded logs in `ml/logs/`, so `python train.py logs` has something to train on.

Test items, laid out on the table in the order they are used

1. Incense stick, already lit and glowing (smoke). Light it first and put the matches away before the lighter comes out.
2. Cup of just-boiled water (steam). Boil it just before the steam scene.
3. Unlit lighter (LPG). Never a flame near this test.
4. A ruler, to show the 10 cm and 20 cm distances on camera.

Between gas tests, open OLED page 2 and wait until all three Rs/R0 values are above 0.90 before the next item. Keep the camera rolling or restart it; the waiting is cut in the edit.

Camera and screen

- Phone or camera on a tripod. Two framings: a top-down wide shot of the breadboard, and a close-up of the OLED with the sensors just in frame. Use macro or tap to focus on the OLED; its text is small.
- Even light from the front or side, no window behind the board, no reflections on the OLED glass.
- Android screen recording for the phone app scene (Quick Settings, Screen record). Laptop screen recording for the schematic and training scenes (QuickTime on Mac, Xbox Game Bar on Windows, or OBS).
- Hands and the test item should be visible in the wide shot. Keep the pointing pen out of the OLED close-up.

---

## 2. Scene table

| # | Time | What to record (shot) | On-screen caption | Voice-over |
|---|---|---|---|---|
| 1 Intro | 0:00 to 0:20 | Wide shot of a kitchen counter: kettle or pot steaming, the detector beside it showing SAFE. If you have an ordinary gas alarm, show it too. | Ordinary gas alarms cannot tell LPG from steam or smoke | Intro paragraph |
| 2 Hardware and how it works | 0:20 to 1:45 | Top-down slow pan of the breadboard; point with a pen at each part as it is named: Black Pill, MQ-6, MQ-2, MQ-135, AHT20, OLED, buzzer, HC-05. Then screen recording of `docs/submission/lpg_detector_schematic_A3.pdf`: full sheet 3 s, zoom on the MQ-6 divider, then the I2C bus. Then OLED close-up: warm-up countdown (speed up 20x), CALIBRATING, SAFE dashboard, and page 6 (ALARM CHECK) with empty boxes. | Lower-thirds with each part name. Then: Ten features every 100 ms. Decision tree on the chip. Vote over 1 s. Fail-safe: MQ-6 below 0.40 for 1 s | Hardware paragraph, then how it works paragraph |
| 3 Incense | 1:45 to 2:05 | Wide shot: incense 15 to 20 cm away, ruler in frame. Cut to OLED page 6: class SMOKE, boxes empty, then page 2: MQ-2 dropped most. | Smoke: SMOKE, no alarm | Incense paragraph |
| 4 Steam | 2:05 to 2:20 | Wide shot: cup of hot water about 10 cm below the sensors. OLED page 6: class STEAM, boxes empty. Page 4 (Climate): humidity rising. | Steam: STEAM, no alarm | Steam paragraph |
| 5 LPG | 2:20 to 2:50 | Wide shot: unlit lighter 10 cm from MQ-6, gas button 2 to 3 s, lighter put down. OLED page 6: boxes fill one by one, MQ-6 bar rises, buzzer at six boxes, GAS LEAK flashing. Keep rolling: fan the air, alarm clears after 5 s of safe readings. | LPG: GAS LEAK. Alarm in [X] s. Clears after 5 s of clean air | LPG paragraph |
| 6 LPG with steam (optional) | 2:50 to 3:05 | Wide shot: cup of hot water under the sensors and the same lighter puff at the same time. OLED page 6: boxes fill, alarm sounds. | Steam does not hide a leak | LPG with steam paragraph |
| 7 Mobile app | 3:05 to 3:30 | Android screen recording: Chrome on the https link, tap Connect HC-05, choose the HC-05, live status and gauges. Repeat a short lighter puff off camera so the alarm panel and votes move. Tap Start log, then Save log file. | Phone app over Bluetooth, Web Serial, https | Mobile app paragraph |
| 8 Other things | 3:30 to 4:00 | Laptop screen recording: terminal, `python train.py logs`, output scrolls (accuracy, confusion matrix), then open `firmware/model.h`. Cut to OLED page 5 (System): inference time, MODEL: TREE. Then a quick KEY hold for the self test and the mute refused during a strong leak if you have the footage. | train.py writes model.h. Inference [N] us. Self test, mute, event log | Other things paragraphs |
| 9 Conclusion | 4:00 to 5:10 | Wide shot: detector on the counter showing SAFE, kettle steaming next to it. While the decision tree is described, cut to `firmware/model.h` on the laptop screen, then back to the wide shot. End card with the repository link. | github.com/nimaldanyathk/stm32-lpg-leak-detector | Conclusion paragraph |

---

## 3. Voice-over script (read this straight through)

Intro

Most kitchen gas alarms use one sensor and one threshold. They go off when someone boils water or fries food, so people switch them off. Then nobody is protected when a real leak happens. This project is an LPG leak detector that can tell the difference between a leak and normal cooking.

What we used and how it works

The detector is built on an STM32F401 Black Pill. Three metal-oxide gas sensors feed the ADC through voltage dividers: an MQ-6 for LPG, an MQ-2 for smoke, and an MQ-135 for alcohol and VOCs. An AHT20 gives temperature and humidity. A small OLED shows the status, a buzzer and an LED raise the alarm, and an HC-05 module streams data to a phone over Bluetooth. The firmware is register-level C with CMSIS, no HAL, and it fits in about nineteen kilobytes of flash.

At power-on the sensors warm up for two minutes, then the detector calibrates for ten seconds in clean air. After that, every 100 milliseconds it reads each sensor as a ratio to its clean-air value and computes ten features: the three sensor ratios, the ratios between sensors, how fast MQ-6 and MQ-2 are changing, and how fast humidity is rising. A decision tree trained on our own recordings runs on the chip and labels the air as clean, LPG, smoke, steam or alcohol. The last ten answers are voted, and the alarm sounds when six of the ten say LPG. A separate fail-safe rule always alarms when MQ-6 drops below forty percent of its clean-air value for one second, whatever the model says. So a strong leak can never be missed.

Incense stick

First, smoke. An incense stick about twenty centimetres from the sensors. MQ-2 reacts first and drops the most. The detector labels the air SMOKE. On the alarm check page the vote boxes stay empty, and the buzzer stays silent. A normal gas alarm would already be sounding.

Steam

Next, steam. A cup of just-boiled water held under the sensors. Humidity rises fast, all three sensors move a little, and the detector says STEAM. Again, no alarm.

LPG

Now real LPG. An unlit lighter, gas button pressed for two seconds, ten centimetres from the sensors. MQ-6 drops hardest, the tree says LPG, and the vote boxes fill one by one. At six boxes the buzzer starts and the screen flashes GAS LEAK. In this run the alarm came [X] seconds after the gas. Once the air is clean again, the alarm clears by itself after five seconds of safe readings.

LPG with steam (optional)

Steam does not hide a leak. With the cup of hot water still under the sensors, the same two seconds of gas. The humidity is high, but the sensor pattern is still LPG, and the alarm sounds again.

Mobile app

The phone app runs in Chrome over Bluetooth using Web Serial, served from an https page. It shows the same status live: the air class, the model's votes, the MQ-6 bar, gas levels, temperature, humidity and a two-minute chart. It beeps on an alarm, sends commands to the board, and records labelled data for training.

Other things

The classifier comes from recordings made with this app. One script splits the data by recording run, reports the accuracy on runs it has never seen, [ACCURACY] percent on our data, and writes the tree as C code that drops straight into the firmware. One inference takes [N] microseconds on the chip.

The detector also has a self test, a sixty-second mute that is refused during a strong leak, wiring fault detection, and an alarm event log over Bluetooth.

Conclusion

To sum up. An ordinary gas alarm cannot tell a leak from cooking, so it is either annoying or switched off. By reading three gas sensors and humidity together, and running a small decision tree on the microcontroller itself, this detector stayed quiet through smoke and steam, and raised the alarm within [X] seconds of real LPG.

We chose a decision tree because it is the right size for this chip. A depth-five tree is at most thirty-one comparisons, so it needs no neural network library, runs in microseconds, and takes only a few hundred bytes of flash. It is also explainable: every branch is a plain rule you can read, for example that LPG pulls MQ-6 down much harder than MQ-135, while smoke hits MQ-2 first. The tree is trained on a laptop with scikit-learn from our own recordings, exported as C code, and we checked that the C version gives the same answer as the Python model on every sample. The fail-safe rule sits on top of it, so the model can never silence a strong leak.

The whole system runs on a 16 megahertz core in about nineteen kilobytes of flash, using low-cost, off-the-shelf parts, and the training pipeline is open, so anyone can record their own data and improve the model. The next steps are testing with calibrated gas concentrations, collecting data over more days and more kitchens, and building an enclosure for long-term use. Thank you for watching.

---

## 4. Recording tips

- Shoot each scene as its own clip, 2 or 3 takes. Name clips by scene number.
- For the test scenes, use the same framing every time so the cuts are clean: wide shot first, then the OLED close-up.
- Keep the OLED on page 6 (ALARM CHECK) during the incense and steam tests; that page is what proves the point. Page 2 is the backup shot that shows which sensor moved.
- Very dense smoke pressed against the sensors can trigger the fail-safe. Keep the incense at 15 to 20 cm; this is a feature, say so if it happens.
- Do the LPG tests last. Ventilate after them. If the alarm does not clear, fan the air and wait; do not restart the board on camera.
- Voice-over: phone 15 to 20 cm from your mouth, quiet room, soft furnishings around you. Read at a calm pace, pause one second between paragraphs, and re-read any sentence you stumble on; the pause makes it easy to cut.
- Measure [X] (time from gas to buzzer) with the video itself: count frames from the lighter click to the first beep.
- Read [N] off OLED page 5 and [ACCURACY] from the train.py output. If you have not trained a model yet, record the train.py scene after you have at least 15 runs per class, or drop the accuracy sentence rather than inventing a number.

## 5. Editing and export

- Any free editor works: CapCut, DaVinci Resolve, iMovie, Clipchamp.
- Put the voice-over on its own track, then trim the video to match it. Speed up the warm-up countdown 20x. Cut all waiting between gas tests.
- Captions as lower-thirds, white text on a dark bar, 2 to 4 seconds each. Use the captions in the scene table.
- Add the schematic zoom in scene 2 as a screen recording, or import the PNG and animate a slow zoom.
- Export: MP4, H.264, 1920x1080, 30 fps, AAC audio 48 kHz. Keep the file under the portal limit (usually 100 to 500 MB; 5 minutes at 8 Mbit/s is about 300 MB, use 5 Mbit/s if you need it smaller).
- File name: `lpg_detector_demo_nimaldanyathk.mp4`.

## 6. What to submit alongside the video

- `docs/submission/lpg_detector_schematic_A3.pdf` (vector, A3 landscape, full sheet) and `lpg_detector_schematic_300dpi.png` if the portal wants an image.
- The repository link, or a zip of the repository if the portal needs a file.
- The measured numbers from the trial table in the README (trials and alarms for each test, detection time, inference time, flash and RAM). Reviewers look for these before anything else.

Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
