//--------------------------------------------------------------------
// DMX ChuGin Test — 8 five-channel fixtures on universe 1
// Tests all protocols: sACN, ArtNet, Serial, Serial Raw
//--------------------------------------------------------------------
// `chuck --chugin:build/Release/DMX.chug DMX-test.ck`
//--------------------------------------------------------------------

8 => int NUM;
5 => int CH;

KBHit kb;

fun void waitForKey(string msg) {
    <<< "\n[" + msg + " — press any key]" >>>;
    kb => now;
    kb.getchar();
}

fun void setAll(DMX dmx, int r, int g, int b, int nw, int ww) {
    for (int i; i < NUM; i++) {
        dmx.channels(1 + i * CH, [r, g, b, nw, ww]);
    }
}

fun void runTests(DMX dmx) {
    // --- Test 1: Chase ---
    waitForKey("Test 1: Chase");
    dmx.blackout();
    dmx.send();
    for (int i; i < NUM; i++) {
        if (i > 0) dmx.channels(1 + (i-1) * CH, [0, 0, 0, 0, 0]);
        dmx.channels(1 + i * CH, [255, 255, 255, 255, 255]);
        dmx.send();
        <<< "  Fixture", i + 1, "ON" >>>;
        500::ms => now;
    }

    // --- Test 2: Single channel set/get ---
    waitForKey("Test 2: channel() set/get");
    dmx.blackout();
    dmx.send();
    dmx.channel(1, 128);
    dmx.send();
    <<< "  Set ch1=128, read back:", dmx.channel(1) >>>;
    dmx.channel(3, 200);
    dmx.send();
    <<< "  Set ch3=200, read back:", dmx.channel(3) >>>;
    <<< "  Unset ch5, read back:", dmx.channel(5) >>>;
    1::second => now;

    // --- Test 3: All red ---
    waitForKey("Test 3: All red");
    setAll(dmx, 255, 0, 0, 0, 0);
    dmx.send();
    2::second => now;

    // --- Test 4: All green ---
    waitForKey("Test 4: All green");
    setAll(dmx, 0, 255, 0, 0, 0);
    dmx.send();
    2::second => now;

    // --- Test 5: All blue ---
    waitForKey("Test 5: All blue");
    setAll(dmx, 0, 0, 255, 0, 0);
    dmx.send();
    2::second => now;

    // --- Test 6: Blackout ---
    waitForKey("Test 6: Blackout");
    dmx.blackout();
    dmx.send();
    <<< "  All channels zeroed, ch1:", dmx.channel(1) >>>;
    1::second => now;

    // --- Test 7: Fade up ---
    waitForKey("Test 7: Fade all to white (2s)");
    dmx.blackout();
    dmx.send();
    for (int i; i < NUM; i++) {
        for (int c; c < CH; c++) {
            dmx.fade(1 + i * CH + c, 255, 2000);
        }
    }
    now => time t;
    while (now < t + 2500::ms) {
        dmx.send();
        23::ms => now;
    }
    <<< "  After fade, ch1:", dmx.channel(1) >>>;

    // --- Test 8: Fade down ---
    waitForKey("Test 8: Fade all to black (2s)");
    for (int i; i < NUM; i++) {
        for (int c; c < CH; c++) {
            dmx.fade(1 + i * CH + c, 0, 2000);
        }
    }
    now => time t2;
    while (now < t2 + 2500::ms) {
        dmx.send();
        23::ms => now;
    }
    <<< "  After fade, ch1:", dmx.channel(1) >>>;

    // --- Test 9: Staggered fade wave ---
    waitForKey("Test 9: Staggered fade wave");
    1 => dmx.debug;
    for (int i; i < NUM; i++) {
        for (int c; c < CH; c++) {
            dmx.fade(1 + i * CH + c, 255, 500);
        }
        now => time tw;
        while (now < tw + 200::ms) {
            dmx.send();
            23::ms => now;
        }
    }
    now => time tw2;
    while (now < tw2 + 2::second) {
        dmx.send();
        23::ms => now;
    }

    0 => dmx.debug;

    // --- Test 10: Live config changes ---
    waitForKey("Test 10: Live name/priority change");
    "New Name" => dmx.name;
    <<< "  Name changed to:", dmx.name() >>>;
    150 => dmx.priority;
    <<< "  Priority changed to:", dmx.priority() >>>;
    setAll(dmx, 255, 255, 255, 255, 255);
    dmx.send();
    1::second => now;
    dmx.blackout();
    dmx.send();
    "DMX Test" => dmx.name;
    100 => dmx.priority;
}

// Protocol names and constants
[DMX.SACN, DMX.ARTNET, DMX.SERIAL, DMX.SERIAL_RAW] @=> int protocols[];
["sACN", "ArtNet", "Serial", "Serial Raw"] @=> string protoNames[];

// Detect serial port for serial protocols
DMX.ports() => string portList;
"" => string serialPort;
if (portList.length() > 0) {
    portList.find(",") => int comma;
    if (comma >= 0) portList.substring(0, comma) => serialPort;
    else portList => serialPort;
}

// Single DMX object reused across protocols so init() properly shuts down
// the previous protocol (e.g. sACN source stops before ArtNet starts).
DMX dmx;

for (int p; p < protocols.size(); p++) {
    <<< "\n============================================" >>>;
    <<< "  PROTOCOL:", protoNames[p] >>>;
    <<< "============================================" >>>;

    if (protocols[p] == DMX.SERIAL || protocols[p] == DMX.SERIAL_RAW) {
        if (serialPort.length() == 0) {
            <<< "  No serial ports found, skipping", protoNames[p] >>>;
            continue;
        }
        <<< "  Using serial port:", serialPort >>>;
    }

    protocols[p] => dmx.protocol;
    1 => dmx.universe;
    "DMX Test" => dmx.name;
    100 => dmx.priority;

    if (protocols[p] == DMX.SERIAL || protocols[p] == DMX.SERIAL_RAW) {
        serialPort => dmx.port;
    }

    if (!dmx.init()) {
        <<< "  Init failed for", protoNames[p], "— skipping" >>>;
        continue;
    }
    <<< "  Connected:", dmx.connected() >>>;

    runTests(dmx);

    // Blackout and send before switching protocols
    dmx.blackout();
    dmx.send();
    100::ms => now;
    <<< "\n  Done with", protoNames[p] >>>;
}

<<< "\n============================================" >>>;
<<< "  ALL TESTS COMPLETE" >>>;
<<< "============================================" >>>;
